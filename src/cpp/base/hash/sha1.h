// SHA-1（RFC 3174）。**只给 WebSocket 握手用**：RFC 6455 规定 Sec-WebSocket-Accept 就是 base64(SHA-1(key + GUID))，
// 一条连接算一次，不在热路径上，所以不追求速度，也不依赖 TLS 库（明文 ws:// 不该背 BoringSSL）。
// SHA-1 已经不安全：不要把它用在任何需要抗碰撞的地方。
//
// 只有头文件。用法：一次性 Sha1::Hash(bytes)，或 Update 若干次再 Final。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace breeze
{

class Sha1
{
public:
    static constexpr std::size_t kDigestSize = 20;
    static constexpr std::size_t kBlockSize = 64;
    using Digest = std::array<std::uint8_t, kDigestSize>;

    Sha1() noexcept { Reset(); }

    void Reset() noexcept
    {
        h_ = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
        total_bytes_ = 0;
        buffered_ = 0;
    }

    void Update(std::span<const std::byte> data) noexcept
    {
        const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
        std::size_t n = data.size();
        total_bytes_ += n;
        if (buffered_ != 0)
        {
            const std::size_t take = (kBlockSize - buffered_ < n) ? kBlockSize - buffered_ : n;
            for (std::size_t i = 0; i < take; ++i)
                buf_[buffered_ + i] = p[i];
            buffered_ += take;
            p += take;
            n -= take;
            if (buffered_ < kBlockSize)
                return;
            Block(buf_.data());
            buffered_ = 0;
        }
        for (; n >= kBlockSize; p += kBlockSize, n -= kBlockSize)
            Block(p);
        for (std::size_t i = 0; i < n; ++i)
            buf_[i] = p[i];
        buffered_ = n;
    }
    void Update(std::string_view s) noexcept { Update(std::as_bytes(std::span(s))); }

    // 取摘要；之后对象回到初始状态，可以接着算下一份。
    Digest Final() noexcept
    {
        const std::uint64_t bits = total_bytes_ * 8;
        buf_[buffered_++] = 0x80;
        if (buffered_ > kBlockSize - 8)
        {
            for (; buffered_ < kBlockSize; ++buffered_)
                buf_[buffered_] = 0;
            Block(buf_.data());
            buffered_ = 0;
        }
        for (; buffered_ < kBlockSize - 8; ++buffered_)
            buf_[buffered_] = 0;
        for (int i = 0; i < 8; ++i)
            buf_[kBlockSize - 1 - static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(bits >> (8 * i));
        Block(buf_.data());

        Digest out{};
        for (std::size_t i = 0; i < 5; ++i)
            for (std::size_t j = 0; j < 4; ++j)
                out[i * 4 + j] = static_cast<std::uint8_t>(h_[i] >> (24 - 8 * j));
        Reset();
        return out;
    }

    static Digest Hash(std::span<const std::byte> data) noexcept
    {
        Sha1 s;
        s.Update(data);
        return s.Final();
    }
    static Digest Hash(std::string_view s) noexcept { return Hash(std::as_bytes(std::span(s))); }

private:
    static constexpr std::uint32_t Rotl(std::uint32_t x, int n) noexcept { return (x << n) | (x >> (32 - n)); }

    void Block(const std::uint8_t* p) noexcept
    {
        std::uint32_t w[80];
        for (std::size_t i = 0; i < 16; ++i)
            w[i] = (static_cast<std::uint32_t>(p[4 * i]) << 24) | (static_cast<std::uint32_t>(p[4 * i + 1]) << 16) |
                   (static_cast<std::uint32_t>(p[4 * i + 2]) << 8) | static_cast<std::uint32_t>(p[4 * i + 3]);
        for (std::size_t i = 16; i < 80; ++i)
            w[i] = Rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (std::size_t i = 0; i < 80; ++i)
        {
            std::uint32_t f, k;
            if (i < 20)
            {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            }
            else if (i < 40)
            {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            }
            else if (i < 60)
            {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            }
            else
            {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t t = Rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = Rotl(b, 30);
            b = a;
            a = t;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
    }

    std::array<std::uint32_t, 5> h_{};
    std::array<std::uint8_t, kBlockSize> buf_{};
    std::uint64_t total_bytes_ = 0;
    std::size_t buffered_ = 0;
};

} // namespace breeze
