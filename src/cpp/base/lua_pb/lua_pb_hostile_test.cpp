// codec 层：恶意 / 损坏输入必须返回错误码，不许崩、不许越界。无测试框架，失败即 abort。

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

} // namespace

int main()
{
    auto schema = Schema::Load(ReadFile(LUA_PB_TEST_DESCRIPTOR));
    CHECK(schema.has_value());
    auto shared = std::make_shared<const Schema>(std::move(*schema));

    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    {
        Codec codec(L, shared); // 必须早于 lua_close(L)：析构里要 unref kit 表

    const auto S = std::string_view{"lua_pb_test.Scalars"};
    const auto R = std::string_view{"lua_pb_test.Repeated"};
    const auto M = std::string_view{"lua_pb_test.Maps"};
    const auto D = std::string_view{"lua_pb_test.Deep"};

    const auto expect_err = [&](std::string_view type, const std::vector<std::byte>& b, ErrorCode want)
    {
        lua_settop(L, 0);
        auto r = codec.Decode(type, b);
        if (r)
        {
            std::fprintf(stderr, "本该失败却成功：%s（%zu 字节）\n", std::string(type).c_str(), b.size());
            std::abort();
        }
        if (r.error().code != want)
        {
            std::fprintf(stderr, "错误码不符：期望 %s，实际 %s\n", std::string(ErrorCodeName(want)).c_str(),
                         std::string(ErrorCodeName(r.error().code)).c_str());
            std::abort();
        }
        CHECK(lua_gettop(L) == 0); // 失败时栈必须还原
    };

    // 截断：标签后面什么都没有
    expect_err(S, Bytes({0x08}), ErrorCode::kTruncated);
    // 截断：varint 只给了续行字节
    expect_err(S, Bytes({0x08, 0x80}), ErrorCode::kTruncated);
    // 超长 varint：11 个续行字节
    expect_err(S, Bytes({0x08, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}),
               ErrorCode::kTruncated);
    // 第 10 字节带高位（值超过 64 位）
    expect_err(S, Bytes({0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x03}),
               ErrorCode::kTruncated);
    // LEN 长度超过剩余字节
    expect_err(S, Bytes({0x72, 0x05, 'a', 'b'}), ErrorCode::kLengthOverflow);
    expect_err(S, Bytes({0x72, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 'a'}), ErrorCode::kLengthOverflow);
    // field number 0
    expect_err(S, Bytes({0x00, 0x01}), ErrorCode::kFieldZero);
    expect_err(S, Bytes({0x07, 0x01}), ErrorCode::kFieldZero); // tag 7 = field 0 + wire type 7
    // 非法 wire type（6 / 7）
    expect_err(S, Bytes({0x0E, 0x01}), ErrorCode::kTruncated); // field 1 wire type 6
    // group（本库不做）：开始 group 后无法跳过
    expect_err(S, Bytes({0x0B, 0x01}), ErrorCode::kTruncated); // field 1 wire type 3
    // 未知字段的 LEN 也越界 → 跳过失败（跳过路径只能报 truncated）
    expect_err(S, Bytes({0xBA, 0x3E, 0x7F}), ErrorCode::kTruncated);
    // 嵌套子消息长度撒谎：Nested.inner 说 5 字节，实际只有 1 字节
    expect_err(std::string_view{"lua_pb_test.Nested"}, Bytes({0x0A, 0x05, 0x08}), ErrorCode::kLengthOverflow);
    // 子消息内部截断
    expect_err(std::string_view{"lua_pb_test.Nested"}, Bytes({0x0A, 0x01, 0x08}), ErrorCode::kTruncated);
    // packed 元素截断：说 3 字节，里面只有 1 个完整 varint
    expect_err(R, Bytes({0x0A, 0x03, 0x80}), ErrorCode::kLengthOverflow);
    // 定长字段（fixed32）字节不够
    expect_err(S, Bytes({0x3D, 0x01, 0x02}), ErrorCode::kTruncated);
    // map 条目长度撒谎
    expect_err(M, Bytes({0x0A, 0x09, 0x0A, 0x01, 'a'}), ErrorCode::kLengthOverflow);
    // map 的 value 是消息，但内部截断
    expect_err(M, Bytes({0x12, 0x04, 0x10, 0x01, 0x12, 0x01}), ErrorCode::kLengthOverflow);
    // 深度炸弹：Deep 套 200 层
    {
        std::vector<std::byte> inner;
        for (int i = 0; i < 200; ++i)
            inner = WrapLen(2, inner);
        expect_err(D, inner, ErrorCode::kDepthExceeded);
    }
    // 未知类型名
    {
        lua_settop(L, 0);
        auto r = codec.Decode("lua_pb_test.NoSuchType", {});
        CHECK(!r.has_value());
        CHECK(r.error().code == ErrorCode::kUnknownType);
        CHECK(lua_gettop(L) == 0);
    }
    // 空 payload 是合法的（全部字段缺失 → 默认值）
    {
        lua_settop(L, 0);
        CHECK(codec.Decode(S, {}).has_value());
        lua_settop(L, 0);
    }

    }
    lua_close(L);
    std::printf("lua_pb_hostile_test: ok\n");
    return 0;
}
