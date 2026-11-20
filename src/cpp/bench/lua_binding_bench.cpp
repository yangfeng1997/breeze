// breeze::lua_binding 基准：只依赖 breeze::lua_binding 自身与 Lua，可单独构建。
// 跑法：cmake --preset release -DBREEZE_BUILD_LUA_BINDING_BENCH=ON
//       ./build/cpp-release/lua_binding_bench
// 竞品对比见 docs/Lua绑定库设计.md 的实测表（Windows 上的临时对比基准）。

#include "base/lua_binding/ext/ext.h"
#include "base/lua_binding/lua_binding.h"
#include "base/lua_binding/sandbox.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace breeze::lua_binding;

namespace {

volatile int64_t g_sink = 0;

// 裸 lua_CFunction：对照组，测“绑定的固定开销”，不代表通用绑定的安全性。
int RawCppAdd(lua_State* L) {
    lua_pushinteger(L, lua_tointeger(L, 1) + lua_tointeger(L, 2));
    return 1;
}

int RawCppReadTable(lua_State* L) {
    lua_Integer sum = 0;
    for (const char* key : {"a", "b", "c", "d"}) {
        lua_getfield(L, 1, key);
        sum += lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    lua_pushinteger(L, sum);
    return 1;
}

template <class F>
void RunCase(const char* name, int64_t iterations, F&& function) {
    function(std::max<int64_t>(1, iterations / 10));  // warmup
    std::vector<double> samples;
    samples.reserve(5);
    for (int round = 0; round < 5; ++round) {
        const auto start = std::chrono::steady_clock::now();
        function(iterations);
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::nano>(end - start).count() /
                          static_cast<double>(iterations));
    }
    std::sort(samples.begin(), samples.end());
    std::printf("%-30s %10.2f ns/op\n", name, samples[samples.size() / 2]);
}

}  // namespace

int main() {
    AllocStats stats;
    SandboxOptions options;
    LuaState lua(MakeSandboxOptions(options, stats));

    const auto script = lua.RunScript(R"lua(
        function lua_noop() end
        function lua_add(a, b) return a + b end
        function lua_loop(n)
            local s = 0
            for i = 1, n do s = s + i end
            return s
        end
        bench_table = {a = 1, b = 2, c = 3, d = 4}
        function lua_read_table(t)
            return t.a + t.b + t.c + t.d
        end
        function lua_call_cpp(n)
            local s = 0
            for i = 1, n do s = s + cpp_add(i, 1) end
            return s
        end
        function lua_call_cpp_table(n)
            local s = 0
            for i = 1, n do s = s + cpp_read_table(bench_table) end
            return s
        end
        function lua_call_cpp_raw(n)
            local s = 0
            for i = 1, n do s = s + cpp_add_raw(i, 1) end
            return s
        end
        function lua_call_cpp_table_raw(n)
            local s = 0
            for i = 1, n do s = s + cpp_read_table_raw(bench_table) end
            return s
        end
    )lua");
    if (!script.has_value()) {
        std::fprintf(stderr, "load script failed: %s\n", script.error().message.c_str());
        return 1;
    }

    lua.Set("cpp_add", [](int a, int b) { return a + b; });
    lua.Set("cpp_read_table", [](LuaTableView table) {
        int a = 0;
        int b = 0;
        int c = 0;
        int d = 0;
        table.TryGetField("a", a);
        table.TryGetField("b", b);
        table.TryGetField("c", c);
        table.TryGetField("d", d);
        return a + b + c + d;
    });

    LuaFunction noop = ValueOrDie(lua.GetFunction("lua_noop"));
    LuaFunction add = ValueOrDie(lua.GetFunction("lua_add"));
    LuaFunction loop = ValueOrDie(lua.GetFunction("lua_loop"));
    LuaFunction call_cpp = ValueOrDie(lua.GetFunction("lua_call_cpp"));
    LuaFunction call_cpp_table = ValueOrDie(lua.GetFunction("lua_call_cpp_table"));
    LuaFunction call_cpp_raw = ValueOrDie(lua.GetFunction("lua_call_cpp_raw"));
    LuaFunction call_cpp_table_raw = ValueOrDie(lua.GetFunction("lua_call_cpp_table_raw"));

    lua.SetRawFunction("cpp_add_raw", &RawCppAdd);
    lua.SetRawFunction("cpp_read_table_raw", &RawCppReadTable);
    LuaTable table = ValueOrDie(lua.GetTable("bench_table"));

    constexpr int64_t kIterations = 2'000'000;

    RunCase("lua_vm_loop", kIterations, [&](int64_t n) {
        const auto result = loop.Call<void>(n);
        (void)result;
    });

    RunCase("cpp2lua_noop_cached", kIterations, [&](int64_t n) {
        for (int64_t i = 0; i < n; ++i) {
            const auto result = noop.Call<void>();
            (void)result;
        }
    });

    RunCase("cpp2lua_noop_fast", kIterations, [&](int64_t n) {
        for (int64_t i = 0; i < n; ++i) {
            const auto result = noop.CallNoTraceback<void>();
            (void)result;
        }
    });

    RunCase("cpp2lua_add_cached", kIterations, [&](int64_t n) {
        for (int64_t i = 0; i < n; ++i) {
            auto result = add.Call<int>(1, 2);
            g_sink += ValueOrDie(result);
        }
    });

    RunCase("cpp2lua_add_fast", kIterations, [&](int64_t n) {
        for (int64_t i = 0; i < n; ++i) {
            auto result = add.CallNoTraceback<int>(1, 2);
            g_sink += ValueOrDie(result);
        }
    });

    RunCase("cpp2lua_noop_unprotected", kIterations, [&](int64_t n) {
        for (int64_t i = 0; i < n; ++i) {
            noop.CallUnprotected<void>();
        }
    });

    RunCase("cpp2lua_add_unprotected", kIterations, [&](int64_t n) {
        for (int64_t i = 0; i < n; ++i) {
            g_sink += add.CallUnprotected<int>(1, 2);
        }
    });

    RunCase("cpp2lua_add_by_name", kIterations, [&](int64_t n) {
        for (int64_t i = 0; i < n; ++i) {
            auto result = lua.Call<int>("lua_add", 1, 2);
            g_sink += ValueOrDie(result);
        }
    });

    RunCase("cpp2lua_add_by_name_unprotected", kIterations, [&](int64_t n) {
        for (int64_t i = 0; i < n; ++i) {
            g_sink += lua.CallUnprotected<int>("lua_add", 1, 2);
        }
    });

    RunCase("cpp2lua_table_get", kIterations, [&](int64_t n) {
        for (int64_t i = 0; i < n; ++i) {
            auto result = table.Get<int>("a");
            g_sink += ValueOrDie(result);
        }
    });

    RunCase("lua2cpp_add", kIterations, [&](int64_t n) {
        const auto result = call_cpp.Call<void>(n);
        (void)result;
    });

    RunCase("lua2cpp_table", kIterations, [&](int64_t n) {
        const auto result = call_cpp_table.Call<void>(n);
        (void)result;
    });

    RunCase("lua2cpp_add_raw", kIterations, [&](int64_t n) {
        const auto result = call_cpp_raw.Call<void>(n);
        (void)result;
    });

    RunCase("lua2cpp_table_raw", kIterations, [&](int64_t n) {
        const auto result = call_cpp_table_raw.Call<void>(n);
        (void)result;
    });

    // 热路径零分配：预热后统计 100k 次缓存的 C++ -> Lua 调用新增的 Lua 分配次数。
    for (int i = 0; i < 1000; ++i) {
        (void)add.Call<int>(1, 2);
    }
    const std::size_t before = stats.allocations;
    for (int i = 0; i < 100'000; ++i) {
        g_sink += ValueOrDie(add.Call<int>(1, 2));
    }
    std::printf("cpp2lua_add_cached allocations: %zu per 100k calls\n", stats.allocations - before);

    std::printf("lua_binding_bench: done\n");
    return 0;
}
