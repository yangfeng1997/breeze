// 分帧编解码接口。设计见 docs/网络库设计.md §4.4 / §4.7。
//
// IFrameCodec 是**每连接一个实例**、有状态（半包、序号），不能被多线程共享；由 IFrameCodecFactory 在连接建立时创建。
// 库只内置 LengthPrefixCodec（内部链路与测试用）；客户端协议（魔数、序号、加密、压缩）由应用层实现自己的 IFrameCodec。
//
// 有状态的协议（WebSocket：HTTP 升级、Ping / Pong / Close、客户端掩码）用 Decode / EncodeHeader 之外那几个有默认实现的
// 虚函数：traits / Attach / OnClosing / EncodePayload，以及 ICodecHost（codec_host_interface.h）。无状态的 codec 一个都不用管。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace breeze
{

class ICodecHost;

inline constexpr std::size_t kMaxFrameHeaderSize = 16;

enum class DecodeResult : std::uint8_t
{
    NeedMore, // 缓冲里没有完整帧，不消耗任何字节
    Ok,       // 解出一帧并已交给 sink；调用方继续调用，直到 NeedMore
    Error,    // 帧非法：调用方关闭连接，不尝试重新同步
    Consumed, // 消耗了 used 字节但没有消息（握手、控制帧、非末尾分片）；调用方照常推进并继续调用
};

// 建立连接时读一次。
struct CodecTraits
{
    // codec 自己有握手（WebSocket 升级）：OnConnected 要等它调 ICodecHost::HandshakeDone() 之后才回调。
    bool needs_handshake = false;
    // codec 要改写出站 payload（WebSocket 客户端的掩码）：每条消息先拷贝一份再交给 EncodePayload，
    // 所以这样的连接不能零拷贝共享 payload。服务端的 codec 返回 false，payload 仍然零拷贝共享。
    bool encodes_payload = false;
};

// 非拥有的可调用对象视图，只在 Decode 调用期间有效；不分配。
using FrameSink = std::function_ref<void(std::span<const std::byte>)>;

class IFrameCodec
{
public:
    virtual ~IFrameCodec() = default;

    // 从 data 开头最多解出一帧；一次一帧，调用方才能在两帧之间检查连接是否已在回调里被关闭。
    //   Ok       解出一帧并已交给 sink；used = 这一帧占的字节数（帧头 + payload），调用方据此推进
    //   Consumed 消耗了 used 字节但没有消息（握手 / 控制帧 / 非末尾分片）；调用方推进 used 后继续调用
    //   NeedMore data 里没有完整帧；used = 0
    //   Error    帧非法；used = 0，调用方关闭连接，不尝试重新同步
    // payload 的视图指向 data 内部，只在 sink 调用期间有效。接口只吃 span、不碰 Buffer，
    // 所以调用方可以直接从线程本地读缓冲分帧，只有半个帧才需要拷进每连接的缓冲。
    //
    // data 是可写视图（连接自己的缓冲）。契约：**只有在一帧完整、used 已定之后才可以改写这一帧的字节**
    // （WebSocket 服务端对客户端帧原地去掩码）；NeedMore / Error 不得改动——同一批字节会再次传入。
    virtual DecodeResult Decode(std::span<std::byte> data, FrameSink sink, std::size_t& used) = 0;

    // 把帧头写进 header_out（不超过 kMaxFrameHeaderSize），返回写入的字节数。
    // 只依赖 payload 长度，不依赖内容，所以同一块 payload 发给多条连接时帧头仍可带逐连接的序号。
    // payload_len 超过上限返回 0。
    virtual std::size_t EncodeHeader(std::size_t payload_len, std::span<std::byte, kMaxFrameHeaderSize> header_out) = 0;

    // ---- 以下全部有默认实现（空操作）----

    virtual CodecTraits traits() const noexcept { return {}; }

    // 连接建立时调用一次（登记 epoll 之后、OnConnected 之前）。host 活得与连接一样久，codec 可以保存它。
    // WebSocket 客户端在这里 SendRaw 升级请求。
    virtual void Attach(ICodecHost& /*host*/) {}

    // 连接开始优雅关闭（CloseGracefully）时调用一次。WebSocket 在这里发 Close(1001)。
    virtual void OnClosing() {}

    // 仅 CodecTraits::encodes_payload：EncodeHeader 之后立即调用，对出站 payload 的**拷贝**原地改写。
    virtual void EncodePayload(std::span<std::byte> /*payload*/) noexcept {}
};

// Create 会被多个 loop 的线程并发调用（accept 在一个 loop，连接可能落在另一个 loop 上），实现必须线程安全。
class IFrameCodecFactory
{
public:
    virtual ~IFrameCodecFactory() = default;
    virtual std::unique_ptr<IFrameCodec> Create() const = 0;
};

} // namespace breeze
