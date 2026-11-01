#include "base/shm/shm_mirror_map.h"

#include <sys/mman.h>

#include <cerrno>
#include <utility>

namespace breeze::shm
{

SharedMapping& SharedMapping::operator=(SharedMapping&& o) noexcept
{
    if (this != &o)
    {
        if (base_ != nullptr)
            ::munmap(base_, size_);
        base_ = std::exchange(o.base_, nullptr);
        size_ = std::exchange(o.size_, 0);
    }
    return *this;
}

SharedMapping::~SharedMapping()
{
    if (base_ != nullptr)
        ::munmap(base_, size_);
}

std::expected<SharedMapping, int> SharedMapping::Map(int fd, std::uint64_t offset, std::uint64_t size)
{
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, static_cast<off_t>(offset));
    if (p == MAP_FAILED)
        return std::unexpected(errno);
    SharedMapping m;
    m.base_ = p;
    m.size_ = size;
    return m;
}

MirrorMapping& MirrorMapping::operator=(MirrorMapping&& o) noexcept
{
    if (this != &o)
    {
        if (base_ != nullptr)
            ::munmap(base_, 2 * capacity_);
        base_ = std::exchange(o.base_, nullptr);
        capacity_ = std::exchange(o.capacity_, 0);
    }
    return *this;
}

MirrorMapping::~MirrorMapping()
{
    if (base_ != nullptr)
        ::munmap(base_, 2 * capacity_);
}

std::expected<MirrorMapping, int> MirrorMapping::Map(int fd, std::uint64_t offset, std::uint64_t capacity)
{
    // 先预留一整段连续地址，再把两半各自覆盖成对同一文件偏移的映射；中途失败就整段还回去。
    void* reserve = ::mmap(nullptr, 2 * capacity, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reserve == MAP_FAILED)
        return std::unexpected(errno);
    auto* base = static_cast<std::byte*>(reserve);
    for (int half = 0; half < 2; ++half)
    {
        void* p = ::mmap(base + half * capacity,
                         capacity,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_FIXED,
                         fd,
                         static_cast<off_t>(offset));
        if (p == MAP_FAILED)
        {
            const int err = errno;
            ::munmap(reserve, 2 * capacity);
            return std::unexpected(err);
        }
    }
    MirrorMapping m;
    m.base_ = reserve;
    m.capacity_ = capacity;
    return m;
}

} // namespace breeze::shm
