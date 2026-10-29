// muduo 的 EventLoop.h / TcpConnection.h 只为 setContext 用到 boost::any。
// 基线 bench 不为这一个类型引入 Boost，映射到 std::any（语义相同，muduo 的库代码里没有 any_cast 以外的用法）。

#pragma once

#include <any>

namespace boost
{
using any = std::any;
using std::any_cast;
} // namespace boost
