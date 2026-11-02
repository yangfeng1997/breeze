// 睡着的线程 / 进程被唤醒要多久：eventfd + epoll、eventfd 直接 read、futex、条件变量的对比（设计见 docs/共享内存IPC设计.md §5 / §10）。
//
// 乒乓：A 通知 B、B 通知回 A，两端都真的睡（不自旋），测往返 = 两次唤醒。数据只是一个序号（原子变量），通知方式不同：
//   eventfd-epoll   生产者 write(eventfd)；消费者 epoll_wait → read(eventfd)。事件循环线程的标准做法，也是共享内存环现在用的
//   eventfd-read    同上，但消费者直接阻塞 read(eventfd)，不经 epoll：看 epoll 本身占多少
//   futex           序号就是 futex 字：生产者 futex_wake；消费者 futex_wait。线程间 / 进程间都能用（进程间用非 PRIVATE 版）
//   condvar         std::mutex + std::condition_variable（只有线程版）
//   *-proc          两个进程（fork；序号放在 MAP_SHARED 的匿名内存里）：看"拆成两个进程"对唤醒有没有影响
// 每次都无条件通知（不做"消费者在睡才通知"的优化）：乒乓里消费者每次都在睡，两边公平。
//
// 用法：wake_bench --mode eventfd-epoll|eventfd-read|futex|condvar|eventfd-epoll-proc|futex-proc [--iters 30000] [--cpu-a 2] [--cpu-b 4]
// 输出一行 RESULT，由 bench/run_wake.sh 汇总。

#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

namespace
{

[[noreturn]] void Die(const std::string& m)
{
    std::fprintf(stderr, "wake_bench: %s\n", m.c_str());
    std::_Exit(2);
}

void PinThis(int cpu)
{
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    if (pthread_setaffinity_np(pthread_self(), sizeof(s), &s) != 0)
        std::perror("pthread_setaffinity_np");
}

enum class Kind { EventfdEpoll, EventfdRead, Futex, Condvar };

// 一个方向：序号字（兼 futex 字）+ 这个方向的 eventfd + 条件变量。
struct Dir
{
    alignas(64) std::atomic<std::uint32_t> word{0};
    int efd = -1;
    std::mutex mu;
    std::condition_variable cv;
};

long FutexWait(std::atomic<std::uint32_t>* a, std::uint32_t expected, bool priv)
{
    return ::syscall(SYS_futex, a, priv ? FUTEX_WAIT_PRIVATE : FUTEX_WAIT, expected, nullptr, nullptr, 0);
}
long FutexWake(std::atomic<std::uint32_t>* a, bool priv)
{
    return ::syscall(SYS_futex, a, priv ? FUTEX_WAKE_PRIVATE : FUTEX_WAKE, 1, nullptr, nullptr, 0);
}

// 一端的等待者：每端只等一个方向。epoll 实例在各端自己创建（fork 之后各建各的，免得两个进程共用一个 epoll 集合）。
class Waiter
{
public:
    Waiter(Kind k, Dir& rx, bool priv)
        : k_(k)
        , rx_(rx)
        , priv_(priv)
    {
        if (k_ == Kind::EventfdEpoll)
        {
            ep_ = ::epoll_create1(EPOLL_CLOEXEC);
            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.fd = rx_.efd;
            if (ep_ < 0 || ::epoll_ctl(ep_, EPOLL_CTL_ADD, rx_.efd, &ev) != 0)
                Die("epoll 初始化失败");
        }
    }
    ~Waiter()
    {
        if (ep_ >= 0)
            ::close(ep_);
    }
    // 等到序号字 == seq。通知可能先于等待到达（字已经是 seq）：直接返回；eventfd 里留下的旧计数会让下一次等待多醒一次，循环里重查字即可。
    void Wait(std::uint32_t seq)
    {
        while (rx_.word.load(std::memory_order_acquire) != seq)
        {
            switch (k_)
            {
                case Kind::EventfdEpoll:
                {
                    epoll_event ev;
                    ::epoll_wait(ep_, &ev, 1, -1);
                    std::uint64_t c;
                    (void)!::read(rx_.efd, &c, sizeof(c));
                    break;
                }
                case Kind::EventfdRead:
                {
                    std::uint64_t c;
                    (void)!::read(rx_.efd, &c, sizeof(c));
                    break;
                }
                case Kind::Futex:
                {
                    const std::uint32_t cur = rx_.word.load(std::memory_order_acquire);
                    if (cur != seq)
                        FutexWait(&rx_.word, cur, priv_);
                    break;
                }
                case Kind::Condvar:
                {
                    std::unique_lock lk(rx_.mu);
                    rx_.cv.wait(lk, [&] { return rx_.word.load(std::memory_order_acquire) == seq; });
                    break;
                }
            }
        }
    }

private:
    Kind k_;
    Dir& rx_;
    bool priv_;
    int ep_ = -1;
};

void Notify(Kind k, Dir& tx, std::uint32_t seq, bool priv)
{
    switch (k)
    {
        case Kind::EventfdEpoll:
        case Kind::EventfdRead:
        {
            tx.word.store(seq, std::memory_order_release);
            const std::uint64_t one = 1;
            (void)!::write(tx.efd, &one, sizeof(one));
            break;
        }
        case Kind::Futex:
            tx.word.store(seq, std::memory_order_release);
            FutexWake(&tx.word, priv);
            break;
        case Kind::Condvar:
        {
            {
                std::lock_guard lk(tx.mu);
                tx.word.store(seq, std::memory_order_release);
            }
            tx.cv.notify_one();
            break;
        }
    }
}

struct Shared
{
    Dir ab; // A → B
    Dir ba; // B → A
};

void RoleB(Kind k, Shared& s, bool priv, std::uint32_t total)
{
    Waiter w(k, s.ab, priv);
    for (std::uint32_t i = 1; i <= total; ++i)
    {
        w.Wait(i);
        Notify(k, s.ba, i, priv);
    }
}

struct Stats
{
    double avg_us = 0, p50_us = 0, p99_us = 0;
};

Stats RoleA(Kind k, Shared& s, bool priv, std::uint32_t warm, std::uint32_t iters)
{
    Waiter w(k, s.ba, priv);
    std::vector<double> samples;
    samples.reserve(iters);
    for (std::uint32_t i = 1; i <= warm + iters; ++i)
    {
        const auto t0 = Clock::now();
        Notify(k, s.ab, i, priv);
        w.Wait(i);
        if (i > warm)
            samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
    }
    Stats st;
    double sum = 0;
    for (double v : samples)
        sum += v;
    st.avg_us = sum / static_cast<double>(samples.size());
    std::sort(samples.begin(), samples.end());
    st.p50_us = samples[samples.size() / 2];
    st.p99_us = samples[samples.size() * 99 / 100];
    return st;
}

} // namespace

int main(int argc, char** argv)
{
    std::map<std::string, std::string> kv;
    for (int i = 1; i + 1 < argc; i += 2)
        kv[argv[i] + 2] = argv[i + 1];
    const std::string mode = kv.count("mode") ? kv["mode"] : "";
    const std::uint32_t iters = kv.count("iters") ? static_cast<std::uint32_t>(std::strtoul(kv["iters"].c_str(), nullptr, 10)) : 30000;
    const int cpu_a = kv.count("cpu-a") ? std::atoi(kv["cpu-a"].c_str()) : 2;
    const int cpu_b = kv.count("cpu-b") ? std::atoi(kv["cpu-b"].c_str()) : 4;
    constexpr std::uint32_t kWarm = 3000;

    const bool proc = mode.size() > 5 && mode.substr(mode.size() - 5) == "-proc";
    const std::string base = proc ? mode.substr(0, mode.size() - 5) : mode;
    Kind k;
    if (base == "eventfd-epoll")
        k = Kind::EventfdEpoll;
    else if (base == "eventfd-read")
        k = Kind::EventfdRead;
    else if (base == "futex")
        k = Kind::Futex;
    else if (base == "condvar")
        k = Kind::Condvar;
    else
        Die("--mode 不对");
    if (proc && k == Kind::Condvar)
        Die("condvar 没有进程版");

    Shared local;
    Shared* s = &local;
    if (proc) // 序号字放在跨进程共享的匿名内存里；条件变量不用（condvar 只有线程版）
    {
        void* m = ::mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED)
            Die("mmap 失败");
        s = new (m) Shared();
    }
    s->ab.efd = ::eventfd(0, EFD_CLOEXEC);
    s->ba.efd = ::eventfd(0, EFD_CLOEXEC);
    if (s->ab.efd < 0 || s->ba.efd < 0)
        Die("eventfd 失败");
    const bool priv = !proc; // 线程版用 PRIVATE 的 futex（更快）；进程版必须用非 PRIVATE

    const std::uint32_t total = kWarm + iters;
    Stats st;
    if (!proc)
    {
        std::thread tb(
            [&]
            {
                PinThis(cpu_b);
                RoleB(k, *s, priv, total);
            });
        PinThis(cpu_a);
        st = RoleA(k, *s, priv, kWarm, iters);
        tb.join();
    }
    else
    {
        const pid_t pid = ::fork();
        if (pid < 0)
            Die("fork 失败");
        if (pid == 0)
        {
            PinThis(cpu_b);
            RoleB(k, *s, priv, total);
            std::_Exit(0);
        }
        PinThis(cpu_a);
        st = RoleA(k, *s, priv, kWarm, iters);
        int status = 0;
        ::waitpid(pid, &status, 0);
    }
    std::printf("RESULT mode=%s avg_rtt_us=%.2f p50_rtt_us=%.2f p99_rtt_us=%.2f\n", mode.c_str(), st.avg_us, st.p50_us, st.p99_us);
    return 0;
}
