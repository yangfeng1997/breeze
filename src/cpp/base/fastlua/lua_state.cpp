#include "base/fastlua/lua_state.h"

#include <string>

namespace breeze::fastlua {

Result<void> LuaStateView::RunScript(std::string_view source, const char* chunkname) {
    const int base = lua_gettop(L_);
    int status = luaL_loadbufferx(L_, source.data(), source.size(), chunkname, "t");
    if (status != LUA_OK) {
        std::string message = TakeError(L_, base);
        return Error{ErrorCode::kSyntax, std::move(message)};
    }
    status = lua_pcall(L_, 0, LUA_MULTRET, 0);
    if (status != LUA_OK) {
        std::string message = TakeError(L_, base);
        return Error{status == LUA_ERRMEM ? ErrorCode::kMemory : ErrorCode::kRuntime, std::move(message)};
    }
    lua_settop(L_, base);
    return Ok();
}

Result<void> LuaStateView::RunFile(const char* path) {
    const int base = lua_gettop(L_);
    int status = luaL_loadfilex(L_, path, "t");
    if (status != LUA_OK) {
        std::string message = TakeError(L_, base);
        return Error{ErrorCode::kSyntax, std::move(message)};
    }
    status = lua_pcall(L_, 0, LUA_MULTRET, 0);
    if (status != LUA_OK) {
        std::string message = TakeError(L_, base);
        return Error{status == LUA_ERRMEM ? ErrorCode::kMemory : ErrorCode::kRuntime, std::move(message)};
    }
    lua_settop(L_, base);
    return Ok();
}

void LuaStateView::SetPath(std::string_view path) {
    lua_getglobal(L_, "package");
    lua_getfield(L_, -1, "path");
    const char* current_raw = lua_tostring(L_, -1);
    const std::string current = current_raw != nullptr ? current_raw : "";
    lua_pop(L_, 1);

    std::string value(path);
    if (const std::size_t pos = value.find(";;"); pos != std::string::npos) {
        value.replace(pos, 2, ";" + current + ";");
    }

    lua_pushlstring(L_, value.data(), value.size());
    lua_setfield(L_, -2, "path");
    lua_pop(L_, 1);
}

void LuaStateView::AddPath(std::string_view path) {
    lua_getglobal(L_, "package");
    lua_getfield(L_, -1, "path");
    const char* current_raw = lua_tostring(L_, -1);
    const std::string current = current_raw != nullptr ? current_raw : "";
    lua_pop(L_, 1);

    std::string value = current;
    if (!value.empty()) {
        value.push_back(';');
    }
    value.append(path.data(), path.size());

    lua_pushlstring(L_, value.data(), value.size());
    lua_setfield(L_, -2, "path");
    lua_pop(L_, 1);
}

void LuaStateView::EnsureCallableMeta() {
    lua_rawgetp(L_, LUA_REGISTRYINDEX, &kCallableMetaKey);
    if (lua_isnil(L_, -1)) {
        lua_pop(L_, 1);
        lua_newtable(L_);
        lua_pushcfunction(L_, &CallableGc);
        lua_setfield(L_, -2, "__gc");
        lua_pushvalue(L_, -1);
        lua_rawsetp(L_, LUA_REGISTRYINDEX, &kCallableMetaKey);
    }
    lua_pop(L_, 1);
}

}  // namespace breeze::fastlua
