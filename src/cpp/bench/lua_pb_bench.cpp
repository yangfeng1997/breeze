// breeze::lua_pb 基准：编解码三个量级（~64B 标量 / ~1KB 嵌套+repeated / ~16KB bytes）。
// 跑法：cmake --preset release -DBREEZE_BUILD_LUA_PB_BENCH=ON
//       ./build/cpp-release/lua_pb_bench
//
// 只测本库的 C++ API（生产路径就是这样调的），不经过 require("pb")。
// 与 starwing lua-protobuf / quanta luapb 的并排对比在临时工程里做，结论写进
// docs/Lua_pb库设计.md §21；这里的数字是基准，不是承诺。

#include "base/lua_pb/lua_pb.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace breeze::lua_pb;

namespace
{

volatile std::size_t g_sink = 0;

template <class F>
void RunCase(const char* name, int64_t iterations, F&& function)
{
    function(std::max<int64_t>(1, iterations / 10)); // warmup
    std::vector<double> samples;
    samples.reserve(5);
    for (int round = 0; round < 5; ++round)
    {
        const auto start = std::chrono::steady_clock::now();
        function(iterations);
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::nano>(end - start).count() /
                          static_cast<double>(iterations));
    }
    std::sort(samples.begin(), samples.end());
    std::printf("%-34s %10.2f ns/op\n", name, samples[samples.size() / 2]);
}

std::shared_ptr<const Schema> LoadSchema()
{
#ifndef LUA_PB_BENCH_DESCRIPTOR
#error "需要 -DLUA_PB_BENCH_DESCRIPTOR=<path>"
#endif
    std::ifstream in(LUA_PB_BENCH_DESCRIPTOR, std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "打不开描述符：%s\n", LUA_PB_BENCH_DESCRIPTOR);
        std::abort();
    }
    std::vector<std::byte> data;
    char buf[4096];
    while (in.read(buf, sizeof(buf)) || in.gcount() > 0)
        for (std::streamsize i = 0; i < in.gcount(); ++i)
            data.push_back(static_cast<std::byte>(static_cast<unsigned char>(buf[i])));
    auto s = Schema::Load(data);
    if (!s)
    {
        std::fprintf(stderr, "Schema::Load 失败\n");
        std::abort();
    }
    return std::make_shared<const Schema>(std::move(*s));
}

void RunLua(lua_State* L, const std::string& code)
{
    if (luaL_dostring(L, code.c_str()) != 0)
    {
        std::fprintf(stderr, "Lua 出错：%s\n", lua_tostring(L, -1));
        std::abort();
    }
}

// 三类负载：小（标量）、中（嵌套 + repeated）、大（16KB bytes）
const char* kSmall = R"({i32 = -7, i64 = 1 << 40, u64 = -2, ok = true, name = "hello", color = 2})";

const char* kMedium = R"lua((function()
    local items = {}
    for i = 1, 40 do items[i] = {v = i, tag = "item-" .. i} end
    local nums = {}
    for i = 1, 100 do nums[i] = i * 3 end
    return {nums = nums, names = {"alpha", "beta", "gamma"}, flags = {true, false, true}, items = items}
end)())lua";

const char* kLarge = R"({blob = string.rep("x", 16 * 1024), name = "large"})";

std::vector<std::byte> PreparePayload(lua_State* L, Codec& codec, const char* tag, const char* type,
                                      const char* table_code)
{
    lua_settop(L, 0);
    RunLua(L, std::string("t = ") + table_code);
    lua_getglobal(L, "t");
    auto bytes = codec.Encode(type, -1);
    if (!bytes)
    {
        std::fprintf(stderr, "%s 编码失败：%s\n", tag, std::string(ErrorCodeName(bytes.error().code)).c_str());
        std::abort();
    }
    std::vector<std::byte> payload(bytes->begin(), bytes->end());
    lua_pop(L, 1);

    // 往返正确性：编不出来 / 解不回去就别测了
    auto round = codec.Decode(type, payload);
    if (!round)
    {
        std::fprintf(stderr, "%s 解码失败：%s\n", tag, std::string(ErrorCodeName(round.error().code)).c_str());
        std::abort();
    }
    lua_pop(L, 1);
    return payload;
}

void BenchOne(lua_State* L, Codec& codec, const char* tag, const char* type, const char* table_code,
              int64_t iters)
{
    const std::vector<std::byte> payload = PreparePayload(L, codec, tag, type, table_code);
    const std::string prefix = std::string(tag) + " (" + std::to_string(payload.size()) + " B)";
    g_sink += payload.size();

    RunCase((prefix + " encode").c_str(), iters,
            [&](int64_t n)
            {
                lua_getglobal(L, "t");
                for (int64_t i = 0; i < n; ++i)
                {
                    auto r = codec.Encode(type, -1);
                    g_sink += r->size();
                }
                lua_pop(L, 1); // 弹掉表本身（编码不改它）
            });

    RunCase((prefix + " decode").c_str(), iters,
            [&](int64_t n)
            {
                for (int64_t i = 0; i < n; ++i)
                {
                    auto r = codec.Decode(type, payload);
                    lua_pop(L, 1); // 丢掉结果表：让 GC 压力真实存在
                }
            });
}

double LuaHeapKb(lua_State* L)
{
    lua_getglobal(L, "collectgarbage");
    lua_pushstring(L, "count");
    lua_call(L, 1, 1);
    const double kb = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return kb;
}

// 分配压力的粗略代理：停掉 GC 跑 N 次解码，看 Lua 堆净增多少字节/次。
void BenchAlloc(lua_State* L, Codec& codec, const char* tag, const char* type, const char* table_code,
                int64_t iters)
{
    const std::vector<std::byte> payload = PreparePayload(L, codec, tag, type, table_code);
    lua_settop(L, 0);

    RunLua(L, "collectgarbage('collect'); collectgarbage('stop')");
    const double before = LuaHeapKb(L);
    for (int64_t i = 0; i < iters; ++i)
    {
        auto r = codec.Decode(type, payload);
        lua_pop(L, 1);
    }
    const double after = LuaHeapKb(L);
    RunLua(L, "collectgarbage('restart')");
    std::printf("%-34s %10.1f byte/op（Lua 堆增量）\n", (std::string(tag) + " decode 分配").c_str(),
                (after - before) * 1024.0 / static_cast<double>(iters));
}

} // namespace

int main()
{
    auto schema = LoadSchema();
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    {
        Codec codec(L, schema); // 必须先于 lua_close 析构

        BenchOne(L, codec, "small", "lua_pb_test.Scalars", kSmall, 200000);
        BenchOne(L, codec, "medium", "lua_pb_test.Repeated", kMedium, 20000);
        BenchOne(L, codec, "large", "lua_pb_test.Scalars", kLarge, 2000);

        BenchAlloc(L, codec, "medium", "lua_pb_test.Repeated", kMedium, 20000);
    }
    lua_close(L);
    std::printf("sink=%zu\n", g_sink);
    return 0;
}
