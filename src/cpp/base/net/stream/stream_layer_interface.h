// 字节流层的接口：夹在 socket 与 codec 之间，把线上字节变成明文（握手、解密），再把明文变回线上字节（加密）。
// TLS 是目前唯一的真实现（base/net/tls/）；breeze_net 里不出现任何 TLS 类型，TLS 库只实现这里的两个接口。设计见方案 §3.3。
//
// 一条连接最多一个层；层与 codec 互相不知道对方：`tls` = 层 + LengthPrefix，`wss` = 层 + WebSocket。
// 层的握手与 codec 的握手都完成，才回调 OnConnected（见 stream_connection.h 的握手阶段）。
//
// 线程：每条连接一个实例，只在所属 loop 的线程上调用，实现不必线程安全。IStreamLayerFactory 是每端点一个、被多条连接
// 共享、多个 loop 的线程并发调用 Create，实现必须线程安全，且创建出来的对象不得依赖创建它的线程。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace breeze
{

enum class LayerRole : std::uint8_t
{
    Server,
    Client,
};

enum class LayerStatus : std::uint8_t
{
    Ok,
    HandshakeDone, // 层的握手完成。只给一次：由 Start 或某次 Unwrap 返回（可以与本次交付的明文同一次调用）
    PeerShutdown,  // 对端发来了关闭通知（TLS 的 close_notify）：此后不会再有明文
    Error,         // 出错（握手失败、证书不对、record 损坏）：连接随即关闭，之后只会析构；详细原因用 error_code() 进日志
};

// Unwrap 把解出的明文交给连接：视图是可写的（codec 会在原地改写完整帧），只在这次调用期间有效；每块非空，
// 一次 Unwrap 可以交付任意多块。返回 false 表示连接已经不可用（回调里被关闭了）：层应尽快停止交付。
using PlainSink = std::function_ref<bool(std::span<std::byte>)>;

// 一次 Wrap 的明文上限（一个 TLS record 的明文上限）。连接按这个大小切块再调 Wrap，层不必处理更大的输入。
inline constexpr std::size_t kLayerMaxWrapBytes = 16 * 1024;

class IStreamLayer
{
public:
    virtual ~IStreamLayer() = default;

    // 连接登记 epoll 之后调用一次。客户端层在这里产出握手的第一段字节（TLS 的 ClientHello，进 PendingWire）。
    // 可以直接返回 HandshakeDone（没有握手的层）。
    virtual LayerStatus Start() = 0;

    // 线上字节 → 明文。wire 是连接的读缓冲，**必须一次吃完**（层不得把它的视图留到调用之后，也不得留下"下次再解"的
    // 已收字节让连接去补发事件）；握手期的字节被层自己消耗，不产生明文。层可以原地改写 wire。
    //
    // 层**只在握手完成之后才交付明文**；握手完成与明文可以在同一次 Unwrap 里（TLS 客户端发完 Finished 立刻发数据）。
    // 此时返回 HandshakeDone 可以晚于 sink 的调用——连接把"交付明文"也当作握手完成的信号，保证 OnConnected 先于第一条消息。
    // sink 里业务可以直接 Send，所以会重入 Wrap / Shutdown：层必须允许在 sink 调用期间被调用这两个。
    virtual LayerStatus Unwrap(std::span<std::byte> wire, PlainSink sink) = 0;

    // 明文 → 线上字节，进内部发送缓冲（PendingWire）。plain 不超过 kLayerMaxWrapBytes。握手完成之前连接不会调它。
    virtual LayerStatus Wrap(std::span<const std::byte> plain) = 0;

    // 等着写进 socket 的字节（握手字节、密文）。连接写出之后用 ConsumeWire 告诉层写了多少。
    virtual std::span<const std::byte> PendingWire() const noexcept = 0;
    virtual void ConsumeWire(std::size_t n) noexcept = 0;

    // 优雅关闭：把关闭通知（TLS 的 close_notify）放进 PendingWire。连接在写队列排空之后调用，幂等。
    virtual LayerStatus Shutdown() = 0;

    // 最近一次 Error 的底层错误码，只给日志用：值域是各层自己的（BoringSSL 的错误码会与 errno 重叠），
    // 连接不会把它放进 sys_errno，也不会拿它去判断能不能重试。
    virtual int error_code() const noexcept = 0;
};

class IStreamLayerFactory
{
public:
    virtual ~IStreamLayerFactory() = default;
    // 不得返回空：创建失败是编程错误（悄悄退化成明文比崩溃更糟）。
    virtual std::unique_ptr<IStreamLayer> Create(LayerRole role) const = 0;
};

} // namespace breeze
