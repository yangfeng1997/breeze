// 类绑定测试：构造、方法、属性、继承、AllowFields、DotCall、所有权/GC、枚举。

#include "base/lua_binding/lua_binding.h"

#include <cstdio>
#include <cstdlib>
#include <ostream>
#include <string>
#include <utility>

using namespace breeze::lua_binding;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                                \
        }                                                                                \
    } while (0)

namespace {

struct Entity {
    int id = 0;
    int GetId() const { return id; }
};

struct Player : Entity {
    int hp = 0;
    std::string name;

    explicit Player(int value) : hp(value) {}
    int Hp() const { return hp; }
    void SetHp(int value) { hp = value; }
    std::string Name() const { return name; }
    void SetName(std::string value) { name = std::move(value); }
    int Add(int value) const { return hp + value; }
};

struct Lifetime {
    static inline int alive = 0;
    Lifetime() { ++alive; }
    ~Lifetime() { --alive; }
};

struct DotObj {
    int value = 5;
    int Get() const { return value; }
};

// 自动元方法：ToString / == / < / Close。
struct MetaBase {
    static inline int closed = 0;
    int value = 0;
    explicit MetaBase(int v) : value(v) {}
    std::string ToString() const { return "MetaBase(" + std::to_string(value) + ")"; }
    bool operator==(const MetaBase& other) const { return value == other.value; }
    bool operator<(const MetaBase& other) const { return value < other.value; }
    void Close() { ++closed; }
};

struct MetaDerived : MetaBase {
    explicit MetaDerived(int v) : MetaBase(v) {}
};

// 只有 operator<< 的类型，走另一条 __tostring 检测路径。
struct Streamed {
    int value = 0;
    explicit Streamed(int v) : value(v) {}
};

std::ostream& operator<<(std::ostream& os, const Streamed& value) {
    return os << "Streamed(" << value.value << ')';
}

// 自动检测没覆盖的元方法，走 .MetaMethod() 逃生门。
struct LenObj {
    int size = 0;
};

int LenObjLen(lua_State* L) {
    lua_pushinteger(L, 42);
    return 1;
}

// shared_ptr 所有权。
struct SharedObj {
    int value = 0;
    explicit SharedObj(int v) : value(v) {}
};

int SharedValue(std::shared_ptr<SharedObj> obj) {
    return obj != nullptr ? obj->value : -1;
}

}  // namespace

int main() {
    LuaState lua;

    lua.Class<Entity>("Entity")
        .Constructor<>()
        .Method("GetId", &Entity::GetId)
        .Property("id", &Entity::id);

    CHECK(lua.RunScript("e = Entity(); assert(e.id == 0); e.id = 3; assert(e.id == 3)").Ok());

    lua.Class<Player>("Player")
        .Constructor<int>()
        .Method("Hp", &Player::Hp)
        .Method("Add", &Player::Add)
        .Property("hp", &Player::hp)
        .Property("name", &Player::Name, &Player::SetName)
        .Base<Entity>()
        .AllowFields();

    const auto script = lua.RunScript(R"lua(
        p = Player(50)
        assert(p.id == 0)
        assert(p:Hp() == 50)
        p.hp = 70
        assert(p.hp == 70)
        p.name = 'bob'
        assert(p.name == 'bob')
        assert(p:Add(5) == 75)
        p.extra = 9
        assert(p.extra == 9)
        p.id = 3
        assert(p.id == 3)
        assert(p:GetId() == 3)
    )lua");
    if (!script.Ok()) {
        std::fprintf(stderr, "script failed: %s\n", script.GetError().message.c_str());
        return 1;
    }

    // C++ 拥有（borrowed）：Lua 不 delete，C++ 自己管理。
    auto* borrowed = new Player(10);
    lua.Set("borrowed", borrowed);
    CHECK(lua.RunScript("assert(borrowed:Hp() == 10)").Ok());
    delete borrowed;
    CHECK(lua.RunScript("borrowed = nil; collectgarbage('collect')").Ok());

    // Lua 拥有：__gc 必须析构。
    CHECK(Lifetime::alive == 0);
    lua.Class<Lifetime>("Lifetime").Constructor<>();
    CHECK(lua.RunScript("life = Lifetime()").Ok());
    CHECK(Lifetime::alive == 1);
    CHECK(lua.RunScript("life = nil; collectgarbage('collect')").Ok());
    CHECK(Lifetime::alive == 0);

    // 点号兼容模式：每次访问分配一个闭包，但调用语义正确。
    lua.Class<DotObj>("DotObj").Constructor<>().Method("Get", &DotObj::Get).DotCall();
    CHECK(lua.RunScript("d = DotObj(); assert(d.Get() == 5)").Ok());

    // 自动元方法：__tostring / __eq / __lt / __close；派生类继承基类的元方法。
    lua.Class<MetaBase>("MetaBase").Constructor<int>();
    lua.Class<MetaDerived>("MetaDerived").Constructor<int>().Base<MetaBase>();
    lua.Class<Streamed>("Streamed").Constructor<int>();
    const auto meta_script = lua.RunScript(R"lua(
        m1 = MetaBase(1)
        m2 = MetaBase(1)
        m3 = MetaBase(2)
        assert(tostring(m1) == 'MetaBase(1)')
        assert(m1 == m2)
        assert(m1 ~= m3)
        assert(m1 < m3)
        do local x <close> = MetaBase(9) end
        d1 = MetaDerived(1)
        d2 = MetaDerived(1)
        assert(tostring(d1) == 'MetaBase(1)')
        assert(d1 == d2)
        assert(tostring(Streamed(7)) == 'Streamed(7)')
    )lua");
    if (!meta_script.Ok()) {
        std::fprintf(stderr, "meta script failed: %s\n", meta_script.GetError().message.c_str());
        return 1;
    }
    CHECK(MetaBase::closed == 1);

    // 未自动检测的元方法走 MetaMethod 逃生门。
    lua.Class<LenObj>("LenObj").Constructor<>().MetaMethod("__len", &LenObjLen);
    CHECK(lua.RunScript("assert(#LenObj() == 42)").Ok());

    // shared_ptr：Lua 持有期间对象存活；C++ 侧仍可自己用 std::weak_ptr 观察。
    lua.Class<SharedObj>("SharedObj").Constructor<int>().Property("value", &SharedObj::value);
    lua.SetFunction("shared_value", &SharedValue);

    auto shared = std::make_shared<SharedObj>(7);
    std::weak_ptr<SharedObj> weak = shared;
    lua.Set("shared_obj", shared);
    CHECK(lua.RunScript("assert(shared_obj.value == 7)").Ok());

    // Lua -> C++：函数参数、Get。
    auto from_lua = lua.Call<int>("shared_value", shared);
    CHECK(from_lua.Ok());
    CHECK(from_lua.Value() == 7);
    {
        auto shared_back = lua.Get<std::shared_ptr<SharedObj>>("shared_obj");
        CHECK(shared_back.Ok());
        CHECK(shared_back.Value() != nullptr);
        CHECK(shared_back.Value()->value == 7);
    }

    // C++ 释放后，Lua 仍持有 shared_obj，对象继续存活。
    shared.reset();
    CHECK(!weak.expired());
    CHECK(lua.RunScript("shared_obj = nil; collectgarbage('collect')").Ok());
    CHECK(weak.expired());

    // 空 shared_ptr 往返为 nil。
    lua.Set("empty_shared", std::shared_ptr<SharedObj>{});
    auto empty_shared = lua.Get<std::shared_ptr<SharedObj>>("empty_shared");
    CHECK(empty_shared.Ok());
    CHECK(empty_shared.Value() == nullptr);

    // Class<T>().Constructor 创建的 owned 对象没有控制块，不能反解成 shared_ptr。
    CHECK(lua.RunScript("owned = SharedObj(3)").Ok());
    auto not_shared = lua.Get<std::shared_ptr<SharedObj>>("owned");
    CHECK(!not_shared.Ok());
    CHECK(not_shared.GetError().code == ErrorCode::kType);

    // 枚举表
    lua.Enum("Color", {{"Red", 1}, {"Green", 2}});
    CHECK(lua.RunScript("assert(Color.Red == 1 and Color.Green == 2)").Ok());

    std::printf("lua_class_test: ok\n");
    return 0;
}
