// breeze::lua_binding 核心测试：全局读写、调用、多返回值、错误路径、容器与 optional。
// 与 breeze 其余测试一致：不依赖测试框架，CHECK 失败直接 abort。

#include "base/lua_binding/lua_binding.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace breeze::lua_binding;

namespace {
int RawAdd(lua_State* L) {
    lua_pushinteger(L, lua_tointeger(L, 1) + lua_tointeger(L, 2));
    return 1;
}
}  // namespace

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                              \
        }                                                                              \
    } while (0)

int main() {
    LuaState lua;

    // 全局读写
    lua.Set("answer", 42);
    auto answer = lua.Get<int>("answer");
    CHECK(answer.has_value());
    CHECK(ValueOrDie(answer) == 42);

    lua.Set("name", std::string("breeze"));
    auto name = lua.Get<std::string>("name");
    CHECK(name.has_value());
    CHECK(ValueOrDie(name) == "breeze");

    lua.Set("flag", true);
    auto flag = lua.Get<bool>("flag");
    CHECK(flag.has_value());
    CHECK(ValueOrDie(flag));

    // C++ 函数绑定（lambda）
    lua.Set("add", [](int a, int b) { return a + b; });
    auto add = lua.Call<int>("add", 20, 22);
    CHECK(add.has_value());
    CHECK(ValueOrDie(add) == 42);

    // 裸 lua_CFunction 绑定：最热路径的逃生门。
    lua.SetRawFunction("raw_add", &RawAdd);
    auto raw_add = lua.Call<int>("raw_add", 20, 22);
    CHECK(raw_add.has_value());
    CHECK(ValueOrDie(raw_add) == 42);

    // 缺函数是 NotFound，不是运行时错误
    auto missing = lua.Call<int>("missing");
    CHECK(!missing.has_value());
    CHECK(IsNotFound(missing));

    // 参数类型错误
    auto type_error = lua.Call<int>("add", "x", 2);
    CHECK(!type_error.has_value());
    CHECK(!IsNotFound(type_error));

    // 脚本与普通 Lua 函数调用
    auto script = lua.RunScript("function mul(a, b) return a * b end");
    CHECK(script.has_value());
    auto mul = lua.Call<int>("mul", 6, 7);
    CHECK(mul.has_value());
    CHECK(ValueOrDie(mul) == 42);

    auto noop_script = lua.RunScript("function noop() end");
    CHECK(noop_script.has_value());
    auto noop = lua.Call<void>("noop");
    CHECK(noop.has_value());

    // 脚本错误
    auto script_error = lua.RunScript("error('boom')");
    CHECK(!script_error.has_value());
    CHECK(script_error.error().code == ErrorCode::kLuaError);
    CHECK(script_error.error().message.find("boom") != std::string::npos);

    // 多返回值（tuple）
    lua.Set("divmod", [](int a, int b) { return std::tuple<int, int>{a / b, a % b}; });
    auto divmod = lua.Call<std::tuple<int, int>>("divmod", 7, 3);
    CHECK(divmod.has_value());
    CHECK(std::get<0>(ValueOrDie(divmod)) == 2);
    CHECK(std::get<1>(ValueOrDie(divmod)) == 1);

    // optional 与 nil
    lua.Set("maybe", std::optional<int>{});
    auto maybe = lua.Get<std::optional<int>>("maybe");
    CHECK(maybe.has_value());
    CHECK(!ValueOrDie(maybe).has_value());

    // vector 往返
    lua.Set("vec", std::vector<int>{1, 2, 3});
    auto vec = lua.Get<std::vector<int>>("vec");
    CHECK(vec.has_value());
    CHECK(ValueOrDie(vec).size() == 3);
    CHECK(ValueOrDie(vec)[2] == 3);

    // array 往返；长度必须精确匹配。
    lua.Set("arr", std::array<int, 3>{4, 5, 6});
    auto arr = lua.Get<std::array<int, 3>>("arr");
    CHECK(arr.has_value());
    CHECK(ValueOrDie(arr)[0] == 4);
    CHECK(ValueOrDie(arr)[2] == 6);
    auto wrong_array = lua.Get<std::array<int, 2>>("arr");
    CHECK(!wrong_array.has_value());
    CHECK(wrong_array.error().code == ErrorCode::kType);

    // span 是 push-only：C++ 连续区间推给 Lua；不作为入参。
    const std::array<int, 3> span_source{7, 8, 9};
    lua.Set("span_values", std::span<const int>(span_source));
    CHECK(lua.RunScript("assert(span_values[1] == 7 and span_values[3] == 9 and #span_values == 3)").has_value());

    // unordered_map 往返。
    const std::unordered_map<std::string, int> unordered{{"a", 1}, {"b", 2}};
    lua.Set("unordered", unordered);
    auto unordered_back = lua.Get<std::unordered_map<std::string, int>>("unordered");
    CHECK(unordered_back.has_value());
    CHECK(ValueOrDie(unordered_back).size() == 2);
    CHECK(ValueOrDie(unordered_back).at("a") == 1);
    CHECK(ValueOrDie(unordered_back).at("b") == 2);

    // registry 引用：LuaTable / LuaFunction
    CHECK(lua.RunScript("bench = {a = 1, b = 2}; function double(x) return x * 2 end").has_value());

    LuaTable bench = ValueOrDie(lua.GetTable("bench"));
    auto a_value = bench.Get<int>("a");
    CHECK(a_value.has_value());
    CHECK(ValueOrDie(a_value) == 1);
    bench.Set("c", 3);
    auto c_value = bench.Get<int>("c");
    CHECK(c_value.has_value());
    CHECK(ValueOrDie(c_value) == 3);
    CHECK(IsNotFound(bench.Get<int>("missing")));

    LuaFunction double_fn = ValueOrDie(lua.GetFunction("double"));
    auto doubled = double_fn.Call<int>(21);
    CHECK(doubled.has_value());
    CHECK(ValueOrDie(doubled) == 42);

    CHECK(lua.RunScript("bench.plus_one = function(x) return x + 1 end").has_value());
    auto plus_one = bench.Call<int>("plus_one", 41);
    CHECK(plus_one.has_value());
    CHECK(ValueOrDie(plus_one) == 42);

    // 显式 Clone：新建独立 registry 槽，原引用析构后 clone 仍然有效。
    LuaFunction double_copy = double_fn.Clone();
    CHECK(double_copy.Valid());
    CHECK(ValueOrDie(double_copy.Call<int>(5)) == 10);
    double_fn = LuaFunction{};
    CHECK(ValueOrDie(double_copy.Call<int>(6)) == 12);

    LuaTable bench_copy = bench.Clone();
    bench_copy.Set("from_clone", 9);
    auto from_clone = bench.Get<int>("from_clone");
    CHECK(from_clone.has_value());
    CHECK(ValueOrDie(from_clone) == 9);

    // 空引用 Clone 返回空，不碰 VM。
    CHECK(!LuaTable{}.Clone().Valid());
    CHECK(!LuaFunction{}.Clone().Valid());

    // 返回值类型不匹配必须是 kType，不能静默变成 0 / false / 空串。
    LuaFunction double_again = ValueOrDie(lua.GetFunction("double"));
    auto wrong_bool = double_again.Call<bool>(1);
    CHECK(!wrong_bool.has_value());
    CHECK(wrong_bool.error().code == ErrorCode::kType);
    auto fast_wrong = double_again.CallNoTraceback<bool>(1);
    CHECK(!fast_wrong.has_value());
    CHECK(fast_wrong.error().code == ErrorCode::kType);
    auto wrong_global = lua.Call<bool>("double", 1);
    CHECK(!wrong_global.has_value());
    CHECK(wrong_global.error().code == ErrorCode::kType);
    auto missing_tuple = lua.Call<std::tuple<int, int>>("double", 1);
    CHECK(!missing_tuple.has_value());
    CHECK(missing_tuple.error().code == ErrorCode::kType);

    // 无保护的逃生门：函数确定不报错时用；参数/返回值仍然经过 Stack 转换。
    CHECK(double_again.CallUnprotected<int>(9) == 18);
    CHECK(lua.CallUnprotected<int>("double", 10) == 20);
    CHECK(lua.RunScript("function noop() end").has_value());
    lua.CallUnprotected<void>("noop");

    // lua_close 之后仍存活的 Reference 析构必须安全（StateData 的 __gc 已经把 state 置空）。
    LuaFunction outlives;
    {
        LuaState other;
        CHECK(other.RunScript("function late() return 1 end").has_value());
        outlives = ValueOrDie(other.GetFunction("late"));
        CHECK(outlives.Valid());
    }
    CHECK(!outlives.Valid());
    auto after_close = outlives.Call<int>();
    CHECK(!after_close.has_value());
    CHECK(after_close.error().code == ErrorCode::kInvalidState);

    // 可调用对象的生命周期：去掉 Lua 引用并 GC 后，捕获的 shared_ptr 应该被释放。
    {
        auto held = std::make_shared<int>(7);
        lua.SetFunction("held", [held]() { return *held; });
        CHECK(held.use_count() == 2);
        CHECK(lua.RunScript("held = nil; collectgarbage('collect')").has_value());
        CHECK(held.use_count() == 1);
    }

#if defined(__cpp_exceptions)
    // C++ 异常在 Invoke 里被转换成 ErrorSlot，不能穿过 Lua 的 C 帧。
    lua.Set("cpp_throw", []() -> int { throw std::runtime_error("cpp boom"); });
    auto thrown = lua.Call<int>("cpp_throw");
    CHECK(!thrown.has_value());
    CHECK(thrown.error().code == ErrorCode::kLuaError);
    CHECK(thrown.error().message.find("cpp boom") != std::string::npos);

    // Lua 调 C++、C++ 抛异常、再回到 Lua：错误应该变成 Lua 错误，而不是崩溃。
    CHECK(lua.RunScript("function call_throw() return cpp_throw() end").has_value());
    auto nested = lua.Call<int>("call_throw");
    CHECK(!nested.has_value());
    CHECK(nested.error().code == ErrorCode::kLuaError);
    CHECK(nested.error().message.find("cpp boom") != std::string::npos);
#endif

    std::printf("lua_core_test: ok\n");
    return 0;
}
