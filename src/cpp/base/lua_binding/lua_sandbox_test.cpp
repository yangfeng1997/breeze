// 沙箱测试：内存上限、指令预算、库白名单。

#include "base/lua_binding/lua_binding.h"
#include "base/lua_binding/sandbox.h"

#include <cstdio>
#include <cstdlib>

using namespace breeze::lua_binding;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                                \
        }                                                                                \
    } while (0)

int main() {
    // 内存上限
    {
        AllocStats stats;
        SandboxOptions options;
        options.memory_limit = 256 * 1024;
        LuaState lua(MakeSandboxOptions(options, stats));
        ApplySandbox(lua, options);
        const auto result = lua.RunScript("local s = string.rep('x', 1024 * 1024)");
        CHECK(!result.Ok());
        CHECK(stats.failures > 0);
    }

    // 指令预算
    {
        LuaState lua;
        SandboxOptions options;
        options.instruction_budget = 1'000'000;
        ApplySandbox(lua, options);
        const auto result = lua.RunScript("while true do end");
        CHECK(!result.Ok());
    }

    // 库白名单：io/os/debug 不打开
    {
        AllocStats stats;
        SandboxOptions options;
        LuaState lua(MakeSandboxOptions(options, stats));
        ApplySandbox(lua, options);
        CHECK(lua.RunScript("assert(io == nil and os == nil and debug == nil)").Ok());
    }

    std::printf("lua_sandbox_test: ok\n");
    return 0;
}
