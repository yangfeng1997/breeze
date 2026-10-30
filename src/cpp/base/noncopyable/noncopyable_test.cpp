// NonCopyable：派生类不可拷贝也不可移动、可默认构造、不增加大小；基类不能单独实例化；可移动的类型不要继承它。
// 全是编译期断言（失败就编译不过），运行时什么也不做。

#include "noncopyable.h"

#include <cstdio>
#include <type_traits>
#include <utility>

using namespace breeze;

namespace
{

struct Plain : private NonCopyable
{
    int value = 0;
};
struct Empty : NonCopyable
{
};

// 派生类：既不可拷贝也不可移动，但可以默认构造 / 析构
static_assert(!std::is_copy_constructible_v<Plain> && !std::is_copy_assignable_v<Plain>);
static_assert(!std::is_move_constructible_v<Plain> && !std::is_move_assignable_v<Plain>);
static_assert(std::is_default_constructible_v<Plain> && std::is_destructible_v<Plain>);
static_assert(!std::is_copy_constructible_v<Empty> && !std::is_move_constructible_v<Empty>);

// 基类本身不能单独实例化、不能销毁（构造与析构是 protected 的）
static_assert(!std::is_default_constructible_v<NonCopyable>);
static_assert(!std::is_destructible_v<NonCopyable>);
static_assert(!std::is_copy_constructible_v<NonCopyable>);

// 空基类优化：继承它不增加大小
static_assert(sizeof(Plain) == sizeof(int));
static_assert(sizeof(Empty) == sizeof(NonCopyable));

// 作为成员的类型里，它也让外层类型不可拷贝
struct Holder
{
    Plain p;
};
static_assert(!std::is_copy_constructible_v<Holder>);

// 可移动的类型不要继承它：自己删拷贝、写移动即可（这里只验证"继承它之后自己写移动构造"这条路也走得通，但不推荐）
struct Handle
{
    int fd = -1;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept
        : fd(std::exchange(o.fd, -1))
    {
    }
    Handle& operator=(Handle&& o) noexcept
    {
        fd = std::exchange(o.fd, -1);
        return *this;
    }
};
static_assert(!std::is_copy_constructible_v<Handle> && std::is_nothrow_move_constructible_v<Handle>);

struct MovableOnTopOfNonCopyable : private NonCopyable
{
    int v = 0;
    MovableOnTopOfNonCopyable() = default;
    MovableOnTopOfNonCopyable(MovableOnTopOfNonCopyable&& o) noexcept
        : NonCopyable()
        , v(o.v)
    {
    } // 基类按默认构造，不搬它
};
static_assert(std::is_move_constructible_v<MovableOnTopOfNonCopyable> &&
              !std::is_copy_constructible_v<MovableOnTopOfNonCopyable>);

} // namespace

int main()
{
    // 运行时也走一遍，确认能正常使用
    Plain p;
    p.value = 3;
    Handle h;
    Handle h2 = std::move(h);
    MovableOnTopOfNonCopyable m;
    MovableOnTopOfNonCopyable m2 = std::move(m);
    (void)h2;
    (void)m2;
    std::puts("noncopyable_test: ok");
    return p.value == 3 ? 0 : 1;
}
