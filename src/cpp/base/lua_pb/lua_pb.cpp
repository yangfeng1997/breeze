// require("pb") 的实现。见 lua_pb.h 顶部的规矩。
//
// 错误路径的两段式（照 base/lua_binding 的思路）：拼错误文本的那一层**不调用 lua_error**，
// 让 std::string 正常析构；真正 lua_error 的那一帧只留平凡局部变量（longjmp 不会跳过析构）。

#include "base/lua_pb/lua_pb.h"

#include <cstddef>
#include <new>
#include <string>

namespace breeze::lua_pb
{
namespace
{

constexpr const char* kMetaName = "breeze.lua_pb";
constexpr const char* kModuleName = "pb";

// 放进 userdata：__gc 在 lua_close 期间跑，正好满足"Codec 先于 state 销毁"。
struct PbModule
{
    Codec codec;
};

PbModule* CheckModule(lua_State* L, int index)
{
    return static_cast<PbModule*>(luaL_checkudata(L, index, kMetaName));
}

// 这一层不 lua_error：std::string 在这里正常析构，栈上留下错误文本。
void PushErrorText(lua_State* L, const Schema& schema, const Error& e)
{
    const std::string msg = schema.Describe(e);
    lua_pushlstring(L, msg.data(), msg.size());
}

// "unknown type" 是用法错误（脚本写错了类型名），单独给一条带类型名的文本。
void PushUnknownType(lua_State* L, const char* what, const char* type)
{
    lua_pushfstring(L, "pb.%s: unknown type '%s'", what, type);
}


int PbDecode(lua_State* L)
{
    PbModule* m = CheckModule(L, lua_upvalueindex(1));
    const char* type = luaL_checkstring(L, 1);
    std::size_t len = 0;
    const char* data = luaL_checklstring(L, 2, &len);

    const TypeId id = m->codec.GetSchema().FindType(type);
    if (id == kNoIndex)
    {
        PushUnknownType(L, "decode", type);
        return lua_error(L);
    }
    auto r = m->codec.Decode(id, std::span<const std::byte>{reinterpret_cast<const std::byte*>(data), len});
    if (!r)
    {
        PushErrorText(L, m->codec.GetSchema(), r.error());
        return lua_error(L);
    }
    return 1; // 解码把一个 table 压栈了
}

int PbEncode(lua_State* L)
{
    PbModule* m = CheckModule(L, lua_upvalueindex(1));
    const char* type = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    const TypeId id = m->codec.GetSchema().FindType(type);
    if (id == kNoIndex)
    {
        PushUnknownType(L, "encode", type);
        return lua_error(L);
    }
    auto r = m->codec.Encode(id, 2);
    if (!r)
    {
        PushErrorText(L, m->codec.GetSchema(), r.error());
        return lua_error(L);
    }
    lua_pushlstring(L, reinterpret_cast<const char*>(r->data()), r->size());
    return 1;
}

// __gc 里不能抛：userdata 类型不对就直接返回。
int PbGc(lua_State* L)
{
    if (void* ud = luaL_testudata(L, 1, kMetaName); ud != nullptr)
        static_cast<PbModule*>(ud)->~PbModule();
    return 0;
}

void EnsureMetatable(lua_State* L)
{
    if (luaL_newmetatable(L, kMetaName) == 0)
        return; // 已经建过

    lua_pushcfunction(L, PbGc);
    lua_setfield(L, -2, "__gc");
    lua_pushliteral(L, "breeze.lua_pb");
    lua_setfield(L, -2, "__name");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable"); // 保护：脚本改不了元表
}

} // namespace

void OpenPb(lua_State* L, std::shared_ptr<const Schema> schema)
{
    const int top = lua_gettop(L);

    EnsureMetatable(L);
    lua_pop(L, 1); // 弹掉元表

    void* ud = lua_newuserdatauv(L, sizeof(PbModule), 0);
    new (ud) PbModule{Codec{L, std::move(schema)}};
    luaL_setmetatable(L, kMetaName);

    lua_createtable(L, 0, 2);
    lua_pushvalue(L, -2); // userdata 当 upvalue
    lua_pushcclosure(L, PbDecode, 1);
    lua_setfield(L, -2, "decode");
    lua_pushvalue(L, -2);
    lua_pushcclosure(L, PbEncode, 1);
    lua_setfield(L, -2, "encode");
    lua_remove(L, -2); // 去掉 userdata，只留模块表

    // package.loaded["pb"] = 模块表
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, kModuleName);

    lua_settop(L, top);
}

} // namespace breeze::lua_pb
