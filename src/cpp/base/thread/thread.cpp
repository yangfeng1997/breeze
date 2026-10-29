#include "thread.h"

#include <sched.h>

#include <cerrno>
#include <memory>

#include "current_thread.h"
#include "logger.h"
#include "logger_macros.h"

namespace breeze
{

namespace
{

std::atomic<std::uint32_t> g_created{0};
std::atomic<std::uint32_t> g_auto_name_seq{0};

struct Payload
{
    std::move_only_function<void()> fn;
    std::string name;
    std::atomic<pid_t>* tid; // 指向 Thread 里的槽：Start 在等它，写完通知之后就不能再碰了
};

void* Run(void* arg)
{
    std::unique_ptr<Payload> p(static_cast<Payload*>(arg));
    this_thread::SetName(p->name);
    auto fn = std::move(p->fn);
    std::atomic<pid_t>* slot = p->tid;
    p.reset();
    // 先把要用的都搬到本线程，再通知：通知之后 Start 返回，Thread 对象随时可能被调用方移动或销毁（Join 之前不会销毁，
    // 但这个线程不该再依赖它）。
    slot->store(this_thread::Tid(), std::memory_order_release);
    slot->notify_one();
    fn();
    return nullptr;
}

} // namespace

Thread::~Thread()
{
    if (joinable_)
        LOG_FATAL("Thread 析构时仍未 Join name={}", name_);
}

int Thread::Start(std::move_only_function<void()> fn, std::string_view name)
{
    if (joinable_)
        LOG_FATAL("Thread 重复 Start name={}", name_);
    name_ = name.empty() ? "Thread-" + std::to_string(g_auto_name_seq.fetch_add(1, std::memory_order_relaxed) + 1)
                         : std::string(name);
    tid_.store(0, std::memory_order_relaxed);
    auto payload = std::make_unique<Payload>(Payload{std::move(fn), name_, &tid_});
    const int rc = ::pthread_create(&handle_, nullptr, &Run, payload.get());
    if (rc != 0)
        return rc; // payload 由 unique_ptr 释放，fn 随之销毁
    payload.release();
    joinable_ = true;
    g_created.fetch_add(1, std::memory_order_relaxed);
    // 等新线程报上 tid：之后 Tid() 立刻可用，也保证线程名已经设好。
    for (pid_t t = tid_.load(std::memory_order_acquire); t == 0; t = tid_.load(std::memory_order_acquire))
        tid_.wait(0, std::memory_order_acquire);
    return 0;
}

void Thread::Join()
{
    if (!joinable_)
        return;
    ::pthread_join(handle_, nullptr);
    joinable_ = false;
    tid_.store(0, std::memory_order_release);
}

int Thread::SetAffinity(int cpu) const noexcept
{
    if (!joinable_ || cpu < 0 || cpu >= CPU_SETSIZE)
        return EINVAL;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return ::pthread_setaffinity_np(handle_, sizeof(set), &set);
}

std::uint32_t Thread::NumCreated() noexcept { return g_created.load(std::memory_order_relaxed); }

} // namespace breeze
