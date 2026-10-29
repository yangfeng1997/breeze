// 引用计数的不可变字节块上的 (block, offset, len) 切片值。设计见 docs/网络库设计.md §4.6。
//
// - 同一块 payload 可被多条连接同时持有，发送时不拷贝数据；拷贝 / 切片只增加引用计数。
// - 块在分享之前（唯一持有时）可写，用于序列化直接写进 Create(size)
// 的结果，不再多拷一次；一旦被复制或切片，视为不可变。
// - 用通用分配器，不自建池：SharedBuffer 在 IO 线程分配、主线程释放，自建池需要跨线程归还通道。
// - 引用计数是原子的，可在线程间复制、销毁。

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace breeze
{

class SharedBuffer
{
public:
    SharedBuffer() noexcept = default;
    SharedBuffer(const SharedBuffer& o) noexcept;
    SharedBuffer(SharedBuffer&& o) noexcept;
    SharedBuffer& operator=(const SharedBuffer& o) noexcept;
    SharedBuffer& operator=(SharedBuffer&& o) noexcept;
    ~SharedBuffer();

    // 新块，size 字节，内容未初始化；size 为 0 得到空对象，不分配。size 超过 4GB 是编程错误。
    static SharedBuffer Create(std::size_t size);
    static SharedBuffer CopyFrom(std::span<const std::byte> src);

    // 只有唯一持有时才可写（Debug 断言）。
    std::span<std::byte> MutableBytes() noexcept;

    std::span<const std::byte> bytes() const noexcept { return {data(), len_}; }
    const std::byte* data() const noexcept;
    std::size_t size() const noexcept { return len_; }
    bool empty() const noexcept { return len_ == 0; }

    // 与本对象共享同一块的 [offset, offset + len)；越界是编程错误。
    SharedBuffer Slice(std::size_t offset, std::size_t len) const noexcept;

    // 诊断 / 测试用：块的当前引用数，空对象为 0。
    std::size_t UseCount() const noexcept;

private:
    struct Block;

    SharedBuffer(Block* b, std::uint32_t offset, std::uint32_t len) noexcept
        : block_(b)
        , offset_(offset)
        , len_(len)
    {
    }
    void Release() noexcept;

    Block* block_ = nullptr;
    std::uint32_t offset_ = 0;
    std::uint32_t len_ = 0;
};

} // namespace breeze
