// 协程辅助测试：yield / resume / done。

#include "base/lua_binding/coroutine.h"
#include "base/lua_binding/lua_binding.h"

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <utility>

using namespace breeze::lua_binding;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                                \
        }                                                                                \
    } while (0)

int main() {
    LuaState lua;
    CHECK(lua.RunScript("function co() coroutine.yield(1); return 2 end").has_value());
    LuaFunction function = ValueOrDie(lua.GetFunction("co"));
    std::expected<Coroutine, Error> created = Coroutine::Create(lua, function);
    CHECK(created.has_value());

    Coroutine& coroutine = ValueOrDie(created);
    auto first = coroutine.Resume();
    CHECK(first.has_value());
    CHECK(ValueOrDie(first).yielded);
    CHECK(!coroutine.Done());

    auto second = coroutine.Resume();
    CHECK(second.has_value());
    CHECK(!ValueOrDie(second).yielded);
    CHECK(coroutine.Done());

    auto third = coroutine.Resume();
    CHECK(!third.has_value());
    CHECK(third.error().code == ErrorCode::kInvalidState);

    // 取消：挂起时标记，下一次 Resume 立即返回 kCancelled。
    CHECK(lua.RunScript("function quick() return 1 end").has_value());
    LuaFunction quick = ValueOrDie(lua.GetFunction("quick"));
    std::expected<Coroutine, Error> quick_coroutine = Coroutine::Create(lua, quick);
    CHECK(quick_coroutine.has_value());
    ResumeToken quick_token = ValueOrDie(quick_coroutine).Token();
    quick_token.Cancel();
    auto cancelled_before_resume = ValueOrDie(quick_coroutine).Resume();
    CHECK(!cancelled_before_resume.has_value());
    CHECK(cancelled_before_resume.error().code == ErrorCode::kCancelled);

    // 运行中取消：count hook 看到原子标志后中断死循环。
    CHECK(lua.RunScript("function spin() coroutine.yield(); while true do end end").has_value());
    LuaFunction spin = ValueOrDie(lua.GetFunction("spin"));
    std::expected<Coroutine, Error> spin_coroutine = Coroutine::Create(lua, spin);
    CHECK(spin_coroutine.has_value());
    ResumeToken spin_token = ValueOrDie(spin_coroutine).Token();
    auto yielded = ValueOrDie(spin_coroutine).Resume();
    CHECK(yielded.has_value());
    CHECK(ValueOrDie(yielded).yielded);
    spin_token.Cancel();
    auto cancelled_running = ValueOrDie(spin_coroutine).Resume();
    CHECK(!cancelled_running.has_value());
    CHECK(cancelled_running.error().code == ErrorCode::kCancelled);

    // Token 可以在任意线程析构：析构只减原子引用计数，不碰 VM。
    ResumeToken moved = ValueOrDie(spin_coroutine).Token();
    std::thread worker([token = std::move(moved)]() mutable { token = ResumeToken{}; });
    worker.join();

    std::printf("lua_coroutine_test: ok\n");
    return 0;
}
