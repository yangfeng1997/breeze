#include "buffer.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace breeze
{

Buffer::Buffer(std::size_t initial_capacity)
    : data_(initial_capacity == 0 ? nullptr : std::make_unique_for_overwrite<std::byte[]>(initial_capacity))
    , cap_(initial_capacity)
{
}

void Buffer::Consume(std::size_t n) noexcept
{
    assert(n <= ReadableBytes() && "Buffer::Consume 超过可读字节数");
    read_ += n;
    if (read_ == write_)
        read_ = write_ = 0;
}

void Buffer::Append(std::span<const std::byte> src)
{
    if (src.empty())
        return;
    PrepareWrite(src.size());
    std::memcpy(data_.get() + write_, src.data(), src.size());
    write_ += src.size();
}

void Buffer::PrepareWrite(std::size_t n)
{
    if (cap_ - write_ >= n)
        return;
    const std::size_t readable = ReadableBytes();
    if (cap_ - readable >= n)
    {
        // 前面已读的空间加上尾部空闲够用：挤到起点，不分配。
        std::memmove(data_.get(), data_.get() + read_, readable);
    }
    else
    {
        const std::size_t new_cap = std::max(cap_ * 2, readable + n);
        auto fresh = std::make_unique_for_overwrite<std::byte[]>(new_cap);
        if (readable != 0)
            std::memcpy(fresh.get(), data_.get() + read_, readable);
        data_ = std::move(fresh);
        cap_ = new_cap;
    }
    read_ = 0;
    write_ = readable;
}

void Buffer::Commit(std::size_t n) noexcept
{
    assert(n <= cap_ - write_ && "Buffer::Commit 超出已准备的可写空间");
    write_ += n;
}

void Buffer::ShrinkIfEmpty(std::size_t keep)
{
    if (ReadableBytes() != 0 || cap_ <= keep)
        return;
    data_ = keep == 0 ? nullptr : std::make_unique_for_overwrite<std::byte[]>(keep);
    cap_ = keep;
    read_ = write_ = 0;
}

} // namespace breeze
