#include "base/lua_binding/ext/ext_impl.h"

#include <cctype>
#include <string_view>

namespace breeze::lua_binding {
namespace {

std::string_view ToView(lua_State* L, int index, size_t* length) {
    const char* data = luaL_checklstring(L, index, length);
    return std::string_view(data, *length);
}

int StringSplit(lua_State* L) {
    size_t length = 0;
    const std::string_view source = ToView(L, 1, &length);
    size_t delimiter_length = 0;
    const char* delimiter_data = luaL_checklstring(L, 2, &delimiter_length);
    if (delimiter_length == 0) {
        luaL_error(L, "breeze::lua_binding: string.split delimiter cannot be empty");
    }
    const std::string_view delimiter(delimiter_data, delimiter_length);

    lua_newtable(L);
    const int dst = lua_gettop(L);
    lua_Integer count = 0;
    size_t begin = 0;
    while (true) {
        const size_t pos = source.find(delimiter, begin);
        const size_t end = pos == std::string_view::npos ? source.size() : pos;
        lua_pushlstring(L, source.data() + begin, end - begin);
        lua_rawseti(L, dst, ++count);
        if (pos == std::string_view::npos) {
            break;
        }
        begin = pos + delimiter.size();
    }
    return 1;
}

int StringStartsWith(lua_State* L) {
    size_t source_length = 0;
    const std::string_view source = ToView(L, 1, &source_length);
    size_t prefix_length = 0;
    const std::string_view prefix = ToView(L, 2, &prefix_length);
    lua_pushboolean(L, source.size() >= prefix.size() && source.compare(0, prefix.size(), prefix) == 0);
    return 1;
}

int StringEndsWith(lua_State* L) {
    size_t source_length = 0;
    const std::string_view source = ToView(L, 1, &source_length);
    size_t suffix_length = 0;
    const std::string_view suffix = ToView(L, 2, &suffix_length);
    lua_pushboolean(L, source.size() >= suffix.size() &&
                           source.compare(source.size() - suffix.size(), suffix.size(), suffix) == 0);
    return 1;
}

int StringTrim(lua_State* L) {
    size_t length = 0;
    const std::string_view source = ToView(L, 1, &length);
    size_t begin = 0;
    size_t end = source.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(source[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(source[end - 1])) != 0) {
        --end;
    }
    lua_pushlstring(L, source.data() + begin, end - begin);
    return 1;
}

}  // namespace

void OpenStringExt(lua_State* L) {
    lua_getglobal(L, "string");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    const struct {
        const char* name;
        lua_CFunction fn;
    } functions[] = {
        {"split", StringSplit},
        {"starts_with", StringStartsWith},
        {"ends_with", StringEndsWith},
        {"trim", StringTrim},
    };
    for (const auto& entry : functions) {
        lua_pushcfunction(L, entry.fn);
        lua_setfield(L, -2, entry.name);
    }
    lua_pop(L, 1);
}

}  // namespace breeze::lua_binding
