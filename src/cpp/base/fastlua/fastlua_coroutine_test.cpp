// 协程辅助测试：yield / resume / done。

#include "base/fastlua/coroutine.h"
#include "base/fastlua/fastlua.h"

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <utility>

using namespace breeze::fastlua;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                                \
        }                                                                                \
    } while (0)

int main() {
    LuaState lua;
    CHECK(lua.RunScript("function co() coroutine.yield(1); return 2 end").Ok());
    Function function = lua.GetFunction("co").Value();
    Result<Coroutine> created = Coroutine::Create(lua, function);
    CHECK(created.Ok());

    Coroutine& coroutine = created.Value();
    auto first = coroutine.Resume();
    CHECK(first.Ok());
    CHECK(first.Value().yielded);
    CHECK(!coroutine.Done());

    auto second = coroutine.Resume();
    CHECK(second.Ok());
    CHECK(!second.Value().yielded);
    CHECK(coroutine.Done());

    auto third = coroutine.Resume();
    CHECK(!third.Ok());

    // 取消：挂起时标记，下一次 Resume 立即返回 kCancelled。
    CHECK(lua.RunScript("function quick() return 1 end").Ok());
    Function quick = lua.GetFunction("quick").Value();
    Result<Coroutine> quick_coroutine = Coroutine::Create(lua, quick);
    CHECK(quick_coroutine.Ok());
    ResumeToken quick_token = quick_coroutine.Value().Token();
    quick_token.Cancel();
    auto cancelled_before_resume = quick_coroutine.Value().Resume();
    CHECK(!cancelled_before_resume.Ok());
    CHECK(cancelled_before_resume.GetError().code == ErrorCode::kCancelled);

    // 运行中取消：count hook 看到原子标志后中断死循环。
    CHECK(lua.RunScript("function spin() coroutine.yield(); while true do end end").Ok());
    Function spin = lua.GetFunction("spin").Value();
    Result<Coroutine> spin_coroutine = Coroutine::Create(lua, spin);
    CHECK(spin_coroutine.Ok());
    ResumeToken spin_token = spin_coroutine.Value().Token();
    auto yielded = spin_coroutine.Value().Resume();
    CHECK(yielded.Ok());
    CHECK(yielded.Value().yielded);
    spin_token.Cancel();
    auto cancelled_running = spin_coroutine.Value().Resume();
    CHECK(!cancelled_running.Ok());
    CHECK(cancelled_running.GetError().code == ErrorCode::kCancelled);

    // Token 可以在任意线程析构：析构只减原子引用计数，不碰 VM。
    ResumeToken moved = spin_coroutine.Value().Token();
    std::thread worker([token = std::move(moved)]() mutable { token = ResumeToken{}; });
    worker.join();

    std::printf("fastlua_coroutine_test: ok\n");
    return 0;
}
