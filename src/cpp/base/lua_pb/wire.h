// lua_pb 的字节层：varint / zigzag / 定长小端，以及带上界的 Reader 与可回填长度的 Writer。
// 设计见 docs/Lua_pb库设计.md §3、§9。
//
// 只用标准库，不碰 Lua，不抛异常：所有读函数返回 false 表示失败，调用方转成 Error。
// 纯计算部分（VarintSize / ZigZag / 编解码）是 constexpr，编译期用例见 lua_pb_wire_test.cpp。
//
// 不可信输入的前提：varint 最多 10 字节且第 10 字节只能有最低 1 位；每个 LEN 长度先与剩余字节做
// 溢出安全比较再取视图；group 直接拒绝（本库不做）。

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring> // std::memcpy / std::memmove
#include <memory>  // std::unique_ptr
#include <span>
#include <utility> // std::unreachable

namespace breeze::lua_pb
{

// 小端：fixed32 / fixed64 与 float / double 的线上字节序（protobuf 规定）。
static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big,
              "只支持小端或大端主机");

enum class WireType : std::uint8_t
{
    kVarint = 0,
    kFixed64 = 1,
    kLen = 2,
    kStartGroup = 3, // 拒绝
    kEndGroup = 4,   // 拒绝
    kFixed32 = 5,
};

// field number 上限：1..2^29-1（协议规定），tag 因此一定装进 uint32。
inline constexpr std::uint32_t kMaxFieldNumber = 536870911u;

inline constexpr std::size_t kMaxVarintBytes = 10;

// tag = (field_number << 3) | wire_type
constexpr std::uint32_t MakeTag(std::uint32_t field_number, WireType wt) noexcept
{
    return (field_number << 3) | static_cast<std::uint32_t>(wt);
}

constexpr std::uint32_t TagFieldNumber(std::uint32_t tag) noexcept { return tag >> 3; }

constexpr WireType TagWireType(std::uint32_t tag) noexcept { return static_cast<WireType>(tag & 0x7u); }

constexpr std::size_t VarintSize(std::uint64_t v) noexcept
{
    std::size_t n = 1;
    while (v >= 0x80u)
    {
        v >>= 7;
        ++n;
    }
    return n;
}

constexpr std::uint64_t ZigZagEncode(std::int64_t v) noexcept
{
    return (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63);
}

constexpr std::int64_t ZigZagDecode(std::uint64_t v) noexcept
{
    return static_cast<std::int64_t>((v >> 1) ^ (~(v & 1) + 1));
}

// 把 v 写成 varint，返回写完的位置。调用方用 Reserve(VarintSize(v)) 保证空间。
constexpr std::byte* WriteVarint(std::byte* p, std::uint64_t v) noexcept
{
    while (v >= 0x80u)
    {
        *p++ = static_cast<std::byte>((v & 0x7Fu) | 0x80u);
        v >>= 7;
    }
    *p++ = static_cast<std::byte>(v);
    return p;
}

// 只读、带游标的视图；不拥有数据。Read* 失败时游标可能已前进，调用方一律放弃本次解析。
class Reader
{
public:
    constexpr Reader() noexcept = default;

    constexpr explicit Reader(std::span<const std::byte> data) noexcept
        : data_(data)
    {
    }

    constexpr bool Empty() const noexcept { return pos_ == data_.size(); }
    constexpr std::size_t Remaining() const noexcept { return data_.size() - pos_; }
    constexpr std::size_t Offset() const noexcept { return pos_; }
    constexpr std::span<const std::byte> Rest() const noexcept { return data_.subspan(pos_); }

    // ≤10 字节；第 10 字节只允许最低 1 位（否则值溢出或还有后续）。
    constexpr bool ReadVarint(std::uint64_t& out) noexcept
    {
        std::uint64_t result = 0;
        unsigned shift = 0;
        for (std::size_t i = 0; i < kMaxVarintBytes; ++i)
        {
            if (pos_ >= data_.size())
                return false;
            const auto byte = std::to_integer<std::uint8_t>(data_[pos_++]);
            if (i == kMaxVarintBytes - 1)
            {
                if ((byte & 0xFEu) != 0)
                    return false;
                result |= static_cast<std::uint64_t>(byte) << shift;
                out = result;
                return true;
            }
            result |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
            if ((byte & 0x80u) == 0)
            {
                out = result;
                return true;
            }
            shift += 7;
        }
        return false;
    }

    // tag：field number 与 wire type。field number 0 由调用方判（要区分 kFieldZero 与 kTruncated）。
    constexpr bool ReadTag(std::uint32_t& tag) noexcept
    {
        std::uint64_t v = 0;
        if (!ReadVarint(v) || v > 0xFFFFFFFFu)
            return false;
        tag = static_cast<std::uint32_t>(v);
        return true;
    }

    constexpr bool ReadFixed32(std::uint32_t& out) noexcept
    {
        if (Remaining() < 4)
            return false;
        std::uint32_t v = 0;
        for (unsigned i = 0; i < 4; ++i)
            v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data_[pos_ + i])) << (8u * i);
        pos_ += 4;
        out = v;
        return true;
    }

    constexpr bool ReadFixed64(std::uint64_t& out) noexcept
    {
        if (Remaining() < 8)
            return false;
        std::uint64_t v = 0;
        for (unsigned i = 0; i < 8; ++i)
            v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(data_[pos_ + i])) << (8u * i);
        pos_ += 8;
        out = v;
        return true;
    }

    // LEN 字段：读长度前缀并在剩余字节内取视图；长度超过剩余字节返回 false（不前进）。
    constexpr bool ReadLengthDelimited(std::span<const std::byte>& out) noexcept
    {
        std::uint64_t len = 0;
        if (!ReadVarint(len))
            return false;
        if (len > Remaining()) // Remaining() 是 size_t，len 是 uint64：先比大小再截断，避免溢出
            return false;
        out = data_.subspan(pos_, static_cast<std::size_t>(len));
        pos_ += static_cast<std::size_t>(len);
        return true;
    }

    // 跳过未知字段。group（kStartGroup / kEndGroup）与非法 wire type（6 / 7）都返回 false。
    constexpr bool Skip(WireType wt) noexcept
    {
        switch (wt)
        {
            case WireType::kVarint:
            {
                std::uint64_t v = 0;
                return ReadVarint(v);
            }
            case WireType::kFixed64:
            {
                std::uint64_t v = 0;
                return ReadFixed64(v);
            }
            case WireType::kLen:
            {
                std::span<const std::byte> v;
                return ReadLengthDelimited(v);
            }
            case WireType::kFixed32:
            {
                std::uint32_t v = 0;
                return ReadFixed32(v);
            }
            case WireType::kStartGroup:
            case WireType::kEndGroup:
                return false;
        }
        return false; // wire type 6 / 7 不存在（tag 只有 3 位，但取值非法）
    }

private:
    std::span<const std::byte> data_{};
    std::size_t pos_ = 0;
};

// 只增的写缓冲：嵌套长度先预留 1 字节，写完子消息再回填（装不下才 memmove）。
// 1.5× + 64 扩容；跨调用复用，不缩小（复用策略由 Codec 决定）。
//
// 缓冲是裸分配：**不能用 std::vector 的 resize 当 Commit**——resize 会把新增元素清零，
// 把 "Reserve 后写指针、再 Commit" 写进去的字节覆盖掉。
class Writer
{
public:
    explicit Writer(std::size_t initial_capacity = 128) { EnsureCapacity(initial_capacity); }

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    Writer(Writer&& o) noexcept
        : data_(std::move(o.data_))
        , capacity_(std::exchange(o.capacity_, 0))
        , size_(std::exchange(o.size_, 0))
    {
    }
    Writer& operator=(Writer&& o) noexcept
    {
        if (this != &o)
        {
            data_ = std::move(o.data_);
            capacity_ = std::exchange(o.capacity_, 0);
            size_ = std::exchange(o.size_, 0);
        }
        return *this;
    }

    std::size_t Size() const noexcept { return size_; }
    std::size_t Capacity() const noexcept { return capacity_; }
    // 视图在下次 Reserve / Commit / Clear 之前有效。
    std::span<const std::byte> Bytes() const noexcept { return {data_.get(), size_}; }

    void Clear() noexcept { size_ = 0; }

    // 保证尾部有 n 字节连续可写空间并返回首地址；**不改 size_**，写完由调用方 Commit。
    std::byte* Reserve(std::size_t n)
    {
        EnsureCapacity(size_ + n);
        return data_.get() + size_;
    }

    void Commit(std::size_t n) noexcept { size_ += n; }

    void WriteVarint(std::uint64_t v)
    {
        std::byte* p = Reserve(VarintSize(v));
        p = lua_pb::WriteVarint(p, v);
        Commit(static_cast<std::size_t>(p - (data_.get() + size_)));
    }

    void WriteRaw(std::span<const std::byte> bytes)
    {
        std::byte* p = Reserve(bytes.size());
        std::memcpy(p, bytes.data(), bytes.size());
        Commit(bytes.size());
    }

    void WriteFixed32(std::uint32_t v)
    {
        std::byte* p = Reserve(4);
        for (unsigned i = 0; i < 4; ++i)
            p[i] = static_cast<std::byte>((v >> (8u * i)) & 0xFFu);
        Commit(4);
    }

    void WriteFixed64(std::uint64_t v)
    {
        std::byte* p = Reserve(8);
        for (unsigned i = 0; i < 8; ++i)
            p[i] = static_cast<std::byte>((v >> (8u * i)) & 0xFFu);
        Commit(8);
    }

    // 嵌套长度：先在当前位置留出 1 字节，写完负载后回填。
    std::size_t ReserveLength() noexcept
    {
        const std::size_t at = size_;
        Commit(1); // 长度 1 字节的占位；值由 BackfillLength 填
        return at;
    }

    // 把 [at] 处的 1 字节替换成 payload_len 的 varint；装不下就把后面整体右移（只在长度 ≥128 时发生）。
    void BackfillLength(std::size_t at, std::size_t payload_len)
    {
        const std::uint64_t len = payload_len;
        if (VarintSize(len) == 1)
        {
            data_[at] = static_cast<std::byte>(len);
            return;
        }
        const std::size_t extra = VarintSize(len) - 1;
        const std::size_t tail = size_ - at - 1;
        EnsureCapacity(size_ + extra);
        std::memmove(data_.get() + at + 1 + extra, data_.get() + at + 1, tail);
        size_ += extra;
        std::byte* p = data_.get() + at;
        lua_pb::WriteVarint(p, len);
    }

private:
    void EnsureCapacity(std::size_t need)
    {
        if (need <= capacity_)
            return;
        std::size_t cap = capacity_ == 0 ? 128 : capacity_;
        while (cap < need)
            cap += cap / 2 + 64;
        std::unique_ptr<std::byte[]> next{new std::byte[cap]};
        if (size_ > 0)
            std::memcpy(next.get(), data_.get(), size_);
        data_ = std::move(next);
        capacity_ = cap;
    }

    std::unique_ptr<std::byte[]> data_{};
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
};

} // namespace breeze::lua_pb
