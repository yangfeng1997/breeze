// codec 层：编码 / 解码 / 往返 / 黄金字节 / 表语义 / 混合遍历。
// 需要真的 lua_State（Lua 5.5.1）。无测试框架，失败即 abort。

#include "base/lua_pb/codec.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

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

std::vector<std::byte> ReadFile(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "打不开描述符：%s\n", path);
        std::abort();
    }
    std::vector<std::byte> data;
    char buf[4096];
    while (in.read(buf, sizeof(buf)) || in.gcount() > 0)
        for (std::streamsize i = 0; i < in.gcount(); ++i)
            data.push_back(static_cast<std::byte>(static_cast<unsigned char>(buf[i])));
    return data;
}

std::shared_ptr<const Schema> LoadSchema()
{
    auto s = Schema::Load(ReadFile(LUA_PB_TEST_DESCRIPTOR));
    if (!s)
    {
        std::fprintf(stderr, "Schema::Load 失败：%s\n", std::string(ErrorCodeName(s.error().code)).c_str());
        std::abort();
    }
    return std::make_shared<const Schema>(std::move(*s));
}

void RunLua(lua_State* L, const char* code)
{
    if (luaL_dostring(L, code) != 0)
    {
        std::fprintf(stderr, "Lua 出错：%s\n%s\n", lua_tostring(L, -1), code);
        std::abort();
    }
}

// 让 Lua 侧顺手把断言写掉：失败即 abort（错误信息带上 Lua 的行号）。
// 消息用长括号字符串，表达式里带单引号 / 双引号都不会破。
void ExpectLua(lua_State* L, const char* expr, int line)
{
    const std::string code = std::string("assert(") + expr + ", [[expect: " + expr + "]])";
    if (luaL_dostring(L, code.c_str()) != 0)
    {
        std::fprintf(stderr, "%s:%d: Lua 断言失败：%s\n", __FILE__, line, lua_tostring(L, -1));
        std::abort();
    }
}
#define EXPECT(expr) ExpectLua(L, expr, __LINE__)


// 解码成功的结果放到全局 t，方便后面的 EXPECT 直接读。
void DecodeT(lua_State* L, Codec& codec, std::string_view type, std::span<const std::byte> payload)
{
    lua_settop(L, 0);
    auto r = codec.Decode(type, payload);
    if (!r)
    {
        std::fprintf(stderr, "解码 %s 失败：%s\n", std::string(type).c_str(),
                     std::string(ErrorCodeName(r.error().code)).c_str());
        std::abort();
    }
    lua_pushvalue(L, -1);
    lua_setglobal(L, "t"); // 同时留一份在栈顶，方便紧接着 Encode(..., -1)
}

std::string Hex(std::span<const std::byte> b)
{
    std::string out;
    char tmp[8];
    for (std::byte x : b)
    {
        std::snprintf(tmp, sizeof(tmp), "%02X", std::to_integer<unsigned>(x));
        out += tmp;
    }
    return out;
}

// 把一段 Lua 代码求值成 table 并编码；失败返回错误。
std::expected<std::vector<std::byte>, Error> EncodeLua(Codec& codec, lua_State* L, std::string_view type,
                                                       const std::string& table_code)
{
    const std::string code = "return " + table_code;
    if (luaL_dostring(L, code.c_str()) != 0)
    {
        std::fprintf(stderr, "Lua 出错：%s\n", lua_tostring(L, -1));
        std::abort();
    }
    auto r = codec.Encode(type, -1);
    if (!r)
    {
        lua_pop(L, 1);
        return std::unexpected(r.error());
    }
    std::vector<std::byte> out(r->begin(), r->end());
    lua_pop(L, 1);
    return out;
}

void ExpectGolden(Codec& codec, lua_State* L, std::string_view type, const std::string& table_code,
                  std::span<const std::byte> expected)
{
    auto got = EncodeLua(codec, L, type, table_code);
    if (!got)
    {
        std::fprintf(stderr, "编码 %s 失败：%s\n", std::string(type).c_str(),
                     std::string(ErrorCodeName(got.error().code)).c_str());
        std::abort();
    }
    const std::string g = Hex(*got);
    const std::string e = Hex(expected);
    if (g != e)
    {
        std::fprintf(stderr, "字节不符 %s\n  期望 %s\n  实际 %s\n", std::string(type).c_str(), e.c_str(), g.c_str());
        std::abort();
    }
}

// 正确地把 inner 包成 field=<number> 的 LEN 字段（长度用 varint，别用单字节）。
std::vector<std::byte> WrapLen(int number, const std::vector<std::byte>& inner)
{
    std::byte buf[16];
    std::vector<std::byte> out;
    std::byte* p = WriteVarint(buf, MakeTag(static_cast<std::uint32_t>(number), WireType::kLen));
    out.assign(buf, p);
    p = WriteVarint(buf, inner.size());
    out.insert(out.end(), buf, p);
    out.insert(out.end(), inner.begin(), inner.end());
    return out;
}

std::vector<std::byte> Bytes(std::initializer_list<int> xs)
{
    std::vector<std::byte> out;
    for (int x : xs)
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(x)));
    return out;
}

void TestGoldenBytes()
{
    auto schema = LoadSchema();
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    {
        Codec codec(L, schema); // 必须早于 lua_close(L)：析构里要 unref kit 表

    const auto S = std::string_view{"lua_pb_test.Scalars"};
    ExpectGolden(codec, L, S, "{}", Bytes({}));                          // 全默认 → 不写
    ExpectGolden(codec, L, S, "{i32 = 0}", Bytes({}));                   // 隐式标量等于默认值 → 不写
    ExpectGolden(codec, L, S, "{i32 = 150}", Bytes({0x08, 0x96, 0x01}));
    ExpectGolden(codec, L, S, "{i32 = -1}", Bytes({0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01}));
    ExpectGolden(codec, L, S, "{ok = true}", Bytes({0x68, 0x01}));
    ExpectGolden(codec, L, S, "{name = 'ab'}", Bytes({0x72, 0x02, 'a', 'b'}));
    ExpectGolden(codec, L, S, "{blob = string.char(0, 1)}", Bytes({0x7A, 0x02, 0x00, 0x01}));
    ExpectGolden(codec, L, S, "{color = 2}", Bytes({0x80, 0x01, 0x02})); // field 16 → tag 128
    ExpectGolden(codec, L, S, "{fl = 1.0}", Bytes({0x5D, 0x00, 0x00, 0x80, 0x3F}));
    ExpectGolden(codec, L, S, "{db = 1.0}", Bytes({0x61, 0, 0, 0, 0, 0, 0, 0xF0, 0x3F}));
    ExpectGolden(codec, L, S, "{s32 = -1}", Bytes({0x28, 0x01}));        // sint32 用 zigzag
    ExpectGolden(codec, L, S, "{fx32 = 1}", Bytes({0x3D, 0x01, 0x00, 0x00, 0x00}));

    // 空 repeated / 空 map 不写
    ExpectGolden(codec, L, std::string_view{"lua_pb_test.Repeated"}, "{nums = {}}", Bytes({}));
    ExpectGolden(codec, L, std::string_view{"lua_pb_test.Maps"}, "{counts = {}}", Bytes({}));
    // packed repeated
    ExpectGolden(codec, L, std::string_view{"lua_pb_test.Repeated"}, "{nums = {1, 2, 3}}", Bytes({0x0A, 0x03, 1, 2, 3}));
    // 非打包的 repeated string
    ExpectGolden(codec, L, std::string_view{"lua_pb_test.Repeated"}, "{names = {'a'}}", Bytes({0x12, 0x01, 'a'}));
    // optional 显式存在：0 也要写
    ExpectGolden(codec, L, std::string_view{"lua_pb_test.Optionals"}, "{maybe = 0}", Bytes({0x08, 0x00}));
    ExpectGolden(codec, L, std::string_view{"lua_pb_test.Optionals"}, "{}", Bytes({}));
    // oneof
    ExpectGolden(codec, L, std::string_view{"lua_pb_test.Oneofs"}, "{a = 5}", Bytes({0x08, 0x05}));
    ExpectGolden(codec, L, std::string_view{"lua_pb_test.Oneofs"}, "{b = 'x'}", Bytes({0x12, 0x01, 'x'}));
    // message 字段显式存在：空表也要写（LEN 0）
    ExpectGolden(codec, L, std::string_view{"lua_pb_test.Nested"}, "{inner = {}}", Bytes({0x0A, 0x00}));
    // map：单条目
    ExpectGolden(codec, L, std::string_view{"lua_pb_test.Maps"}, "{counts = {a = 1}}",
                 Bytes({0x0A, 0x05, 0x0A, 0x01, 'a', 0x10, 0x01}));
    // map 多条 entry：每条必须各自一个 LEN 字段（历史上这里把多条包进了一个 LEN）
    {
        lua_settop(L, 0);
        RunLua(L, "t = {counts = {a = 1, b = 2}}");
        lua_getglobal(L, "t");
        auto two = codec.Encode("lua_pb_test.Maps", -1);
        CHECK(two.has_value());
        lua_settop(L, 0);
        DecodeT(L, codec, "lua_pb_test.Maps", *two);
        EXPECT("t.counts.a == 1 and t.counts.b == 2");
        // 每条 entry 各自带自己的 tag+长度：entry 体 5 字节，加 map 字段的 tag+长度 2 字节 → 7 字节/条
        CHECK(two->size() == 14);
        lua_settop(L, 0);
    }
    // uint64 高位：-1 就是 2^64-1
    ExpectGolden(codec, L, S, "{u64 = -1}",
                 Bytes({0x20, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01}));

    }
    lua_close(L);
}

void TestRoundTripAndIdempotence()
{
    auto schema = LoadSchema();
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    {
        Codec codec(L, schema); // 必须早于 lua_close(L)：析构里要 unref kit 表

    const char* table = R"({
        i32 = -7, i64 = 1 << 40, u32 = 4000000000, u64 = -2,
        s32 = -9, s64 = -1234567890123, fx32 = 7, fx64 = 8,
        sfx32 = -11, sfx64 = -12, fl = 1.5, db = -2.25,
        ok = true, name = "hello", blob = string.char(1, 2, 3),
        color = 2,
        field_with_a_really_long_name_exceeding_forty_bytes = "long-name-value",
    })";

    auto b1 = EncodeLua(codec, L, "lua_pb_test.Scalars", table);
    CHECK(b1.has_value());

    // 解码 → 再编码，字节必须一致（隐式默认值不会让输出变形）
    DecodeT(L, codec, "lua_pb_test.Scalars", *b1);
    EXPECT("t.i32 == -7");
    EXPECT("t.i64 == 1 << 40");
    EXPECT("t.u64 == -2");
    EXPECT("t.name == 'hello'");
    EXPECT("t.color == 2");
    EXPECT("t.field_with_a_really_long_name_exceeding_forty_bytes == 'long-name-value'");
    EXPECT("t.blob == string.char(1,2,3)");

    auto b2 = codec.Encode("lua_pb_test.Scalars", -1);
    CHECK(b2.has_value());
    CHECK(Hex(*b1) == Hex(*b2));

    // 复杂结构往返：Deep 里嵌 Inner 再嵌 Deep
    lua_settop(L, 0);
    RunLua(L, "t = { i = { v = 1 }, next = { i = { v = 2, tag = 'deep' } } }");
    lua_getglobal(L, "t"); // 赋值会把表弹掉，这里重新压回来给 Encode(-1) 用
    auto nb = codec.Encode("lua_pb_test.Deep", -1);
    CHECK(nb.has_value());
    lua_settop(L, 0);
    DecodeT(L, codec, "lua_pb_test.Deep", *nb);
    EXPECT("t.i.v == 1");
    EXPECT("t.next.i.v == 2");
    EXPECT("t.next.i.tag == 'deep'");

    // 两个 lua_State 之间：schema 共享，codec 各自独立
    // 注意：Codec 必须先于它绑定的 lua_State 析构（析构里要 unref kit 表），所以这里用一层作用域。
    lua_State* L2 = luaL_newstate();
    luaL_openlibs(L2);
    {
        Codec codec2(L2, schema);
        DecodeT(L2, codec2, "lua_pb_test.Scalars", *b1);
        ExpectLua(L2, "t.i32 == -7 and t.name == 'hello'", __LINE__);
    }
    lua_close(L2);

    }
    lua_close(L);
}

void TestDefaultsFilling()
{
    auto schema = LoadSchema();
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    {
        Codec codec(L, schema); // 必须早于 lua_close(L)：析构里要 unref kit 表

    // 空 payload：隐式存在的标量被逐字段填成默认值，普通数据（pairs 能看到）
    DecodeT(L, codec, "lua_pb_test.Scalars", {});
    EXPECT("t.i32 == 0 and t.ok == false and t.name == '' and t.color == 0");
    EXPECT("next(t) ~= nil"); // 确实进表了，不是靠元表
    // optional / message / repeated / map 缺失是 nil
    lua_settop(L, 0);
    DecodeT(L, codec, "lua_pb_test.Optionals", {});
    EXPECT("t.maybe == nil and t.maybe_str == nil");
    lua_settop(L, 0);
    DecodeT(L, codec, "lua_pb_test.Nested", {});
    EXPECT("t.inner == nil and t.inners == nil");
    lua_settop(L, 0);
    DecodeT(L, codec, "lua_pb_test.Maps", {});
    EXPECT("t.counts == nil and t.objs == nil");

    }
    lua_close(L);
}

void TestDecodeTolerance()
{
    auto schema = LoadSchema();
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    {
        Codec codec(L, schema); // 必须早于 lua_close(L)：析构里要 unref kit 表
    const auto S = std::string_view{"lua_pb_test.Scalars"};

    // 未知字段号（999，varint）+ 合法 i32 → 跳过未知，i32 正常
    {
        std::vector<std::byte> b = Bytes({0xB8, 0x3E, 0x07}); // tag(999<<3|0) = 7992 = 0xB8 0x3E
        b.insert(b.end(), {std::byte{0x08}, std::byte{0x2A}});
        DecodeT(L, codec, S, b);
        EXPECT("t.i32 == 42");
        lua_settop(L, 0);
    }
    // wire type 与 schema 不符（i32 用 LEN 发）→ 当未知字段跳过
    {
        std::vector<std::byte> b = Bytes({0x0A, 0x02, 0xAA, 0xBB});
        DecodeT(L, codec, S, b);
        EXPECT("t.i32 == 0");
        lua_settop(L, 0);
    }
    // packed 与非 packed 两种形式都接受
    {
        DecodeT(L, codec, "lua_pb_test.Repeated", Bytes({0x0A, 0x03, 1, 2, 3}));
        EXPECT("t.nums[1] == 1 and t.nums[2] == 2 and t.nums[3] == 3 and #t.nums == 3");
        lua_settop(L, 0);
        DecodeT(L, codec, "lua_pb_test.Repeated", Bytes({0x08, 1, 0x08, 2, 0x08, 3}));
        EXPECT("t.nums[3] == 3 and #t.nums == 3");
        lua_settop(L, 0);
    }
    // 非 packed repeated：检查数组没有多余空洞
    {
        DecodeT(L, codec, "lua_pb_test.Repeated", Bytes({0x1A, 0x02, 0x08, 0x05, 0x1A, 0x02, 0x08, 0x06}));
        EXPECT("#t.items == 2 and t.items[1].v == 5 and t.items[2].v == 6");
        lua_settop(L, 0);
    }
    // map：缺 value → 默认值；缺 key → 默认 ""；键序颠倒也认
    {
        DecodeT(L, codec, "lua_pb_test.Maps", Bytes({0x0A, 0x03, 0x0A, 0x01, 'a'}));
        EXPECT("t.counts.a == 0");
        lua_settop(L, 0);
        DecodeT(L, codec, "lua_pb_test.Maps", Bytes({0x0A, 0x02, 0x10, 0x07}));
        EXPECT("t.counts[''] == 7");
        lua_settop(L, 0);
        DecodeT(L, codec, "lua_pb_test.Maps", Bytes({0x0A, 0x05, 0x10, 0x09, 0x0A, 0x01, 'b'}));
        EXPECT("t.counts.b == 9");
        lua_settop(L, 0);
    }
    // oneof：两个成员都出现时，后出现的赢，表里只留一个
    {
        DecodeT(L, codec, "lua_pb_test.Oneofs", Bytes({0x08, 0x05, 0x12, 0x01, 'x'}));
        EXPECT("t.a == nil and t.b == 'x'");
        lua_settop(L, 0);
        DecodeT(L, codec, "lua_pb_test.Oneofs", Bytes({0x12, 0x01, 'x', 0x08, 0x05}));
        EXPECT("t.b == nil and t.a == 5");
        lua_settop(L, 0);
    }
    // 负数的两种线上形式：int32 用 64 位补码的 10 字节 varint；sint32 / sint64 用 zigzag
    {
        DecodeT(L, codec, S, Bytes({0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01}));
        EXPECT("t.i32 == -1");
        lua_settop(L, 0);
        DecodeT(L, codec, S, Bytes({0x28, 0x01})); // field 5 = s32，zigzag(-1) = 1
        EXPECT("t.s32 == -1");
        lua_settop(L, 0);
        DecodeT(L, codec, S, Bytes({0x30, 0x03})); // field 6 = s64，zigzag(-2) = 3
        EXPECT("t.s64 == -2");
        lua_settop(L, 0);
        // enum 的负数也是 10 字节形式：0x80 0x01 = tag(field 16, varint)
        DecodeT(L, codec, S, Bytes({0x80, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01}));
        EXPECT("t.color == -1");
        lua_settop(L, 0);
    }

    }
    lua_close(L);
}

void TestEncodeStrictness()
{
    auto schema = LoadSchema();
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    {
        Codec codec(L, schema); // 必须早于 lua_close(L)：析构里要 unref kit 表
    const auto S = std::string_view{"lua_pb_test.Scalars"};

    const auto expect_err = [&](std::string_view type, const std::string& table_code, ErrorCode want)
    {
        lua_settop(L, 0);
        auto r = EncodeLua(codec, L, type, table_code);
        if (r)
        {
            std::fprintf(stderr, "本该失败却成功：%s %s\n", std::string(type).c_str(), table_code.c_str());
            std::abort();
        }
        if (r.error().code != want)
        {
            std::fprintf(stderr, "错误码不符：%s（期望 %s，实际 %s）\n", table_code.c_str(),
                         std::string(ErrorCodeName(want)).c_str(),
                         std::string(ErrorCodeName(r.error().code)).c_str());
            std::abort();
        }
    };

    expect_err(S, "{nope = 1}", ErrorCode::kUnknownField);            // 未知字段名
    expect_err(S, "{[1] = 5}", ErrorCode::kUnknownField);             // 数字键不是字段名
    expect_err(S, "{i32 = 'x'}", ErrorCode::kTypeMismatch);           // 字符串给整数字段
    expect_err(S, "{i32 = 3.5}", ErrorCode::kTypeMismatch);           // 非整值浮点
    expect_err(S, "{ok = 1}", ErrorCode::kTypeMismatch);              // bool 只收 boolean
    expect_err(S, "{name = 5}", ErrorCode::kTypeMismatch);            // string 只收 string
    expect_err(S, "{i32 = 2147483648}", ErrorCode::kTypeMismatch);    // int32 越界
    expect_err(S, "{u32 = -1}", ErrorCode::kTypeMismatch);            // uint32 不接受负数
    expect_err(S, "{color = 'RED'}", ErrorCode::kTypeMismatch);       // enum 只收整数
    expect_err(std::string_view{"lua_pb_test.Oneofs"}, "{a = 1, b = 'x'}", ErrorCode::kOneofConflict);
    // repeated 的空洞：Lua 的 # 在有空洞时边界未定义（{1, 2, 3} 置 n[2] = nil 之后 # 可能只有 1），
    // 所以"空洞报错"只能在 # 恰好把空洞包进来时生效，属于尽力而为；这里只钉住稠密数组的正常路径。
    {
        lua_settop(L, 0);
        lua_createtable(L, 0, 1);
        lua_createtable(L, 3, 0);
        for (int i = 1; i <= 3; ++i)
        {
            lua_pushinteger(L, i);
            lua_rawseti(L, -2, i);
        }
        lua_setfield(L, -2, "nums");
        lua_getfield(L, -1, "nums");
        CHECK(lua_rawlen(L, -1) == 3);
        lua_pop(L, 1);
        auto r = codec.Encode("lua_pb_test.Repeated", -1);
        CHECK(r.has_value());
        CHECK(Hex(*r) == "0A03010203");
    }
    expect_err(std::string_view{"lua_pb_test.Nested"}, "{inner = 5}", ErrorCode::kTypeMismatch);
    expect_err(std::string_view{"lua_pb_test.Deep"}, "{[1] = 1}", ErrorCode::kUnknownField);
    // 整值浮点可以
    lua_settop(L, 0);
    CHECK(EncodeLua(codec, L, S, "{i32 = 3.0}").has_value());

    }
    lua_close(L);
}

void TestDepthLimit()
{
    auto schema = LoadSchema();
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    {
        Codec codec(L, schema); // 必须早于 lua_close(L)：析构里要 unref kit 表

    // 解码：Deep 套 150 层 → 超深度
    {
        std::vector<std::byte> inner; // 最内层空消息
        for (int i = 0; i < 150; ++i)
            inner = WrapLen(2, inner);
        auto r = codec.Decode("lua_pb_test.Deep", inner);
        CHECK(!r.has_value());
        CHECK(r.error().code == ErrorCode::kDepthExceeded);
    }
    // 编码：套 150 层 → 超深度
    {
        lua_settop(L, 0);
        RunLua(L, "t = {}; local cur = t; for i = 1, 150 do cur.next = {}; cur = cur.next end");
        lua_getglobal(L, "t");
        auto r = codec.Encode("lua_pb_test.Deep", -1);
        CHECK(!r.has_value());
        CHECK(r.error().code == ErrorCode::kDepthExceeded);
    }

    }
    lua_close(L);
}

} // namespace

int main()
{
    TestGoldenBytes();
    TestRoundTripAndIdempotence();
    TestDefaultsFilling();
    TestDecodeTolerance();
    TestEncodeStrictness();
    TestDepthLimit();
    std::printf("lua_pb_codec_test: ok\n");
    return 0;
}
