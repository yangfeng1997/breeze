// 内置编解码：4 字节大端长度（只含 payload，不含自身）+ payload。用于内部链路与测试。

#pragma once

#include <cstddef>
#include <memory>

#include "base/net/codec/frame_codec_interface.h"

namespace breeze
{

class LengthPrefixCodec final : public IFrameCodec
{
public:
    static constexpr std::size_t kHeaderSize = 4;
    // 默认上限要小：它同时是读 Buffer 的上界，10 万连接都按它算内存。内部链路按需调大。
    static constexpr std::size_t kDefaultMaxFrame = 64 * 1024;

    // max_frame 是 payload 的上限，不得超过 UINT32_MAX。
    explicit LengthPrefixCodec(std::size_t max_frame = kDefaultMaxFrame);

    DecodeResult Decode(std::span<const std::byte> data, FrameSink sink, std::size_t& used) override;
    std::size_t EncodeHeader(std::size_t payload_len, std::span<std::byte, kMaxFrameHeaderSize> header_out) override;

private:
    std::size_t max_frame_;
};

class LengthPrefixCodecFactory final : public IFrameCodecFactory
{
public:
    explicit LengthPrefixCodecFactory(std::size_t max_frame = LengthPrefixCodec::kDefaultMaxFrame)
        : max_frame_(max_frame)
    {
    }

    std::unique_ptr<IFrameCodec> Create() const override { return std::make_unique<LengthPrefixCodec>(max_frame_); }

private:
    std::size_t max_frame_;
};

} // namespace breeze
