// lua_pb 的 codec 层：Lua table ↔ protobuf wire。设计见 docs/Lua_pb库设计.md §5、§6。
//
// 线程规则：一个 Codec 绑定一个 lua_State，只能在那个线程用；Schema 编译后不可变，可以跨线程共享。
// 热路径只用 raw API（rawget/rawset/rawgeti/rawseti）：编解码期间不触发元方法、不跑用户代码。
// 错误一律返回值：C++ API 返回 std::expected，不抛异常也不 longjmp；Lua 边界（P3）再转一次 luaL_error。
//
// 属性（"字段名 → 字段"的索引）按 per-state 懒建：≤40 字节的字段名在 Lua 里是驻留短字符串，
// 可以直接用 TString* 指针查表；更长的名字回退成内容比较。

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "base/lua_binding/config.h" // Lua 头（extern "C" 包裹）+ 版本断言
#include "base/lua_pb/error.h"
#include "base/lua_pb/schema.h"
#include "base/lua_pb/wire.h"

namespace breeze::lua_pb
{

class Codec
{
public:
    // schema 必须比 Codec 活得久（shared_ptr 保活）。
    Codec(lua_State* L, std::shared_ptr<const Schema> schema);
    ~Codec();

    Codec(const Codec&) = delete;
    Codec& operator=(const Codec&) = delete;
    Codec(Codec&&) noexcept;
    Codec& operator=(Codec&&) noexcept;

    // 成功时把一个 table 压栈；失败时栈不变。
    std::expected<void, Error> Decode(TypeId type, std::span<const std::byte> payload);
    std::expected<void, Error> Decode(std::string_view type_name, std::span<const std::byte> payload);

    // 读 index 处的 table，成功时返回本 Codec 输出缓冲的视图（下次 Encode / Decode 前有效）。
    std::expected<std::span<const std::byte>, Error> Encode(TypeId type, int index);
    std::expected<std::span<const std::byte>, Error> Encode(std::string_view type_name, int index);

    const Schema& GetSchema() const noexcept { return *schema_; }
    lua_State* State() const noexcept { return L_; }

private:
    // per-state、per-type 的字段名索引（懒建）
    struct Kit
    {
        int names_ref = 0; // registry 里的表：[1..n] = 字段名（同时负责让名字保持驻留）
        std::unordered_map<const void*, std::uint32_t> by_ptr; // 短名：TString* → 字段下标
        std::vector<std::pair<std::string_view, std::uint32_t>> long_names; // 长名：内容比较
    };

    const Kit& GetKit(TypeId type);

    std::expected<void, Error> DecodeMessage(TypeId type, Reader& r, unsigned depth);
    std::expected<void, Error> EncodeMessage(TypeId type, int index, unsigned depth);

    lua_State* L_ = nullptr;
    std::shared_ptr<const Schema> schema_;
    std::vector<Kit> kits_; // 按 TypeId 索引，懒建
    Writer writer_;
};

} // namespace breeze::lua_pb
