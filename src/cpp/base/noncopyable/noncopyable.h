// 不可拷贝的基类：继承它，派生类就既不能拷贝也不能移动（基类的拷贝被删除，派生类隐式的移动构造 / 赋值因此也不可用）。
//
//   class Connection : private breeze::NonCopyable { ... };
//
// 一般用 private 继承就行：它只是个标记，不需要对外暴露"is-a"。基类是空类，派生类的大小不会因此变大（空基类优化）。
// 构造与析构是 protected 且非虚：不能单独实例化，也不能通过基类指针删除派生对象。
//
// 要"可移动、不可拷贝"的类型（如持有文件描述符的句柄）不要继承它：自己声明 `T(const T&) = delete;` 与移动构造即可。
// 只想删拷贝的话，两行 `= delete` 与继承这个基类效果相同；本基类的价值是把意图写成一个名字，
// 并让旧代码的 `: NonCopyable` 写法原样可用。

#pragma once

namespace breeze
{

class NonCopyable
{
public:
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;

protected:
    NonCopyable() = default;
    ~NonCopyable() = default;
};

} // namespace breeze
