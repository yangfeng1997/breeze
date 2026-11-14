// 类绑定测试：构造、方法、属性、继承、AllowFields、DotCall、所有权/GC、枚举。

#include "base/fastlua/fastlua.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

using namespace breeze::fastlua;

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

}  // namespace

int main() {
    LuaState lua;

    lua.Class<Entity>("Entity")
        .Ctor<>()
        .Method("GetId", &Entity::GetId)
        .Prop("id", &Entity::id);

    CHECK(lua.RunScript("e = Entity(); assert(e.id == 0); e.id = 3; assert(e.id == 3)").Ok());

    lua.Class<Player>("Player")
        .Ctor<int>()
        .Method("Hp", &Player::Hp)
        .Method("Add", &Player::Add)
        .Prop("hp", &Player::hp)
        .Prop("name", &Player::Name, &Player::SetName)
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
    lua.Class<Lifetime>("Lifetime").Ctor<>();
    CHECK(lua.RunScript("life = Lifetime()").Ok());
    CHECK(Lifetime::alive == 1);
    CHECK(lua.RunScript("life = nil; collectgarbage('collect')").Ok());
    CHECK(Lifetime::alive == 0);

    // 点号兼容模式：每次访问分配一个闭包，但调用语义正确。
    lua.Class<DotObj>("DotObj").Ctor<>().Method("Get", &DotObj::Get).DotCall();
    CHECK(lua.RunScript("d = DotObj(); assert(d.Get() == 5)").Ok());

    // 枚举表
    lua.Enum("Color", {{"Red", 1}, {"Green", 2}});
    CHECK(lua.RunScript("assert(Color.Red == 1 and Color.Green == 2)").Ok());

    std::printf("fastlua_class_test: ok\n");
    return 0;
}
