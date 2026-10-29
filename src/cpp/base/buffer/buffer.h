// 每连接的读缓冲：[read, write) 是已收到、尚未被分帧消费的字节。设计见 docs/网络库设计.md §4.7。
//
// 读 socket 的用法：PrepareWrite 保证尾部有足够连续空间 → 把 Writable() 交给 readv → Commit(实际读到的字节数)。
// 空闲连接用 ShrinkIfEmpty 释放内存，10 万空闲连接不占大块。只在所属 IO 线程使用，非线程安全。

#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <utility>

namespace breeze
{

class Buffer
{
public:
    explicit Buffer(std::size_t initial_capacity = 0);

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    // 被移走的对象回到空状态（默认移动会留下 cap_ > 0 而 data_ 为空的悬空状态）。
    Buffer(Buffer&& o) noexcept
        : data_(std::move(o.data_))
        , cap_(std::exchange(o.cap_, 0))
        , read_(std::exchange(o.read_, 0))
        , write_(std::exchange(o.write_, 0))
    {
    }
    Buffer& operator=(Buffer&& o) noexcept
    {
        if (this != &o)
        {
            data_ = std::move(o.data_);
            cap_ = std::exchange(o.cap_, 0);
            read_ = std::exchange(o.read_, 0);
            write_ = std::exchange(o.write_, 0);
        }
        return *this;
    }

    std::size_t ReadableBytes() const noexcept { return write_ - read_; }
    // 视图只在下一次 Consume / Append / PrepareWrite / Shrink 之前有效。
    std::span<const std::byte> Readable() const noexcept { return {data_.get() + read_, write_ - read_}; }

    // n 超过 ReadableBytes() 是编程错误。读空时下标回到起点，让下一次写从头开始。
    void Consume(std::size_t n) noexcept;

    void Append(std::span<const std::byte> src);

    // 保证尾部至少有 n 字节连续可写：先尝试把已读部分挤掉，不够再扩容（至少翻倍）。
    void PrepareWrite(std::size_t n);
    std::span<std::byte> Writable() noexcept { return {data_.get() + write_, cap_ - write_}; }
    void Commit(std::size_t n) noexcept;

    std::size_t Capacity() const noexcept { return cap_; }

    // 缓冲为空且容量大于 keep 时缩到 keep 字节（0 即释放）。非空时什么也不做。
    void ShrinkIfEmpty(std::size_t keep);

private:
    std::unique_ptr<std::byte[]> data_;
    std::size_t cap_ = 0;
    std::size_t read_ = 0;
    std::size_t write_ = 0;
};

} // namespace breeze
