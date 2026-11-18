// 绑定层：require("pb")、pb.encode / pb.decode、错误路径、以及"多线程各用各的 state + 共享 Schema"的冒烟。
// 无测试框架，失败即 abort。tsan 本机跑不了（见 docs/Lua_pb库设计.md §19 的说明），这里只做功能冒烟。

#include "base/lua_pb/lua_pb.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if defined(LUA_PB_TEST_ENABLE_THREADS)
#include <atomic>
#include <functional>
#include <thread>
#endif

using namespace breeze::lua_pb;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

#ifndef LUA_PB_TEST_DESCRIPTOR
#error "需要 -DLUA_PB_TEST_DESCRIPTOR=<path>"
#endif

namespace
{

std::shared_ptr<const Schema> LoadSchema()
{
    std::ifstream in(LUA_PB_TEST_DESCRIPTOR, std::ios::binary);
    CHECK(in.good());
    std::vector<std::byte> data;
    char buf[4096];
    while (in.read(buf, sizeof(buf)) || in.gcount() > 0)
        for (std::streamsize i = 0; i < in.gcount(); ++i)
            data.push_back(static_cast<std::byte>(static_cast<unsigned char>(buf[i])));
    auto s = Schema::Load(data);
    CHECK(s.has_value());
    return std::make_shared<const Schema>(std::move(*s));
}

void RunLua(lua_State* L, const char* code)
{
    if (luaL_dostring(L, code) != 0)
    {
        std::fprintf(stderr, "Lua 出错：%s\n", lua_tostring(L, -1));
        std::abort();
    }
}

const char* kScript = R"lua(
local pb = require("pb")
assert(type(pb) == "table" and type(pb.encode) == "function" and type(pb.decode) == "function")
assert(require("pb") == pb, "重复 require 必须是同一张表")

-- 黄金字节：{i32 = 150} → 08 96 01
local s = pb.encode("lua_pb_test.Scalars", {i32 = 150})
assert(type(s) == "string" and #s == 3, "encode 应返回字符串")
assert(s:byte(1) == 0x08 and s:byte(2) == 0x96 and s:byte(3) == 0x01, "encode 字节不对")

-- 解码：隐式默认值也在表里；optional 缺失是 nil
local t = pb.decode("lua_pb_test.Scalars", s)
assert(t.i32 == 150)
assert(t.name == "" and t.ok == false and t.color == 0)
local o = pb.decode("lua_pb_test.Optionals", "")
assert(o.maybe == nil)

-- 富表往返：编码 → 解码 → 再编码，字节一致
local rich = {
    nums = {1, 2, 3}, names = {"a", "b"}, flags = {true, false},
    items = {{v = 1}, {v = 2, tag = "t"}},
}
local b1 = pb.encode("lua_pb_test.Repeated", rich)
local back = pb.decode("lua_pb_test.Repeated", b1)
local b2 = pb.encode("lua_pb_test.Repeated", back)
assert(b1 == b2, "往返后字节必须一致")
assert(#back.nums == 3 and back.nums[3] == 3 and #back.items == 2 and back.items[2].tag == "t")

-- map 往返
local m1 = pb.encode("lua_pb_test.Maps", {counts = {a = 1, b = 2}})
local m2 = pb.decode("lua_pb_test.Maps", m1)
assert(m2.counts.a == 1 and m2.counts.b == 2)

-- 错误路径：坏字节、未知字段名、oneof 冲突、未知类型名、参数类型不对
local ok, err = pcall(pb.decode, "lua_pb_test.Scalars", "\8")
assert(not ok and err:find("truncated"), "坏字节应抛 truncated：" .. tostring(err))

ok, err = pcall(pb.decode, "lua_pb_test.Scalars", "\8\128") -- 0x80 是续行字节，后面没有后续字节
assert(not ok, "截断的 varint 应报错")

ok, err = pcall(pb.encode, "lua_pb_test.Scalars", {nope = 1})
assert(not ok and err:find("unknown field"), "未知字段名应报错：" .. tostring(err))

ok, err = pcall(pb.encode, "lua_pb_test.Oneofs", {a = 1, b = "x"})
assert(not ok and err:find("oneof conflict"), "oneof 冲突应报错：" .. tostring(err))

ok, err = pcall(pb.decode, "NoSuch.Type", "")
assert(not ok and err:find("unknown type"), "未知类型名应报错：" .. tostring(err))

ok, err = pcall(pb.encode, 42, {})
assert(not ok, "类型名必须是字符串")

ok, err = pcall(pb.encode, "lua_pb_test.Scalars", 42)
assert(not ok, "要编码的必须是 table")

ok, err = pcall(pb.decode, "lua_pb_test.Scalars", 42)
assert(not ok, "payload 必须是 string")

-- 深度上限：套 150 层
local deep = {}
local cur = deep
for _ = 1, 150 do
    cur.next = {}
    cur = cur.next
end
ok, err = pcall(pb.encode, "lua_pb_test.Deep", deep)
assert(not ok and err:find("depth exceeded"), "超深度应报错：" .. tostring(err))
)lua";

#if defined(LUA_PB_TEST_ENABLE_THREADS)
// 一个线程一个 lua_State、一个 Codec，共享同一个 Schema。
void ThreadWorker(const std::shared_ptr<const Schema>& schema, std::atomic<int>& failures, int rounds)
{
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    OpenPb(L, schema);
    for (int i = 0; i < rounds; ++i)
    {
        const std::string code = "local pb = require('pb')\n"
                                 "local b = pb.encode('lua_pb_test.Scalars', {i32 = " +
                                 std::to_string(i) +
                                 ", name = 'x'})\n"
                                 "local t = pb.decode('lua_pb_test.Scalars', b)\n"
                                 "assert(t.i32 == " +
                                 std::to_string(i) + " and t.name == 'x')\n";
        if (luaL_dostring(L, code.c_str()) != 0)
            failures.fetch_add(1, std::memory_order_relaxed);
    }
    lua_close(L);
}
#endif // LUA_PB_TEST_ENABLE_THREADS

} // namespace

int main()
{
    auto schema = LoadSchema();

    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    {
        OpenPb(L, schema);
        RunLua(L, kScript);
    } // Codec 由 __gc 在 lua_close 期间销毁，顺序天然正确
    lua_close(L);

#if defined(LUA_PB_TEST_ENABLE_THREADS)
    // 多线程冒烟：各线程自己的 lua_State 与 Codec，共享同一个 Schema（Schema 只读，天然安全；
    // 这里主要是给 tsan 预设跑）。CMake 里默认打开，可用宏关掉。
    std::atomic<int> failures{0};
    std::thread a(ThreadWorker, schema, std::ref(failures), 200);
    std::thread b(ThreadWorker, schema, std::ref(failures), 200);
    a.join();
    b.join();
    CHECK(failures.load() == 0);

    std::printf("lua_pb_binding_test: ok（含多线程冒烟）\n");
#else
    std::printf("lua_pb_binding_test: ok（未启用多线程冒烟，见 CMake 里的 LUA_PB_TEST_ENABLE_THREADS）\n");
#endif
    return 0;
}
