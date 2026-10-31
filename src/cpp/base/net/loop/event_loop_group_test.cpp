// EventLoopGroup：选项校验、注册表用满、线程创建失败后的回滚（无线程 / fd 泄漏）、PickLoop、Shutdown 语义。

#include "base/net/loop/event_loop_group.h"

#include <sys/resource.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "base/net/loop/group_access.h"
#include "base/net/loop/group_registry.h"
#include "base/net/testing/net_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

void TestInvalidOptions()
{
    const LeakCheck leak;
    const EventLoopGroupOptions bad[] = {
        {0, 16},
        {ConnectionHandle::kMaxLoops + 1, 16},
        {1, 0},
        {1, ConnectionHandle::kMaxSlots + 1},
    };
    for (const auto& o : bad)
    {
        EventLoopGroup g(o);
        CHECK(g.Start() == EINVAL);
        CHECK(!g.running());
        g.Shutdown(0ms); // 没有成功 Start 的对象：安全的空操作
    }
    // 边界值合法
    EventLoopGroup lo({1, 1});
    CHECK(lo.Start() == 0);
    lo.Shutdown(0ms);
    EventLoopGroup hi({ConnectionHandle::kMaxLoops, 16});
    CHECK(hi.Start() == 0);
    hi.Shutdown(0ms);
}

void TestEachLoopRunsOnItsOwnThread()
{
    const LeakCheck leak;
    constexpr std::uint32_t kLoops = 4;
    EventLoopGroup g({kLoops, 64});
    CHECK(g.Start() == 0);
    CHECK(g.running());
    CHECK(GroupAccess::LoopCount(g) == kLoops);
    CHECK(GroupRegistry::Global().Find(GroupAccess::Id(g)) == &g);

    std::mutex mu;
    std::set<std::thread::id> tids;
    for (std::uint32_t i = 0; i < kLoops; ++i)
    {
        EventLoop& l = GroupAccess::LoopAt(g, i);
        CHECK(l.options().group_id == GroupAccess::Id(g) && l.options().index == i);
        RunOnLoop(l,
                  [&]
                  {
                      const std::lock_guard lk(mu);
                      tids.insert(std::this_thread::get_id());
                  });
    }
    CHECK(tids.size() == kLoops);
    CHECK(tids.count(std::this_thread::get_id()) == 0);
    CHECK(GroupAccess::TryLoopAt(g, kLoops) == nullptr);
}

void TestShutdownIsIdempotentAndUnregisters()
{
    const LeakCheck leak;
    EventLoopGroup g({2, 64});
    CHECK(g.Start() == 0);
    const std::uint32_t id = GroupAccess::Id(g);
    g.Shutdown(0ms);
    CHECK(!g.running());
    CHECK(GroupRegistry::Global().Find(id) == nullptr); // 之后句柄解析不到
    g.Shutdown(0ms);                                    // 重复调用
}

// 析构兜底：没 Shutdown 过也要 join 线程、归还注册表名额。
void TestDestructorStopsEverything()
{
    const LeakCheck leak;
    std::uint32_t id = 0;
    {
        EventLoopGroup g({3, 64});
        CHECK(g.Start() == 0);
        id = GroupAccess::Id(g);
    }
    CHECK(GroupRegistry::Global().Find(id) == nullptr);
}

// 注册表用满：第 17 个 group 得到 ENOSPC，且没有留下任何线程或 fd；释放一个名额后可以启动。
void TestRegistryFull()
{
    std::vector<std::unique_ptr<EventLoopGroup>> groups;
    for (std::uint32_t i = 0; i < GroupRegistry::kCapacity; ++i)
    {
        groups.push_back(std::make_unique<EventLoopGroup>(EventLoopGroupOptions{1, 8}));
        CHECK(groups.back()->Start() == 0);
    }
    {
        const LeakCheck leak;
        EventLoopGroup extra({2, 8});
        CHECK(extra.Start() == ENOSPC);
        CHECK(!extra.running());
    }
    groups.back()->Shutdown(0ms);
    groups.pop_back();
    EventLoopGroup again({1, 8});
    CHECK(again.Start() == 0);
}

// 选连接数最少的 loop。
void TestPickLoopChoosesLeastLoaded()
{
    EventLoopGroup g({3, 16});
    CHECK(g.Start() == 0);
    EventLoop& l0 = GroupAccess::LoopAt(g, 0);
    EventLoop& l1 = GroupAccess::LoopAt(g, 1);
    EventLoop& l2 = GroupAccess::LoopAt(g, 2);

    CHECK(l0.table().TryReserve());
    CHECK(l0.table().TryReserve());
    CHECK(l1.table().TryReserve());
    CHECK(&GroupAccess::PickLoop(g) == &l2); // 0 个

    for (int i = 0; i < 3; ++i)
        CHECK(l2.table().TryReserve());
    CHECK(&GroupAccess::PickLoop(g) == &l1); // 1 个

    for (EventLoop* l : {&l0, &l1, &l2})
        while (l->table().Live() > 0)
            l->table().Unreserve();
}

// 当前用户的线程总数（RLIMIT_NPROC 按它算）。
long CountUserThreads()
{
    long total = 0;
    if (DIR* d = ::opendir("/proc"))
    {
        while (const dirent* e = ::readdir(d))
        {
            if (e->d_name[0] < '0' || e->d_name[0] > '9')
                continue;
            char path[64];
            std::snprintf(path, sizeof(path), "/proc/%s/status", e->d_name);
            FILE* f = std::fopen(path, "r");
            if (f == nullptr)
                continue;
            char line[256];
            long uid = -1, threads = 0;
            while (std::fgets(line, sizeof(line), f) != nullptr)
            {
                if (std::strncmp(line, "Uid:", 4) == 0)
                    uid = std::strtol(line + 4, nullptr, 10);
                else if (std::strncmp(line, "Threads:", 8) == 0)
                    threads = std::strtol(line + 8, nullptr, 10);
            }
            std::fclose(f);
            if (uid == static_cast<long>(::getuid()))
                total += threads;
        }
        ::closedir(d);
    }
    return total;
}

// 线程创建中途失败（第 3 个起）：先 join 已起的线程、关掉已开的 fd 再返回 errno；注册表名额归还。
// RLIMIT_NPROC 对 root / CAP_SYS_RESOURCE 不生效，那种环境下探测不到失败就跳过。
void TestThreadCreateFailureRollsBack()
{
    rlimit old{};
    CHECK(::getrlimit(RLIMIT_NPROC, &old) == 0);
    rlimit low = old;
    low.rlim_cur = static_cast<rlim_t>(CountUserThreads() + 2); // 再多两个线程就到顶
    CHECK(::setrlimit(RLIMIT_NPROC, &low) == 0);

    int rc;
    {
        const LeakCheck leak;
        EventLoopGroup g({16, 8});
        rc = g.Start();
        if (rc == 0)
            g.Shutdown(0ms);
        else
            CHECK(!g.running());
    }
    CHECK(::setrlimit(RLIMIT_NPROC, &old) == 0);

    if (rc == 0)
    {
        std::puts("  RLIMIT_NPROC 没有生效（root？），跳过线程创建失败用例");
        return;
    }
    CHECK(rc == EAGAIN);
    // 名额已归还：能再启动一个正常的 group
    EventLoopGroup ok({1, 8});
    CHECK(ok.Start() == 0);
}

} // namespace

int main()
{
    TestInvalidOptions();
    TestEachLoopRunsOnItsOwnThread();
    TestShutdownIsIdempotentAndUnregisters();
    TestDestructorStopsEverything();
    TestRegistryFull();
    TestPickLoopChoosesLeastLoaded();
    TestThreadCreateFailureRollsBack();
    std::puts("event_loop_group_test: ok");
    return 0;
}
