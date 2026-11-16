#pragma once

// breeze::lua_binding 总头。业务一般只包含这一个。
//
// 范围约束（详见 docs/Lua绑定库设计.md 的“职责边界”）：
//   本库只做 Lua 5.5 的类型转换、函数/类绑定、调用、错误、引用与可选沙箱/协程辅助。
//   以下不属于绑定库，禁止在这里实现，需求交给框架/运行时/诊断库：
//     - 跨线程 Reference/LuaTable/LuaFunction 的析构队列或延迟释放（异步传 ID）
//     - 协程超时定时器/看门狗（只提供 Cancel，由宿主定时器触发）
//     - sandbox 与 coroutine 的 hook 多路复用（Lua 只有一个 hook 槽）
//     - C++ 调用栈采集/符号化（用 base/backtrace 或业务诊断）
//     - 二进制 codec / 协议 / 缓冲区管理（独立包）

#include "base/lua_binding/check.h"
#include "base/lua_binding/config.h"
#include "base/lua_binding/error.h"
#include "base/lua_binding/stack.h"
#include "base/lua_binding/trampoline.h"
#include "base/lua_binding/call.h"
#include "base/lua_binding/reference.h"
#include "base/lua_binding/table.h"
#include "base/lua_binding/function.h"
#include "base/lua_binding/object.h"
#include "base/lua_binding/metamethod.h"
#include "base/lua_binding/lua_state.h"
#include "base/lua_binding/class.h"
