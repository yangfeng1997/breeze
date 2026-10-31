// 分帧编解码接口。设计见 docs/网络库设计.md §4.4 / §4.7。
//
// FrameCodec 是**每连接一个实例**、有状态（半包、序号），不能被多线程共享；由 FrameCodecFactory 在连接建立时创建。
// 库只内置 LengthPrefixCodec（内部链路与测试用）；客户端协议（魔数、序号、加密、压缩）由应用层实现自己的 FrameCodec。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace breeze
{

inline constexpr std::size_t kMaxFrameHeaderSize = 16;

enum class DecodeResult : std::uint8_t
{
    NeedMore, // 缓冲里没有完整帧，不消耗任何字节
    Ok,       // 解出一帧并已交给 sink；调用方继续调用，直到 NeedMore
    Error,    // 帧非法：调用方关闭连接，不尝试重新同步
};

// 非拥有的可调用对象视图，只在 Decode 调用期间有效；不分配。
using FrameSink = std::function_ref<void(std::span<const std::byte>)>;

class FrameCodec
{
public:
    virtual ~FrameCodec() = default;

    // 从 data 开头最多解出一帧；一次一帧，调用方才能在两帧之间检查连接是否已在回调里被关闭。
    //   Ok       解出一帧并已交给 sink；used = 这一帧占的字节数（帧头 + payload），调用方据此推进
    //   NeedMore data 里没有完整帧；used = 0
    //   Error    帧非法；used = 0，调用方关闭连接，不尝试重新同步
    // payload 的视图指向 data 内部，只在 sink 调用期间有效。接口只吃 span、不碰 Buffer，
    // 所以调用方可以直接从线程本地读缓冲分帧，只有半个帧才需要拷进每连接的缓冲。
    virtual DecodeResult Decode(std::span<const std::byte> data, FrameSink sink, std::size_t& used) = 0;

    // 把帧头写进 header_out（不超过 kMaxFrameHeaderSize），返回写入的字节数。
    // 只依赖 payload 长度，不依赖内容，所以同一块 payload 发给多条连接时帧头仍可带逐连接的序号。
    // payload_len 超过上限返回 0。
    virtual std::size_t EncodeHeader(std::size_t payload_len, std::span<std::byte, kMaxFrameHeaderSize> header_out) = 0;
};

// Create 会被多个 loop 的线程并发调用（accept 在一个 loop，连接可能落在另一个 loop 上），实现必须线程安全。
class FrameCodecFactory
{
public:
    virtual ~FrameCodecFactory() = default;
    virtual std::unique_ptr<FrameCodec> Create() const = 0;
};

} // namespace breeze
