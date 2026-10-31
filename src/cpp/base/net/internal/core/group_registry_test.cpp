// group 注册表：占位 / 注销 / 用满，以及并发占位时同一编号不会同时给两个 group。

#include <atomic>
#include <cerrno>
#include <memory>
#include <thread>
#include <vector>

#include "event_loop_group.h"
#include "group_registry.h"
#include "net_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{

// 没有 Start 的 group 什么都不创建，只是一个有地址的对象。
std::vector<std::unique_ptr<EventLoopGroup>> MakeGroups(std::size_t n)
{
    std::vector<std::unique_ptr<EventLoopGroup>> v;
    for (std::size_t i = 0; i < n; ++i)
        v.push_back(std::make_unique<EventLoopGroup>());
    return v;
}

void TestRegisterUntilFull()
{
    GroupRegistry reg; // 自己的实例，不占进程级的名额
    auto groups = MakeGroups(GroupRegistry::kCapacity + 1);
    for (std::uint32_t i = 0; i < GroupRegistry::kCapacity; ++i)
    {
        std::uint32_t id = 99;
        CHECK(reg.Register(groups[i].get(), id) == 0);
        CHECK(id == i); // 占第一个空位
        CHECK(reg.Find(id) == groups[i].get());
    }
    std::uint32_t id = 99;
    CHECK(reg.Register(groups[GroupRegistry::kCapacity].get(), id) == ENOSPC);
    CHECK(id == 99); // 失败不改输出
}

void TestUnregisterFreesFirstSlot()
{
    GroupRegistry reg;
    auto groups = MakeGroups(GroupRegistry::kCapacity + 1);
    std::uint32_t id = 0;
    for (std::uint32_t i = 0; i < GroupRegistry::kCapacity; ++i)
        CHECK(reg.Register(groups[i].get(), id) == 0);

    reg.Unregister(5);
    reg.Unregister(3);
    CHECK(reg.Find(3) == nullptr);
    CHECK(reg.Find(5) == nullptr);
    CHECK(reg.Register(groups[GroupRegistry::kCapacity].get(), id) == 0);
    CHECK(id == 3); // 第一个空位，不是最近释放的
}

void TestFindOutOfRangeIsNull()
{
    GroupRegistry reg;
    CHECK(reg.Find(0) == nullptr); // 空
    CHECK(reg.Find(GroupRegistry::kCapacity) == nullptr);
    CHECK(reg.Find(UINT32_MAX) == nullptr);
    reg.Unregister(GroupRegistry::kCapacity); // 越界的注销是空操作
    reg.Unregister(UINT32_MAX);
}

// 多线程反复占位 / 注销：同一个编号任何时刻只属于一个持有者。
void TestConcurrentRegister()
{
    GroupRegistry reg;
    std::array<std::atomic<const void*>, GroupRegistry::kCapacity> owner{};
    constexpr int kThreads = 8;
    constexpr int kRounds = 20000;
    auto groups = MakeGroups(kThreads);

    std::vector<std::thread> ths;
    for (int t = 0; t < kThreads; ++t)
    {
        ths.emplace_back(
            [&, t]
            {
                EventLoopGroup* me = groups[static_cast<std::size_t>(t)].get();
                for (int i = 0; i < kRounds; ++i)
                {
                    std::uint32_t id = 0;
                    if (reg.Register(me, id) != 0)
                        continue; // 8 个线程抢 16 个位，不会满；保险起见
                    CHECK(reg.Find(id) == me);
                    CHECK(owner[id].exchange(me) == nullptr); // 之前没有别人持有这个编号
                    owner[id].store(nullptr);
                    reg.Unregister(id);
                }
            });
    }
    for (auto& th : ths)
        th.join();
    for (std::uint32_t i = 0; i < GroupRegistry::kCapacity; ++i)
        CHECK(reg.Find(i) == nullptr);
}

} // namespace

int main()
{
    TestRegisterUntilFull();
    TestUnregisterFreesFirstSlot();
    TestFindOutOfRangeIsNull();
    TestConcurrentRegister();
    std::puts("group_registry_test: ok");
    return 0;
}
