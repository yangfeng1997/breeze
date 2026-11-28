// 字节流连接（TCP 与 UDS 共用）的基类：全部数据成员与共用的生命周期（握手 / 关闭 / 暂停 / 优雅关闭 / 分帧）。
// 设计见 docs/网络库设计.md §4.4 / §4.6 / §4.7 / §4.8 与方案 §3（握手阶段）。
//
// 读写路径在子类里，建立连接时按需选定（StreamConnection::Create），读写热路径里没有 if：
//   PlainStreamConnection    明文：fd 直接读写（plain_stream_connection.h，实现与本类同在 stream_connection.cpp）
//
// 内部类型：业务回调里拿到的是 IConnection&。创建只经 Create（EventLoop 不认识它：core 不依赖 stream）。
//
// 成员顺序是性能决定的（见下面成员处的注释）：IConnection 接口没有数据成员、继承链上只有一个 vptr，
// 所有数据都在本类里、子类不在热区加字段，所以热区布局与拆成子类之前一致。
//
// 握手阶段：层（TLS）与 codec（WebSocket 升级）各自可能有握手，**全部完成才回调 OnConnected**；此前连接已经登记在 epoll 里、
// 收发字节，但业务看不见它。握手期失败的连接没有 OnConnected 也就没有 OnClosed，只记统计，并通过 IHandshakeObserver
// 通知 Connector。没有握手的连接（明文 + LengthPrefixCodec）Establish 里直接回调 OnConnected，与以前完全一样。

#pragma once

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <type_traits>

#include "base/buffer/buffer.h"
#include "base/buffer/shared_buffer.h"
#include "base/net/codec/codec_host_interface.h"
#include "base/net/codec/frame_codec_interface.h"
#include "base/net/connection/connection_context.h"
#include "base/net/connection/connection_handle.h"
#include "base/net/connection/connection_handler_interface.h"
#include "base/net/connection/connection_interface.h"
#include "base/net/connection/connection_options.h"
#include "base/net/connection/send_result.h"
#include "base/net/endpoint/peer_info.h"
#include "base/net/loop/connection_table.h"
#include "base/net/loop/io_completion.h"
#include "base/net/socket/unique_fd.h"
#include "base/net/stream/internal/handshake_observer_interface.h"
#include "base/net/stream/internal/lazy_queue.h"
#include "base/net/stream/stream_layer_interface.h"

namespace breeze
{

class EventLoop;

// 建立连接时的可选项。都不给 = 明文 + 没有握手观察者，与以前完全一样。
struct StreamConnectionExtras
{
    // 只有 Connector 传（握手期失败要触发重连）；服务端传空。
    IHandshakeObserver* observer = nullptr;
    // 非空 = 这条连接走分层（TLS）：用它创建层。调用方保证它活得比连接久。
    const IStreamLayerFactory* layer = nullptr;
    LayerRole role = LayerRole::Server;
};

class StreamConnection : public IConnection
{
public:
    // 在 loop 的连接表里占一个槽并建出连接（归 loop 持有，回收时销毁），调用方随后调 Establish()。
    // 调用方须在 loop 线程上，且已 table().TryReserve() 成功；
    // codec 由调用方（在本 loop 线程上）从 IFrameCodecFactory 创建。
    // 选哪个子类：有层，或 codec 要改写出站 payload（traits().encodes_payload）→ LayeredStreamConnection；否则
    // PlainStreamConnection（明文快路径）。
    static StreamConnection* Create(EventLoop& loop,
                                    UniqueFd fd,
                                    PeerInfo peer,
                                    IConnectionHandler& handler,
                                    std::unique_ptr<IFrameCodec> codec,
                                    const ConnectionOptions& options,
                                    const StreamConnectionExtras& extras = {});

    ~StreamConnection() override;

    // 语义见 IConnection 接口。读写（Send / OnEvents）由子类实现。
    void Close() override;
    void CloseGracefully(std::chrono::milliseconds timeout) override;
    void PauseRead() override;
    void ResumeRead() override;
    void SetContext(std::unique_ptr<ConnectionContext> ctx) noexcept override { ctx_ = std::move(ctx); }
    bool IsConnected() const noexcept override { return state_ == State::Connected; }
    bool IsPaused() const noexcept override { return paused_; }
    const PeerInfo& peer() const noexcept override { return peer_; }
    ConnectionHandle handle() const noexcept override { return handle_; }

    // 装配代码（StreamServer / StreamClient）在 Create 之后调用：登记到 epoll；没有握手就立即回调 OnConnected，
    // 有握手则开始握手（OnConnected 等全部握手完成再回调）。
    // 登记失败（新 fd 的 epoll_ctl 遇到 ENOSPC / ENOMEM）返回 false：只关这一条，没有任何回调，进程继续。
    [[nodiscard]] bool Establish();

protected:
    StreamConnection(EventLoop& loop,
                     UniqueFd fd,
                     PeerInfo peer,
                     IConnectionHandler& handler,
                     std::unique_ptr<IFrameCodec> codec,
                     const ConnectionOptions& options,
                     ConnectionTable::Slot& slot,
                     std::uint32_t slot_index,
                     ConnectionHandle handle);

    enum class State : std::uint8_t
    {
        Connected,
        Disconnecting, // 优雅关闭中：不再接受 Send，仍读，等写队列排空与对端关闭
        Disconnected,
        ClosingIo, // 最终层字节仍在途：停止解帧与新发送，完成或短超时后转 Disconnected
    };

    // 待发送的一帧：内联帧头 + 共享的 payload；offset 是已经写出的字节数（帧头 + payload 合计）。
    // 队列里每一项的 payload.size() 都已计入槽的 pending_bytes，整条写完（或连接回收）时才减。
    struct Pending
    {
        std::array<std::byte, kMaxFrameHeaderSize> header{};
        std::uint8_t header_len = 0;
        SharedBuffer payload;
        std::size_t offset = 0;

        std::size_t Total() const noexcept { return header_len + payload.size(); }
    };

    // flags_ 的位。**只放冷路径状态**：热路径要读的放独立字段（现有的几个 bool 也不迁进来，迁了会改明文热函数的函数体）。
    enum class Flag : std::uint16_t
    {
        LayerHandshaking = 1u << 0, // 层的握手还没完成
        CodecHandshaking = 1u << 1, // codec 的握手还没完成
        ReasonPinned = 1u << 2,     // CloseAfterFlush 已把关闭原因与 code 记进 close_reason_ / close_errno_，收尾时用它们
        HandshakeStarted = 1u << 3, // 握手阶段开始过（只置不清）：NotifyClosed 据此区分"握手失败"与"登记 epoll 失败"
    };
    bool Has(Flag f) const noexcept { return (flags_ & static_cast<std::uint16_t>(f)) != 0; }
    void Set(Flag f) noexcept { flags_ |= static_cast<std::uint16_t>(f); }
    void Clear(Flag f) noexcept { flags_ &= static_cast<std::uint16_t>(~static_cast<std::uint16_t>(f)); }
    bool HandshakePending() const noexcept
    {
        return (flags_ & static_cast<std::uint16_t>(static_cast<std::uint16_t>(Flag::LayerHandshaking) |
                                                    static_cast<std::uint16_t>(Flag::CodecHandshaking))) != 0;
    }

    // 以下由 EventLoop 经 IConnection 接口调用。
    ConnectionContext* context() const noexcept override { return ctx_.get(); }
    void CloseFromTask(CloseReason reason) override { CloseWith(reason, 0); }
    void CloseIdle() override { CloseWith(CloseReason::IdleTimeout, 0); }
    void NotifyClosed() override;
    std::uint32_t slot_index() const noexcept override { return slot_index_; }

    // ---- 子类的钩子（都在冷路径上：每条连接调用几次，不在每条消息的路径上）----
    // Establish 里、Attach 之后调用：分层子类在这里启动层（客户端产出 ClientHello）。默认什么也不做。
    virtual void OnEstablish() {}
    // 写队列排空、要结束写方向时：默认直接 shutdown(SHUT_WR)；分层子类先让层发 close_notify 再 shutdown。
    virtual void FinishWrites() { ShutdownWrite(); }
    // ICodecHost::SendRaw 的实现：不经 EncodeHeader 写原始字节，与 Send 保序。连接已关闭时是空操作。
    virtual void WriteRaw(std::span<const std::byte> bytes) = 0;
    // 只由 completion adapter 调用；epoll 子类不进入这两个钩子。
    virtual void OnIoRead(std::span<std::byte>, int) {}
    virtual void OnIoWrite(std::size_t, int) {}
    virtual void StartIoWrite() {}
    bool SubmitIoWrite(std::span<const iovec> buffers);
    bool SubmitIoOwnedWrite(SharedBuffer bytes);
    bool SubmitIoOwnedWriteV(std::span<const std::byte> prefix, SharedBuffer payload);
    bool CompleteIoWrite(std::size_t bytes, int error);

    // 一个握手来源完成：清它的位，全部清完就回调 OnConnected。幂等；连接已关闭则忽略。
    void HandshakeSourceDone(Flag source);
    // 回调 OnConnected（握手全部完成，或本来就没有握手）。
    void NotifyConnected();

    // 从 data 开头起把能解的帧都解出来，返回消耗的字节数。data 必须是连接自己的可写缓冲（codec 会在原地改写完整帧）。
    std::size_t DecodeAll(std::span<std::byte> data);
    // DecodeAll 的 always_inline 版本，定义在 stream_connection.cpp，只有那里的 PlainStreamConnection::HandleRead 用
    // （明文读路径不多一次调用；见定义处的说明）。别的编译单元用上面的 DecodeAll。
    [[gnu::always_inline]] inline std::size_t DecodeAllInline(std::span<std::byte> data);
    // codec 返回的 Consumed 没有推进（used 为 0 或超出）：记日志并按协议错误关闭。冷路径，见 DecodeAll 里的说明。
    void CloseOnBadConsumed(std::size_t used, std::size_t avail);
    void SetWriteInterest(bool want);
    void ApplyInterest();
    void ShutdownWrite();
    // 对端关闭（EOF / HUP）的原因：我们先发起了优雅关闭时，对端回应的 FIN 是正常结束，算 LocalClose。
    CloseReason PeerCloseReason() const noexcept
    {
        return state_ == State::Disconnecting ? CloseReason::LocalClose : CloseReason::PeerClosed;
    }
    // ReasonPinned 置位时，PeerClosed / LocalClose 收尾会换成记下的原因与 code（其它原因，比如 IoError，照常）。
    void CloseWith(CloseReason reason, int sys_errno);
    // ICodecHost::CloseAfterFlush 的实现。
    void CloseAfterFlush(CloseReason reason, int code);

    // codec 看到的宿主：把 ICodecHost 的四个调用转给本连接。放在冷区，不进热区、也不给连接加第二个 vptr 的基类。
    class CodecHostAdapter final : public ICodecHost
    {
    public:
        explicit CodecHostAdapter(StreamConnection& c) noexcept
            : c_(c)
        {
        }
        void SendRaw(std::span<const std::byte> bytes) override { c_.WriteRaw(bytes); }
        void HandshakeDone() override { c_.HandshakeSourceDone(Flag::CodecHandshaking); }
        void CloseAfterFlush(CloseReason reason, int code) override { c_.CloseAfterFlush(reason, code); }
        void SetContext(std::unique_ptr<ConnectionContext> ctx) override { c_.SetContext(std::move(ctx)); }
        const PeerInfo& peer() const noexcept override { return c_.peer_; }

    private:
        StreamConnection& c_;
    };

    // 第二个接口的 vptr 留在冷区，不能通过多继承把 in_ 的偏移从 72 字节后移。
    class CompletionAdapter final : public IStreamCompletion
    {
    public:
        explicit CompletionAdapter(StreamConnection& c) noexcept : c_(c) {}
        void OnIoRead(std::span<std::byte> data, int error) override { c_.OnIoRead(data, error); }
        void OnIoWrite(std::size_t bytes, int error) override { c_.OnIoWrite(bytes, error); }

    private:
        StreamConnection& c_;
    };

    // 成员顺序是性能决定的：每条消息都碰的热字段集中在前（两条缓存行内），冷的（对端信息、句柄、上下文、定时器、关闭原因）放后面。
    // 10 万连接时每连接多碰一条缓存行就是可测量的代价（docs/网络库设计.md §9.1 / §9.3）。改动时用 sizeof 与 offsetof
    // 核对。新增状态一律放冷区（末尾），子类也不得在热区加字段。
    EventLoop& loop_;
    UniqueFd fd_;
    IConnectionHandler& handler_;
    std::unique_ptr<IFrameCodec> codec_;
    ConnectionTable::Slot& slot_; // 热字段（状态、pending_bytes）在连接表里，别的线程不碰本对象就能读
    const std::uint32_t slot_index_;
    const std::uint32_t idle_timeout_ms_; // 0 = 不参加空闲检测
    const std::uint64_t high_water_;
    State state_ = State::Connected;
    bool registered_ = false;         // 是否已登记到 epoll
    bool want_write_ = false;         // epoll 里是否带 EPOLLOUT
    bool paused_ = false;             // PauseRead 生效中（epoll 里不带 EPOLLIN）
    bool idle_registered_ = false;    // 在 EventLoop 的空闲检测里登记过
    bool notified_connected_ = false; // 回调过 OnConnected 才会回调 OnClosed
    Buffer in_;
    // 发送队列：惰性创建、排空即释放。几乎所有连接的队列永远是空的，而 libstdc++ 的空 std::deque 一构造就是 2 次 new、
    // 576 字节，占每连接约 62% 的内存；现在空队列只占一个指针。empty() 只读这个指针。
    LazyQueue<Pending> out_;

    PeerInfo peer_;
    const ConnectionHandle handle_;
    std::unique_ptr<ConnectionContext> ctx_;
    // 握手超时与 CloseGracefully / CloseAfterFlush 的超时共用这一个定时器（EventLoop::TimerId），0 = 没有：
    // 握手期间不会优雅关闭（CloseGracefully 对未握手完成的连接等于 Close），两者不会同时存在。
    std::uint64_t timer_ = 0;
    CloseReason close_reason_ = CloseReason::LocalClose;
    int close_errno_ = 0;
    // ---- 以下是握手阶段新增的冷区状态 ----
    IHandshakeObserver* observer_ = nullptr; // 只有 Connector 的连接才有
    CodecHostAdapter host_{*this};
    const std::uint32_t handshake_timeout_ms_;
    std::uint16_t flags_ = 0; // Flag 的位
    CompletionAdapter completion_{*this};
    IoRegistration io_registration_ = 0; // 非零时收发都走真正的 completion，fd 不登记 readiness
    std::size_t io_write_bytes_ = 0; // 本次在途提交的上限；引擎持有提交字节直到真实完成
    bool io_write_pending_ = false;
};

} // namespace breeze
