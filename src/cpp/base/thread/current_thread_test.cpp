// this_thread：tid 缓存、线程名、IsMainThread，以及 fork 之后的缓存重置。

#include "base/thread/current_thread.h"

#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "base/thread/thread.h"
#include "base/thread/thread_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

pid_t RawTid() { return static_cast<pid_t>(::syscall(SYS_gettid)); }

void TestTid()
{
    CHECK(this_thread::Tid() == RawTid());
    CHECK(this_thread::Tid() == this_thread::Tid()); // 缓存
    CHECK(this_thread::Tid() == ::getpid());         // 主线程的 tid 就是 pid

    // 每个线程各自缓存，互不相同
    constexpr int kThreads = 32;
    std::vector<Thread> ts(kThreads);
    std::vector<pid_t> tids(kThreads, 0);
    std::vector<pid_t> raws(kThreads, 0);
    for (int i = 0; i < kThreads; ++i)
    {
        CHECK(ts[static_cast<std::size_t>(i)].Start(
                  [&, i]
                  {
                      tids[static_cast<std::size_t>(i)] = this_thread::Tid();
                      raws[static_cast<std::size_t>(i)] = RawTid();
                  }) == 0);
    }
    for (auto& t : ts)
        t.Join();
    std::set<pid_t> distinct(tids.begin(), tids.end());
    CHECK(distinct.size() == kThreads);
    CHECK(tids == raws);
}

void TestIsMainThread()
{
    CHECK(this_thread::IsMainThread());
    bool in_thread = true;
    Thread t;
    CHECK(t.Start([&] { in_thread = this_thread::IsMainThread(); }) == 0);
    t.Join();
    CHECK(!in_thread);
}

void TestName()
{
    CHECK(std::strlen(this_thread::Name()) > 0); // 主线程：操作系统里的名字（可执行文件名）

    std::string name;
    Thread t;
    CHECK(t.Start([&] { name = this_thread::Name(); }, "worker-7") == 0);
    t.Join();
    CHECK(name == "worker-7");

    // SetName：截断到 15 个字符，同时设给操作系统；空串是空操作
    std::string got;
    std::string os;
    Thread t2;
    CHECK(t2.Start(
              [&]
              {
                  this_thread::SetName("abcdefghijklmnopqrstuvwxyz");
                  got = this_thread::Name();
                  char buf[32] = {};
                  pthread_getname_np(pthread_self(), buf, sizeof(buf));
                  os = buf;
                  this_thread::SetName("");
                  CHECK(got == this_thread::Name());
              },
              "orig") == 0);
    t2.Join();
    CHECK(got == "abcdefghijklmno");
    CHECK(os == "abcdefghijklmno");
}

// 线程名放在线程局部的定长缓冲里：线程退出之后，别的线程里再取不会碰到悬空指针（老实现存的是指向已释放 string
// 的指针）。
void TestNameIsPerThread()
{
    std::string a;
    std::string b;
    Thread t1;
    Thread t2;
    CHECK(t1.Start([&] { a = this_thread::Name(); }, "aaa") == 0);
    CHECK(t2.Start([&] { b = this_thread::Name(); }, "bbb") == 0);
    t1.Join();
    t2.Join();
    CHECK(a == "aaa" && b == "bbb");
}

// fork 之后子进程里只剩调用 fork 的线程：它的 tid 缓存必须重置（否则还是父进程里的 tid），也成了子进程的主线程。
// 用例在主线程与非主线程里各 fork 一次；fork 前先把 tid 缓存好，没有重置的话子进程里 Tid() != getpid()。
bool ForkAndCheck()
{
    (void)this_thread::Tid(); // 缓存起来
    (void)this_thread::IsMainThread();
    const pid_t pid = ::fork();
    if (pid == 0)
    {
        const bool ok =
            this_thread::Tid() == ::getpid() && this_thread::Tid() == RawTid() && this_thread::IsMainThread();
        ::_exit(ok ? 0 : 1);
    }
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void TestForkResetsCache()
{
    CHECK(ForkAndCheck()); // 主线程里 fork
    bool ok = false;
    Thread t;
    CHECK(t.Start([&] { ok = ForkAndCheck(); }) == 0); // 非主线程里 fork：子进程里它成了主线程
    t.Join();
    CHECK(ok);
}

} // namespace

int main()
{
    TestTid();
    TestIsMainThread();
    TestName();
    TestNameIsPerThread();
    TestForkResetsCache();
    std::puts("current_thread_test: ok");
    return 0;
}
