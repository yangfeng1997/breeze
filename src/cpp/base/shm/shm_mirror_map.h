// 共享内存的映射。
//
// MirrorMapping：同一段 memfd 在连续的虚拟地址上映射两遍。环里任何一条记录哪怕跨过尾部，
// 在虚拟地址上都是连续的，读写方一行折返判断都不用写（设计文档 §3）。
// 做法：先 mmap(PROT_NONE) 预留 2 × capacity 的地址，再用 MAP_FIXED 把同一个文件偏移映射到前后两半。
//
// capacity 必须是页的整数倍，offset 必须页对齐；两者由 ComputeLayout 保证。映射本身不会增减 memfd 的大小。

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>

namespace breeze::shm
{

class SharedMapping
{
public:
    SharedMapping() noexcept = default;
    SharedMapping(const SharedMapping&) = delete;
    SharedMapping& operator=(const SharedMapping&) = delete;
    SharedMapping(SharedMapping&& o) noexcept
        : base_(o.base_)
        , size_(o.size_)
    {
        o.base_ = nullptr;
        o.size_ = 0;
    }
    SharedMapping& operator=(SharedMapping&& o) noexcept;
    ~SharedMapping();

    // 把 fd 的 [offset, offset + size) 映射成可读写的 MAP_SHARED。
    static std::expected<SharedMapping, int> Map(int fd, std::uint64_t offset, std::uint64_t size);

    std::byte* data() const noexcept { return static_cast<std::byte*>(base_); }
    std::uint64_t size() const noexcept { return size_; }

private:
    void* base_ = nullptr;
    std::uint64_t size_ = 0;
};

class MirrorMapping
{
public:
    MirrorMapping() noexcept = default;
    MirrorMapping(const MirrorMapping&) = delete;
    MirrorMapping& operator=(const MirrorMapping&) = delete;
    MirrorMapping(MirrorMapping&& o) noexcept
        : base_(o.base_)
        , capacity_(o.capacity_)
    {
        o.base_ = nullptr;
        o.capacity_ = 0;
    }
    MirrorMapping& operator=(MirrorMapping&& o) noexcept;
    ~MirrorMapping();

    static std::expected<MirrorMapping, int> Map(int fd, std::uint64_t offset, std::uint64_t capacity);

    // 起始地址；[0, 2 × capacity) 都可访问，且后半与前半是同一份内存。
    std::byte* data() const noexcept { return static_cast<std::byte*>(base_); }
    std::uint64_t capacity() const noexcept { return capacity_; }

private:
    void* base_ = nullptr;
    std::uint64_t capacity_ = 0;
};

} // namespace breeze::shm
