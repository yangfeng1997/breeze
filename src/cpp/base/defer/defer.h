// 作用域守卫：离开作用域时执行回调，可取消。
//
//   DEFER(::close(fd));                      // 单语句
//   DEFER({ std::fclose(fp); delete[] buf; }); // 多语句，花括号里的逗号也没问题
//
//   auto guard = MakeDefer([&] { Rollback(); }); // 需要取消时用具名守卫
//   if (committed) guard.Dismiss();
//
// 回调直接存在守卫对象里，没有 std::function 的类型擦除与堆分配。
// 回调不许抛：守卫在析构里调用它，抛出去就是 std::terminate（DEFER 宏的 lambda 还显式标了 noexcept）。
// 多个 DEFER 按声明的逆序执行。

#pragma once

#include <type_traits>
#include <utility>

namespace breeze
{

template <typename F>
class [[nodiscard]] Defer
{
public:
    explicit Defer(F f) noexcept(std::is_nothrow_move_constructible_v<F>)
        : f_(std::move(f))
    {
    }

    // 移动后原对象失效，回调只会执行一次。
    Defer(Defer&& other) noexcept(std::is_nothrow_move_constructible_v<F>)
        : f_(std::move(other.f_))
        , active_(std::exchange(other.active_, false))
    {
    }

    Defer(const Defer&) = delete;
    Defer& operator=(const Defer&) = delete;
    Defer& operator=(Defer&&) = delete;

    ~Defer()
    {
        if (active_)
            f_();
    }

    // 取消：析构时不再执行。
    void Dismiss() noexcept { active_ = false; }

    // 立刻执行并取消，用于"想提前收尾，又不想离开作用域"。已取消或已执行过则什么也不做。
    void Run()
    {
        if (std::exchange(active_, false))
            f_();
    }

    bool active() const noexcept { return active_; }

private:
    F f_;
    bool active_ = true;
};

template <typename F>
Defer(F) -> Defer<F>;

template <typename F>
[[nodiscard]] Defer<std::decay_t<F>> MakeDefer(F&& f)
{
    return Defer<std::decay_t<F>>(std::forward<F>(f));
}

} // namespace breeze

// 变量名带 __COUNTER__ 而不是 __LINE__：同一行（比如另一个宏展开出两个 DEFER）也不会重名。
#define BREEZE_DEFER_CAT_(a, b) a##b
#define BREEZE_DEFER_NAME_(n) BREEZE_DEFER_CAT_(breeze_defer_guard_, n)
#define DEFER(...)                                                                                                     \
    const ::breeze::Defer BREEZE_DEFER_NAME_(__COUNTER__)                                                              \
    {                                                                                                                  \
        [&]() noexcept { __VA_ARGS__; }                                                                                \
    }
