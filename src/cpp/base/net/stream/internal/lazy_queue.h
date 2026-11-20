// 惰性创建、排空即释放的先进先出队列（std::deque 的薄包装）。给连接的发送队列用，设计见方案 §11-1。
//
// 连接的发送队列几乎永远是空的，而 libstdc++ 的空 std::deque 一构造就是 2 次 new、576 字节（64B 映射表 + 512B 节点），
// 占明文连接约 62% 的内存（docs/网络库设计.md §9.10 里每连接约 920B，实测基线 916B）。所以第一次入队才创建 deque，
// 最后一个元素弹出就释放。不变量：**内部指针非空 ⇒ 队列非空**，empty() 因此只读一个指针（比读 deque 的两个迭代器还少）。
// 代价：反复"积压一点又排空"的连接每次要分配 / 释放一次（约一百纳秒），相对触发积压的那次 EPOLLOUT 注册与系统调用可以忽略。
//
// 只在所属 IO 线程使用，非线程安全。front / pop_front / begin / end 只能在 !empty() 时调用。

#pragma once

#include <cassert>
#include <cstddef>
#include <deque>
#include <memory>
#include <utility>

namespace breeze
{

template <class T>
class LazyQueue
{
public:
    bool empty() const noexcept { return q_ == nullptr; }
    std::size_t size() const noexcept { return q_ != nullptr ? q_->size() : 0; }

    template <class... Args>
    T& emplace_back(Args&&... args)
    {
        if (q_ == nullptr)
            q_ = std::make_unique<std::deque<T>>();
        return q_->emplace_back(std::forward<Args>(args)...);
    }

    T& front() noexcept
    {
        assert(q_ != nullptr);
        return q_->front();
    }

    // 弹出队首；弹空了就释放整个 deque（之后对 front() 取到的引用都失效）。
    void pop_front()
    {
        assert(q_ != nullptr);
        q_->pop_front();
        if (q_->empty())
            q_.reset();
    }

    auto begin() noexcept
    {
        assert(q_ != nullptr);
        return q_->begin();
    }
    auto end() noexcept
    {
        assert(q_ != nullptr);
        return q_->end();
    }

private:
    std::unique_ptr<std::deque<T>> q_;
};

} // namespace breeze
