#include "base/buffer/shared_buffer.h"

#include <atomic>
#include <cassert>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#include "base/logger/logger.h"
#include "base/logger/logger_macros.h"

namespace breeze
{

// 块头紧挨着数据；对齐到 16，保证数据区对任何标量类型都对齐。
// 16 正是默认 operator new 的对齐（__STDCPP_DEFAULT_NEW_ALIGNMENT__），所以用普通版本即可：显式带 align_val_t
// 的版本会走 posix_memalign，callgrind 量到每条消息多约 100 条指令（P2a 基线分析，见 docs/网络库设计.md §9.1）。
struct alignas(16) SharedBuffer::Block
{
    std::atomic<std::uint32_t> refs{1};

    std::byte* bytes() noexcept { return reinterpret_cast<std::byte*>(this + 1); }
};

SharedBuffer::SharedBuffer(const SharedBuffer& o) noexcept
    : block_(o.block_)
    , offset_(o.offset_)
    , len_(o.len_)
{
    if (block_ != nullptr)
        block_->refs.fetch_add(1, std::memory_order_relaxed); // 已有引用在手，新增不需要同步
}

SharedBuffer::SharedBuffer(SharedBuffer&& o) noexcept
    : block_(std::exchange(o.block_, nullptr))
    , offset_(std::exchange(o.offset_, 0))
    , len_(std::exchange(o.len_, 0))
{
}

SharedBuffer& SharedBuffer::operator=(const SharedBuffer& o) noexcept
{
    if (this != &o)
    {
        SharedBuffer tmp(o);
        *this = std::move(tmp);
    }
    return *this;
}

SharedBuffer& SharedBuffer::operator=(SharedBuffer&& o) noexcept
{
    if (this != &o)
    {
        Release();
        block_ = std::exchange(o.block_, nullptr);
        offset_ = std::exchange(o.offset_, 0);
        len_ = std::exchange(o.len_, 0);
    }
    return *this;
}

SharedBuffer::~SharedBuffer() { Release(); }

void SharedBuffer::Release() noexcept
{
    // acq_rel：最后一个持有者销毁块之前，要看到其它持有者此前对数据的全部读写。
    if (block_ != nullptr && block_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        block_->~Block();
        ::operator delete(block_);
    }
    block_ = nullptr;
}

SharedBuffer SharedBuffer::Create(std::size_t size)
{
    if (size == 0)
        return {};
    if (size > std::numeric_limits<std::uint32_t>::max())
        LOG_FATAL("SharedBuffer::Create size={} 超过 4GB", size);
    static_assert(alignof(Block) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
                  "块头对齐超过默认 operator new 的保证，需要对齐版本");
    void* mem = ::operator new(sizeof(Block) + size);
    return SharedBuffer(new (mem) Block, 0, static_cast<std::uint32_t>(size));
}

SharedBuffer SharedBuffer::CopyFrom(std::span<const std::byte> src)
{
    SharedBuffer b = Create(src.size());
    if (!src.empty())
        std::memcpy(b.block_->bytes(), src.data(), src.size());
    return b;
}

std::span<std::byte> SharedBuffer::MutableBytes() noexcept
{
    if (block_ == nullptr)
        return {};
    assert(block_->refs.load(std::memory_order_relaxed) == 1 && "SharedBuffer 已被共享，不可再写");
    return {block_->bytes() + offset_, len_};
}

const std::byte* SharedBuffer::data() const noexcept { return block_ == nullptr ? nullptr : block_->bytes() + offset_; }

SharedBuffer SharedBuffer::Slice(std::size_t offset, std::size_t len) const noexcept
{
    assert(offset <= len_ && len <= len_ - offset && "SharedBuffer::Slice 越界");
    if (len == 0 || block_ == nullptr)
        return {};
    block_->refs.fetch_add(1, std::memory_order_relaxed);
    return SharedBuffer(block_, offset_ + static_cast<std::uint32_t>(offset), static_cast<std::uint32_t>(len));
}

std::size_t SharedBuffer::UseCount() const noexcept
{
    return block_ == nullptr ? 0 : block_->refs.load(std::memory_order_relaxed);
}

} // namespace breeze
