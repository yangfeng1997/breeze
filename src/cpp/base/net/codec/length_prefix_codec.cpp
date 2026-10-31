#include "length_prefix_codec.h"

#include <cassert>
#include <limits>

namespace breeze
{

LengthPrefixCodec::LengthPrefixCodec(std::size_t max_frame)
    : max_frame_(max_frame)
{
    assert(max_frame <= std::numeric_limits<std::uint32_t>::max() && "max_frame 超过 u32 能表示的长度");
}

DecodeResult LengthPrefixCodec::Decode(std::span<const std::byte> data, FrameSink sink, std::size_t& used)
{
    used = 0;
    if (data.size() < kHeaderSize)
        return DecodeResult::NeedMore;

    const std::size_t len = (static_cast<std::size_t>(data[0]) << 24) | (static_cast<std::size_t>(data[1]) << 16) |
                            (static_cast<std::size_t>(data[2]) << 8) | static_cast<std::size_t>(data[3]);
    // 先于等待 payload 检查上限：恶意的超大长度不会让缓冲无限增长。
    if (len > max_frame_)
        return DecodeResult::Error;
    if (data.size() - kHeaderSize < len)
        return DecodeResult::NeedMore;

    sink(data.subspan(kHeaderSize, len));
    used = kHeaderSize + len;
    return DecodeResult::Ok;
}

std::size_t LengthPrefixCodec::EncodeHeader(std::size_t payload_len,
                                            std::span<std::byte, kMaxFrameHeaderSize> header_out)
{
    if (payload_len > max_frame_)
        return 0;
    header_out[0] = static_cast<std::byte>(payload_len >> 24);
    header_out[1] = static_cast<std::byte>(payload_len >> 16);
    header_out[2] = static_cast<std::byte>(payload_len >> 8);
    header_out[3] = static_cast<std::byte>(payload_len);
    return kHeaderSize;
}

} // namespace breeze
