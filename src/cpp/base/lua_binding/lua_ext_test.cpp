// 可选扩展测试：table / string / os 工具（含循环引用 deepcopy）。

#include "base/lua_binding/ext/ext.h"
#include "base/lua_binding/lua_binding.h"

#include <cstdio>
#include <cstdlib>

using namespace breeze::lua_binding;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                                \
        }                                                                                \
    } while (0)

int main() {
    LuaState lua;
    OpenExt(lua, Ext::kAll);

    const auto script = lua.RunScript(R"lua(
        assert(table.size({1, 2, 3}) == 3)

        local source = {a = 1, b = 2}
        local shallow = table.copy(source)
        assert(shallow.a == 1 and shallow.b == 2 and shallow ~= source)

        local cyclic = {x = {y = 1}}
        cyclic.self = cyclic
        local deep = table.deepcopy(cyclic)
        assert(deep.x.y == 1 and deep.self == deep and deep ~= cyclic)

        local keys = table.keys({a = 1, b = 2})
        assert(#keys == 2)
        local vals = table.vals({a = 1, b = 2})
        assert(#vals == 2)
        assert(table.merge({a = 1}, {b = 2}).b == 2)

        local parts = string.split('a,b,c', ',')
        assert(#parts == 3 and parts[2] == 'b')
        assert(string.starts_with('hello', 'he'))
        assert(string.ends_with('hello', 'lo'))
        assert(string.trim('  x  ') == 'x')

        assert(os.setenv('BREEZE_LUA_EXT_TEST', '1'))
        assert(os.getenv('BREEZE_LUA_EXT_TEST') == '1')
    )lua");
    if (!script.has_value()) {
        std::fprintf(stderr, "script failed: %s\n", script.error().message.c_str());
        return 1;
    }

    std::printf("lua_ext_test: ok\n");
    return 0;
}
