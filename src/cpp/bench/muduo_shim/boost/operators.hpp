// muduo 的 Timestamp 用 boost::equality_comparable / less_than_comparable 从 == 与 < 推出其余比较运算符。
// 基线 bench 不为这两个混入基类引入 Boost，这里提供语义相同的最小实现。

#pragma once

namespace boost
{

template <class T>
struct equality_comparable
{
    friend bool operator!=(const T& a, const T& b) { return !(a == b); }
};

template <class T>
struct less_than_comparable
{
    friend bool operator>(const T& a, const T& b) { return b < a; }
    friend bool operator<=(const T& a, const T& b) { return !(b < a); }
    friend bool operator>=(const T& a, const T& b) { return !(a < b); }
};

} // namespace boost
