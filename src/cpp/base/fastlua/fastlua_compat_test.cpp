// luakit 风格兼容层测试：new_class / new_enum / new_table / call / table_call。

#include "base/fastlua/compat/luakit.h"
#include "base/fastlua/fastlua.h"

#include <cstdio>
#include <cstdlib>

using namespace breeze::fastlua;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                                \
        }                                                                                \
    } while (0)

namespace {
struct Thing {
    int value = 7;
    int Get() const { return value; }
    int Add(int delta) const { return value + delta; }
};
}  // namespace

int main() {
    LuaState lua;
    lua.Class<Thing>("Thing").Ctor<>();
    compat::new_class<Thing>(lua, "Thing", "Get", &Thing::Get, "Add", &Thing::Add, "value", &Thing::value);
    compat::new_enum(lua, "Color", "Red", 1, "Green", 2);

    CHECK(lua.RunScript("t = Thing(); assert(t:Get() == 7); t.value = 9; assert(t:Add(1) == 10)").Ok());
    CHECK(lua.RunScript("assert(Color.Red == 1 and Color.Green == 2)").Ok());

    CHECK(lua.RunScript("function sum(a, b) return a + b end").Ok());
    auto sum = compat::call<int>(lua, "sum", 20, 22);
    CHECK(sum.Ok());
    CHECK(sum.Value() == 42);

    CHECK(lua.RunScript("bag = { add = function(a, b) return a + b end }").Ok());
    auto bag = compat::table_call<int>(lua, "bag", "add", 1, 2);
    CHECK(bag.Ok());
    CHECK(bag.Value() == 3);

    Table table = compat::new_table(lua, "fresh");
    table.Set("x", 1);
    CHECK(lua.RunScript("assert(fresh.x == 1)").Ok());

    std::printf("fastlua_compat_test: ok\n");
    return 0;
}
