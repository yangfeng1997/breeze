# Lua 绑定库：`base/lua_binding`

`breeze::lua_binding`：Lua 5.5.1 的 C++ 绑定库。小型编译库（`breeze_lua_binding`）加头文件模板；链式 `Class<T>` 构建器为主，luakit 风格键值列表是同一个构建器上的糖衣；默认冒号调用零分配；错误不依赖 C++ 异常（两段式 trampoline + `Result<T>`）；可选 `breeze_lua_binding_ext`（table / string / os 工具）、`sandbox.h`（内存 / 指令 / 时间预算）、`coroutine.h`。

依赖 vcpkg 的 `lua` 端口（5.5.1，按 C 编译）。**C++26 为基线**（仓库根 `CMakeLists.txt` 设 `CMAKE_CXX_STANDARD 26`，`breeze_lua_binding` 也以 `target_compile_features(... cxx_std_26)` 声明）；不需要网络库，Linux 与 Windows（MSYS2 g++ 16）都能构建。

C++26 下可用的 `std::expected`、`std::move_only_function`、`std::to_underlying`、`std::unreachable` 等（多数从 C++23 起就有），breeze::lua_binding **没有为了用而用**：`Result<T>` 仍是主错误类型，内部也不依赖 `std::move_only_function`。升级标准只是为了不再为低版本兼容性做取舍。

本文记录**为什么是这个样子**。API 的细节与约束写在头文件注释里，本文不重复。

> 本文只讲**绑定层**。主线程 `LuaModule`、worker 线程、目录约定、任务协议和文件级热更属于框架层，见 [Lua脚本框架设计.md](Lua脚本框架设计.md)。

**范围约束**：不属于 Lua 绑定库的职责——跨线程 `Reference` 析构队列、协程超时定时器、sandbox/coroutine 的 hook 复用器、C++ 栈采集、二进制 codec（`luabuf` / `slice` / `encode` / `decode` / `serialize`）——**一律不做**，需求由框架 / 运行时 / 诊断库或独立包承担；详见 §3“职责边界”。

**状态**：debug、release、`-fno-exceptions`、C++26 构建、以及带 `LUA_USE_APICHECK` 的 Lua 下测试通过；**asan + ubsan 与 tsan 预设下 8 个 breeze::lua_binding 测试全部通过**（修掉了一处 `StateData` 桶数组泄漏）。release 基准在 Windows（MinGW g++ 16）与 Linux（clang 23）上跑过。Lua 的 apicheck 只在 debug/asan/tsan 预设里通过 vcpkg overlay 打开。

---

## 1. 来源与取舍

参考了三个现成实现：

- **quanta 的 `extend/luakit`**：游戏行业习惯的来源（`new_class` 键值列表、`new_enum`、按名字调用、`object_call` 钩子、工厂 + `__gc` 所有权）。它的实现问题见 §2。
- **sol2（solpp 分支）**：C++→Lua 缓存调用的性能最好（接近裸 API），因为它把错误处理函数存成引用、`sol::function` 默认走不受保护的 `lua_call`；但上游不支持 Lua 5.5，必须用分支。
- **LuaBridge3**：Lua 5.5 兼容最明确、单头、轻量；但每个参数都走 `Stack<T>::get` + `TypeResult`，读表字段 269ns，且 `raise_lua_error` 在 catch 块里执行（C 编译的 Lua 下会泄漏异常对象）。

取舍：

- **不要每个调用都查 `debug.traceback`**：luakit 的 C++→Lua 慢 4 倍就是每次 `getglobal` + `getfield`。breeze::lua_binding 的 `LuaFunction::Call` 用无 upvalue 的 light C 函数作 msgh（`lua_pushcfunction` 不分配），热路径另提供 `CallNoTraceback` 不挂 traceback。
- **参数先全部检查、再读取**：检查阶段只用平凡存储，避免在 `lua_error` longjmp 前留下带析构的临时对象；错误消息用 `luaL_typeerror` / `luaL_argerror`（带参数序号与期望/实际类型）。
- **不用 `std::function` 做每次调用**：自由函数 / lambda 注册时放在 userdata upvalue 里，调用走模板蹦床；成员函数同理（upvalue 1 成员指针，upvalue 2 声明类）。
- **类身份不用字符串哈希**：`kTypeTag<T>` 的地址作 key；基类转换用注册时生成的 `static_cast`，虚继承也正确。
- **错误不抛异常**：核心在 `-fno-exceptions` 下编译；C++ 调 Lua 一律 `lua_pcall` 并返回 `Result`；绑定函数在 `Invoke`（noinline）里返回，之后才在只有平凡局部变量的那一层 `lua_error`。
- **不用 `std::expected` 替换 `Result<T>`**：`Result<T>` 是公共 API，成功时不分配堆内存；C++26 下可以提供互转，但不改主接口。

## 2. 原实现里发现并处理的问题

luakit（`extend/luakit/include`）：

- 每次调用重新查 `debug.traceback`（`lua_function.h:259-261`）：C++→Lua 44~58ns vs 裸 API 10~25ns。
- 访问方法每次都分配一个闭包（`lua_class.h:49-54`），`__index` 还做多次哈希查找；breeze::lua_binding 默认 `__index = 方法表`，只有属性类才用 C 闭包。
- 对象是“代理表 + lightuserdata”缓存在按地址索引的弱表里：地址复用会拿到陈旧代理，C++ `delete` 后 Lua 侧不失效。breeze::lua_binding 直接用 userdata + cookie，裸指针不再按地址缓存。
- 没有类型 / 参数检查（`lua_to_object` 把任意带 `__pointer__` 的表或 userdata 强转），传错类型是未定义行为；nil 悄悄变成 0 / "" / nullptr。
- `luaL_error(L, e.what())` 是格式串漏洞，而且写在 catch 块里（`lua_codec.h:309/399`）；`printf(msg.c_str())`（`lua_logger.h:19`）同理。
- map / set 转换里对 key 调 `lua_tolstring` 会原地把数字转成字符串，破坏 `lua_next`。
- `thread_local` 全局（`lbuf`、`glogger`）按 DLL / 线程各一份；logger 绑定到第一个 `L`，`lua_close` 后悬空。
- `reference` 从 `lua_State*` 非 explicit 构造并弹栈、无拷贝赋值、`lua_close` 后析构是未定义行为。
- 头文件不自包含、`_mm_pause` 没有 include、`WIN32` 写成 `WIN32`、`typeid(T).name()` 作元表 key、未对齐的 `*(T*)` 读写等可移植性问题。

sol2 / LuaBridge3：前者上游不支持 5.5（`compat-5.3.h` 的 `#error`、`LUA_ERRGCMM`、`lua_newstate` 参数）；后者在 catch 块里 `lua_error`，且逐字段读表很慢。breeze::lua_binding 把两者的优点（sol2 的 handler 缓存与 trampoline、LuaBridge3 的类型身份与 `ArgStorage` 思路）拿过来，避开这些问题。

## 3. 设计

- **`LuaState` / `LuaStateView`**：`LuaState` 拥有 `lua_State`（`lua_newstate` 或 `luaL_newstate`），`LuaStateView` 是非拥有视图，用于 `luaopen_*`。一个 state 绑定一个线程（一个 Shard）。
- **`Stack<T>`**：一个 customization point。内置整数（范围/可表示性检查）、浮点、bool、enum、`std::string`、`std::string_view`、`const char*`、`optional`、`vector`、`array`、`map` / `unordered_map`、`span`（push-only）、`tuple`/`pair`（仅返回值多值）、对象指针/引用、`shared_ptr`、`LuaTableView` 栈视图。
- **两段式 trampoline**（`trampoline.h`）：`Invoke` 里检查参数、构造临时对象、调用用户函数、压返回值；它 normal return 之后，`Trampoline`（只有 `ErrorSlot` 等平凡局部变量）再调用 `RaiseError` → `luaL_typeerror` / `luaL_error`。这样就算 Lua 按 C 编译、`lua_error` 是 longjmp，也不会跳过任何析构。
- **`Result<T>`**：`Ok()` / `Value()` / `ValueOr()` / `GetError()` / `NotFound()`；错误码含 kNotFound / kRuntime / kSyntax / kMemory / kType / kStack / kCancelled 等。缺函数是 `kNotFound`，不是异常。
  - `Value()` 是“已经 `Ok()` 之后”的访问器；在错误上调用属于编程错误，经 `BREEZE_LUA_CHECK`（可接 fatal handler）记录后 abort。需要恢复就用 `Ok()` / `ValueOr()` / `GetError()`。
  - C++→Lua 的**返回值**也走 `Stack<T>::Check`：类型不匹配返回 `kType`，不会静默变成 0 / false / 空串。
  - “不抛异常”不等于所有 Lua 错误都可恢复：`lua_pushlstring`、`luaL_ref`、表插入等可能分配的操作在 OOM 时会 `longjmp` 到 `lua_atpanic`（breeze::lua_binding 的 handler 记录后 abort），这类内存错误按致命处理，不承诺跨 C++ 栈恢复。
- **调用**：`CallGlobal`（按名字，晚绑定，热更友好）、`CallPushed`（函数已在栈上）、`LuaFunction::Call`（缓存引用，带 traceback）与 `LuaFunction::CallNoTraceback`（不挂 traceback）。`LuaFunction::Call` 走 `CallRef`：先压 msgh 再压函数，省掉 `lua_insert`；每次调用一次 `lua_checkstack`，`lua_pcall` 后 `lua_settop` 恢复。`CallNoTraceback` 的 void 返回不需要 `settop`。
  - **逃生门**：`LuaFunction::CallUnprotected` / `LuaStateView::CallUnprotected` 返回裸 `R`（不是 `Result`），走 `lua_call`，不捕获 Lua 错误，用于确认不会报错的热路径。**报错会 longjmp 跳过当前 C++ 栈的析构**；返回值类型不匹配直接 `Fatal`。默认 API 仍然是 pcall。
- **类绑定（`class.h` / `class.cpp`；对象 userdata 访问在 `object.h`，自动元方法在 `metamethod.h`）**：
  - userdata 布局 `[ObjHeader]`（共享所有权时在 header 后**内联** `shared_ptr<void>`，不再单独 `new`）；`ObjHeader` 带 cookie、ClassInfo、对象指针、所有权标志、持有块与释放函数。
  - `ObjHeader` 当前 48B：cookie 8 + cls 8 + obj 8 + flags/pad 8 + holder 8 + destroy_holder 8。内存敏感时可以去掉 `destroy_holder`，用 `kObjShared` 标记调用固定的释放函数，降到 40B（百万实体省 8MB）。这是 header 自身大小，不含 Lua userdata 的 GC 头、metatable 指针和分配器对齐，实际每对象还要再加约 30~50B。
  - 和 sol2 的 userdata 内存对比（Lua 5.5，按 `lua_Alloc` 请求字节数，不含对象本身和 malloc 对齐）：

    | 方式 | sol2 | breeze::lua_binding |
    |---|---:|---:|
    | borrowed / 裸指针 | 64（Udata + 1 uservalue）+ 8（T*）= **72B** | 32（Udata 无 uservalue）+ 48（ObjHeader）= **80B** |
    | shared_ptr | 64 + 8（T*）+ 8（dtor）+ 8（tag）+ 16（shared_ptr）= **104B** | 32 + 48 + 16（内联 shared_ptr）= **96B** |
    | 按值（T） | 64 + sizeof(T)（对象内联） | 32 + 48（对象在 C++ 堆上 `new`，不计入 Lua） |

    sol2 的裸指针 usertype 最省（8B payload），但它固定申请 1 个 uservalue（+32B）；breeze::lua_binding 默认 0 个 uservalue，但 ObjHeader 是 48B。两者在 shared_ptr 上接近，breeze::lua_binding 反而少 8B。`AllowFields` 的类会多 1 个 uservalue（+32B）。
  - 类型身份 `kTypeTag<T>` 地址；`GetObjectAs<T>` 先比类型，再沿 `ancestors` 应用注册时生成的 `up` 转换。
  - 每类一个 metatable；纯方法类的 `__index` 直接是方法表，属性类用 C `__index` 查 getters 表（lightuserdata `PropertyDesc*`，直接调用 getter/setter，无嵌套 Lua 调用）。
  - 所有权：`Policy::kOwned`（默认，`__gc` delete）、`Policy::kBorrowed`（C++ 管，`__gc` 不做事）；`ObjHeader::flags` 决定是否 delete。
  - `shared_ptr`：按**实例**而不是按类。`PushSharedObject` 把对象放进 userdata 内联的 `shared_ptr<void>`，`ObjHeader` 记录 `holder` + `destroy_holder`；`__gc` 只释放持有块，不 delete 对象。`Stack<std::shared_ptr<T>>` 用 aliasing 构造函数从 `shared_ptr<void>` + `GetObjectAs<T>` 得到正确的 `shared_ptr<T>`（支持基类/多继承指针调整）。只有按 `shared_ptr` 推入的对象能反解成 shared_ptr，`Class<T>().Constructor` 创建的 owned 对象不行（没有控制块）。**不绑定 `weak_ptr`**：Lua 侧无法安全地做“可能已失效”的句柄，C++ 侧需要弱观察时请自行用 `std::weak_ptr`。
  - `AllowFields` 把未知字段存到 uservalue 表（`lua_newuserdatauv` 的 uservalue 数在创建时按类决定）；`DotCall` 只对显式开启的类在方法访问时包装一个闭包，供 quanta 点号脚本迁移。
  - 构造函数：`Constructor<Args...>` 设置类表的 `__call`；参数从索引 2 开始，`new T(args...)`，失败转 `ErrorSlot`。
  - **自动元方法**（注册类时按 C++ 能力检测）：
    - `__tostring`：`T::ToString()`（可转 `std::string`）优先，否则 `operator<<(std::ostream&, const T&)`；
    - `__eq`：`operator==`；`__lt`：`operator<`；
    - `__close`：`T::Close()` 或 `T::close()`，只对 Lua 的 `local x <close> = obj` 生效；`Close()` 不应 `delete this`（所有权仍归 `__gc`）。
    - `Base<B>()` 会把基类已有的这四个元方法复制到派生类（派生类自己有的不覆盖），所以基类的比较/打印/关闭语义能被派生对象复用。
    - 未自动检测 `__le`、算术运算符、`__len`/`__pairs` 等；需要时用 `.MetaMethod("__le", &MyLessEqual)` 显式注册裸 `lua_CFunction`（逃生门，同上）。
- **引用（`reference.h`；`LuaTable` / `LuaFunction` 分别在 `table.h` / `function.h`）**：`Reference` / `LuaTable` / `LuaFunction` 是 registry 引用，move-only，需要第二份独立引用时用显式 `Clone()`；`Control` 引用计数块和 `StateData` 在 `state_data.h`，`StateData` 的 `__gc` 在 `lua_close` 时把 `state` 置空，所以 `lua_close` 之后析构 `Reference` 是空操作。**线程规则见下，库不实现跨线程引用释放队列。**
- **文件结构**：`lua_binding.h` 是总头；`state_data.h`（Control/StateData）、`reference.h`、`table.h`、`function.h`、`object.h`（userdata 与 `Stack<T*>`/`T&`/`shared_ptr`）、`metamethod.h`、`class.h`（ClassBuilder/trampoline）、`stack.h`、`call.h`、`trampoline.h`、`error.h`、`check.h`、`config.h`、`lua_state.h`、`sandbox.h`、`coroutine.h`。对象/视图的 `Stack` 特化不在 `stack.h`，分别见 `object.h`、`table.h`、`function.h`。
- **线程规则（必须遵守）**：
  - 一个 `LuaState` 只属于一个线程；`Reference` / `LuaTable` / `LuaFunction` 必须在该线程创建、使用、析构。
  - 异步 worker / IO / DB 线程只传 ID、令牌或序列化数据，不传 `Reference`；需要 Lua 回调时把 ID 投回属主线程，再查表取 `Reference`。
  - 任意线程可析构的 C++ 句柄（如 breeze 的 `ReplyHandle`）不得直接持有 `Reference`，包装层只存 ID。
  - 违反是使用错误：Debug/Release 都只记录一次并泄漏该 registry 槽，不碰 VM，避免 UB。**这是刻意不做跨线程释放队列**：sol2、LuaBridge3、luakit 等主流绑定库同样不做，线程归属由框架/使用者保证。
  - 其它绑定库也不能跨 OS 线程用同一个 `lua_State`：sol2 文档直接写 “Lua has no thread safety”，建议每个线程一个 state、跨 state 传值靠 C++ 侧序列化；`sol::main_*` / `lua_xmove` 只解决 **Lua 协程线程**（同一 OS 线程）的引用归属，不是 OS 线程安全。LuaBridge3 的 `LuaRef` 同样绑定到构造时的 state/thread，不提供同步；luakit 的 `reference` 析构直接 `luaL_unref`，换线程就是 UB。它们“能拷贝”不等于“能跨线程”——拷贝本身也要访问 VM。
- **可选模块**：
  - `ext/`：table.copy / deepcopy（visited 表处理环，深度上限 64）、size / keys / vals / merge；string.split / starts_with / ends_with / trim；os.setenv / getenv。显式传栈索引，不走硬编码索引。
  - `sandbox.h`：库白名单（`luaL_openselectedlibs`）、自定义 `lua_Alloc` 内存上限、`lua_sethook` 指令/时间预算；超预算后把 hook 计数设为 1，防 `while true do pcall(f) end` 吞错误。
  - `LuaStateView::SetRawFunction`：最热路径的逃生门，直接挂裸 `lua_CFunction`（不经过 trampoline/参数检查）。
  - `coroutine.h`：`Coroutine::Create` 把函数移到新线程并在 registry 锚定；`Resume` 返回 `{yielded, results}`；`lua_closethread` 回收。`ResumeToken` 可在任意线程析构（只减原子引用计数，不碰 VM）；`Cancel()` 置原子标志，运行中的协程由 count hook 中断，挂起中的协程下一次 `Resume` 返回 `kCancelled`。超时由宿主定时器触发 `Cancel`。

### 职责边界（硬约束：不属于绑定库的一律不做）

评审规则：下面四项归**框架 / 运行时 / 诊断库**，breeze::lua_binding **不实现**；绑定库最多提供最小接入点（例如 `Cancel()`），不提供策略、线程归属、定时器或栈采集。实现这些的提交应直接拒绝，需求落到对应层。

| 项 | 真正归属 | breeze::lua_binding 的处理 |
|---|---|---|
| 跨线程 Reference 析构队列 | 框架/业务的线程模型（Shard 线程归属、消息回投） | **不实现**；非 owner 线程只记录一次并泄漏槽，规则见上 |
| 协程超时定时器 / 看门狗 | 运行时/框架（`base/timer` 的 `TimerWheel`） | **不实现**；只提供 `Cancel()`，宿主 timer 到点调用 |
| 沙箱与协程的 hook 冲突 | Lua 本身只有单一 hook 槽 + 可选模块集成 | **不做 hook 多路复用器**；文档要求同一协程不要同时开预算与取消 hook |
| C++ 调用栈 | 诊断库（`base/backtrace`）或业务错误处理 | 核心只做 `luaL_traceback`；需要 C++ 栈由 framework 在错误路径接 |

**不做清单（禁止实现）**

- `Reference` / `LuaTable` / `LuaFunction` 的跨线程析构队列、延迟释放、GC 线程回收：线程归属由框架保证，异步只传 ID；库的行为固定为“记录一次 + 泄漏槽”，不为使用错误加运行时设施。
- 协程超时定时器、自动 `Resume`、看门狗线程：超时策略属于调度器。
- sandbox 与 coroutine 的 hook 复用：Lua 只有一个 hook 槽，库不维护第二个策略层。
- 核心里的 C++ 栈采集、符号化、崩溃上报：属于 `base/backtrace` / 业务诊断。
- 二进制 codec、协议、缓冲区管理（luakit 的 `lua_codec.h` / `lua_buff.h` / `lua_slice.h`，即 `encode` / `decode` / `serialize` 那套）：**一律不提供**；需要时用独立的协议/缓冲区包。

**其他“看起来像 Lua 库、但不是绑定库”的东西**

这些经常被顺手塞进绑定库，实际各有归属。breeze::lua_binding 只提供最小的接入点，不实现它们：

| 项 | 真正归属 | 绑定库只负责 |
|---|---|---|
| 定时器 / 调度器（`ltimer`、skynet 的 timer、breeze 的 `TimerWheel`） | 运行时 | 不做调度；协程超时只暴露 `Cancel()`，由宿主定时器触发 |
| 沙箱 | 独立的 sandbox 层 | `luaL_openselectedlibs` 白名单、`lua_Alloc` 接入；`sandbox.h` 的预算 hook 只是可选接入，隔离等级和策略由宿主定 |
| 热更 | 框架的 reload 体系（quanta 的 `import`、skynet 的 hotfix） | 只保证 `CallGlobal` 按名字晚绑定、能重新解析；不做文件 mtime、import 缓存、`__reload` 钩子 |
| 序列化（protobuf / JSON 等） | 独立库 | 不做编解码；二进制 codec 是单独包 |
| 线程池 / 协程池 | 运行时资源管理 | 只提供 `Coroutine` 原语与 `ResumeToken` 取消；不做池、队列、调度 |
| 对象池 / 内存分配器 | 运行时 | 只接受宿主传入的 `lua_Alloc`（`LuaStateOptions`），不做池化策略 |

其中 **`luabuf` / `slice` / codec 最容易越界**：breeze::lua_binding 不提供 `luabuf`、`slice`、`lua_codec`，也不提供 `encode` / `decode` / `serialize` 接口。sol2、LuaBridge3 同样没有（sol2 的 `sol::bytecode` 只是 Lua chunk 的 dump/load，不是对象编解码）；luakit 有是因为 quanta 把 RPC 编解码和绑定做在一起，breeze::lua_binding 不复制这条路线。

**绑定库能力：已覆盖 / 仍有缺口**

已覆盖：

- `__tostring` / `__eq` / `__lt` / `__close` 自动检测（见“自动元方法”）；其它元方法用 `.MetaMethod()` 显式注册。
- 容器转换：`vector`、`array`、`map`、`unordered_map`、`optional`、`span`（push-only）、`tuple`/`pair`（多返回值）。
- `shared_ptr` 所有权（见上）。**这不是跨线程特性**，是单线程也需要共享生命周期：实体由 C++ 容器持有、Lua 拿句柄。

`shared_ptr` 的使用边界（不是缺口）：

| 项 | 说明 |
|---|---|
| `Get<std::shared_ptr<T>>` | 只对“按 shared_ptr 推入”的对象成立；`Class<T>().Constructor` 创建的 owned 对象和 borrowed 对象没有控制块，返回 `kType`。 |
| `weak_ptr` | **不绑定**。Lua 侧没有安全的非拥有句柄：要么锁成强引用（就是 shared），要么每次访问判空并保活，代价大。C++ 侧需要弱观察时用 `std::weak_ptr`；Lua 侧需要“不延长生命周期”的缓存时用 ID。sol2 / LuaBridge3 也都不支持 `weak_ptr`。 |
| `enable_shared_from_this` | 不自动使用；将来若要“从 Lua 反解 owned 对象”，再加这个回退。 |

### 跨语言生命周期规则（必须遵守）

两套 GC 互相看不见：C++ 的 `Reference`/`LuaFunction` 是 Lua registry 的强引用（GC root）；Lua 的 userdata 持有 `shared_ptr`。因此**没有任何库能自动回收跨语言环**，sol2 / LuaBridge3 / luakit 都靠约定，skynet / quanta 靠 ID 和按名字调用绕开。

典型环：

1. C++ 实体成员 `LuaFunction on_update` → registry 强引用 Lua 闭包；
2. 闭包捕获 Lua 的 `entity` 变量/表 → 持有实体 userdata；
3. userdata 的 `shared_ptr<Entity>` → 实体本身。

结果：registry 引用让闭包永不回收 → `entity` 永不回收 → userdata 永不回收 → 实体永不析构。`entity = nil` 也没用，因为 C++ 的 `Reference` 仍是根。

硬性规则：

- **单向强所有权**：实体由 C++ 管理器拥有；Lua 侧优先用 borrowed 句柄或 ID；只有确实需要跨帧共享时才用 `shared_ptr`。
- **C++ 不长期持有 Lua 回调**：优先按名字调用（`CallGlobal`、luakit 风格的 `object_call`）或“ID + Lua 表查函数”。必须存 `LuaFunction`/`Reference` 时，卸载路径必须显式清空（实体 `unload`/`destory`、定时器取消、连接关闭）。
- **Lua 缓存 C++ 对象不要形成回指**：用 ID 查管理器；用 borrowed 指针时只能保证“同步调用期间”有效；不要用 shared_ptr 让 Lua 对象反过来拥有 C++ 里又引用 Lua 的东西。
- **不要指望 `weak_ptr` 或 Lua 弱表**：`weak_ptr` 不在绑定层；Lua 的 `__mode` 弱表只处理 Lua 内部对象，registry 的 C++ 强引用是外部根，弱表管不到。
- **C++ 侧 `shared_ptr` 之间成环**是标准 C++ 问题，用 `std::weak_ptr` 在 C++ 内部打破；绑定层不介入。
- **排查手段**：统计存活的 `LuaFunction`/`Reference` 数量、实体 weak_ptr 是否过期、`lua_close` 前是否还有业务引用；泄漏时先找“C++ 存 Lua 回调 + Lua 存 C++ 对象”的双向强引用。

仍有缺口（按需求排期，不推给业务）：

| 项 | 说明 |
|---|---|
| `__le`、算术/位运算、`__len`/`__pairs` 等元方法 | 不自动检测；用 `.MetaMethod()` 挂裸 `lua_CFunction`。 |

Lua 5.1–5.4 / LuaJIT / Lua 按 C++ 编译**不属于缺口**：本项目 scope 固定 Lua 5.5.1，不做多版本兼容（见“没做的”）。

**已定的 API 策略（保持，不是遗漏）**

- `Result::Value()` 失败 abort；核心不依赖 C++ 异常，可在 `-fno-exceptions` 下编译。
- `Reference` / `LuaTable` / `LuaFunction` move-only，**不提供隐式拷贝，需要时用显式 `Clone()`**：拷贝必须 `lua_pushvalue` + `luaL_ref` 新槽，只能在 owner 线程做；一旦类型可拷贝，`std::function` / 容器在 worker 线程的隐式复制会无声破坏线程规则。`Clone()` 的语义见下。
- 点号 / 冒号按类显式选择，不做自动识别（启发式有歧义）。

这三项在其它绑定库里的对应情况（避免把“我们的选择”误说成“行业的做法”）：

| 项 | sol2 | LuaBridge3 | luakit | breeze::lua_binding |
|---|---|---|---|---|
| 不抛异常、返回错误 | 没有 `Result<T>`；提供 `SOL_NO_EXCEPTIONS`，错误走 abort / panic | 有 `Result` / `TypeResult`（`std::error_code`）；`value()` 不检查，`throw_on_error()` 仅在启用异常时抛 | 没有，返回 `bool` 或 `luaL_error` | `Result<T>`；`Value()` 失败 abort；核心可在 `-fno-exceptions` 下编译 |
| `Reference` 可拷贝 | 可拷贝（复制出新的 registry 槽） | 可拷贝 | 可拷贝（复制出新的 registry 槽） | **不允许隐式拷贝；显式 `Clone()` 新建独立槽** |
| 二进制 codec | 无（`sol::bytecode` 只是加载 Lua chunk，不是对象编解码） | 无 | 有 `lua_codec.h` / `lua_buff.h` / `lua_slice.h` | 无，独立包 |

结论：**无异常构建是行业常见需求**，sol2 / LuaBridge3 都支持；`Result` 也不是我们独有（LuaBridge3 有，只是 `value()` 不检查）。**`Reference` move-only 是 breeze::lua_binding 自己的 API 选择，不是行业惯例**——主流三家都可拷贝。**二进制 codec 与 sol2 / LuaBridge3 一致（不做），与 luakit 不同**（luakit 绑定和序列化是一体的）。

**需要拷贝时的设计：显式 `Clone()`，独立 registry 槽**

- **语义**：`Clone()` 对同一个 Lua 值执行 `lua_pushvalue` + `luaL_ref`，得到新的 registry 引用。两个句柄各自 unref、生命周期独立；底层 Lua 对象仍是同一个（改表字段两边都可见）。这和 sol2 / LuaBridge3 / luakit 的拷贝语义一致，也不引入共享槽的引用计数。
- **API**：`Reference::Clone()`、`LuaTable::Clone()`、`LuaFunction::Clone()` 返回同类型；没有拷贝构造/拷贝赋值。不用 `explicit` 拷贝构造是因为拷贝赋值无法 explicit，且容器 / `std::function` 仍会隐式复制，线程规则会被绕过。
- **规则**：
  - 只能在 owner 线程调用（访问 VM，和 `Push` 一样）；
  - 空引用返回空引用；state 已关闭是用法错误（`BREEZE_LUA_CHECK`）；
  - 每次 `Clone()` 占一个 registry 槽，热路径不要用；循环里只在循环外 clone 一次。
- **典型用法**：

```cpp
LuaFunction cb = lua.GetFunction("on_tick").Value();
LuaFunction saved = cb.Clone();                       // 独立生命周期
auto shared = std::make_shared<LuaFunction>(cb.Clone());
std::function<void(int)> f = [shared](int x) { shared->Call<void>(x); };
```

- **什么时候不该 Clone**：函数参数用 `const Reference&`；同线程短期访问用 `LuaTableView` / `LuaFunctionView`；跨线程 / 异步只传 ID；同一对象只存一份，其他持有者用指针或引用。
- **为什么不做共享槽**：共享槽需要每个槽的引用计数（额外堆块或原子操作），析构仍必须在 owner 线程；收益只是省一次 `luaL_ref`，不值得。

单线程工程里也基本用不到拷贝：常见的回调落地方式是“按名字调用”（`CallGlobal` / luakit 的 `object_call`）、“按 ID 分发”（定时器、网络回调在 Lua 表里查），或每个 C++ 对象只存一份（移动进去）。luakit 虽然可拷贝，但 quanta 的 C++ 扩展并没有把 `reference` 存成成员，也印证了这一点。真需要两份独立引用时，一次 `Clone()` 即可覆盖。

### 没做的

- 二进制 codec / `luabuf` / `slice` / MPSC ring（luakit 的 encode/decode/serialize 那套）：强依赖协议与缓冲区，单独包（见“不做清单”）。
- 跨线程待释放引用队列、协程超时定时器、hook 复用器、C++ 栈采集：见“不做清单”，属于框架/运行时/诊断库，breeze::lua_binding 不做；协程池也暂不做（取消 token 已实现）。
- LuaJIT / Lua 5.1–5.4 兼容；Lua 按 C++ 编译（会改变 `lua_error` 语义）。scope 固定 Lua 5.5.1，不计划支持低版本。
- 点号兼容类的冒号调用自动识别（启发式容易歧义；一个类要么冒号，要么点号）。
- 完整 asan / ubsan / tsan 已在 x86_64 Linux（clang 23）跑过；ARM、GCC 单独编译和真机压测没做。
- Free-threaded Lua / 多线程共享一个 state（Lua 本身不支持）。

## 4. 验证

测试（与源码同目录，`CHECK` 失败 abort，不引测试框架）：

- `lua_core_test`：全局读写、C++ 函数/lambda、多返回值、脚本错误、optional/vector/array/span/unordered_map、LuaTable/LuaFunction、`Clone()`、返回值类型检查、`CallUnprotected` / `CallGlobalUnprotected`、`lua_close` 后 `Reference`、可调用对象 GC、C++ 异常转 Lua 错误。
- `lua_check_test`：`ENSURE` 每个调用点只记一次；`CHECK` 在 fork 子进程里 abort。
- `lua_class_test`：构造、方法、属性、继承（属性/方法 cast）、AllowFields、DotCall、所有权/GC、枚举、自动元方法（`__tostring` / `__eq` / `__lt` / `__close`，含继承）、`shared_ptr` 生命周期。
- `lua_sandbox_test`：内存上限（`AllocStats.failures`）、指令预算（`while true do end` 被中断）、库白名单。
- `lua_coroutine_test`：yield / resume / done / 再次 resume 失败；取消（挂起时与运行中）、token 跨线程析构。
- `lua_ext_test`：table/string/os 工具、循环引用 deepcopy。
- `lua_core_noexc_test`：同一份核心测试用 `-fno-exceptions` 再编一遍。
- `lua_apicheck_selftest`：debug/asan 预设链接带 `LUA_USE_APICHECK + LUAI_ASSERT` 的 Lua；子进程故意 `lua_pushvalue(L, 100)`，必须断言 abort，证明开关真的生效。

跑法：

```
cmake --preset debug && cmake --build build/cpp && ctest --test-dir build/cpp -R lua
cmake --preset release -DBREEZE_BUILD_LUA_BINDING_BENCH=ON && ./build/cpp-release/lua_binding_bench
```

Windows 上单独用 `build/lua_binding_dev` 的临时 CMake（MinGW g++ 16 + quanta 里的 Lua 5.5.1 源码）跑过同一套测试与基准。

实测（Windows，MinGW g++ 16，Release，ns/op；竞品数据来自 `breeze/build/lua_bench` 的临时对比基准）：

| 场景 | 裸 C API | breeze::lua_binding Call | breeze::lua_binding CallNoTraceback | breeze::lua_binding CallUnprotected | sol2 unsafe | sol2 safe | LuaBridge3 | luakit |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| C++→Lua noop（缓存） | 10.5 | 14.5 | 12.7 | **8.8** | 9.2 | 16.8 | 19.4 | 43.9 |
| C++→Lua add（缓存） | 15.6 | 19.5 | 19.5 | **14.9** | 14.3 | 24.1 | 27.6 | 50.5 |
| C++→Lua 按名字 | — | 28.8 | — | 23.2 | — | — | — | — |
| Lua→C++ add | 14.9 | 17.5 | — | — | 19.8 | 18.6 | 22.9 | 15.0 |
| Lua→C++ 读表 4 字段 | 45.6 | 68.4 | — | — | 103.8 | 105.8 | 268.6 | 49.8 |
| 热路径分配 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 / 10 万次调用 |

其中 sol2 的三列模式：

- **unsafe**：`SOL_SAFE_FUNCTION_OBJECTS` 关闭（Release 默认），`sol::function` = `unsafe_function`，走 `lua_call`；
- **safe**：编译时 `-DSOL_SAFE_FUNCTION_OBJECTS=1`，`sol::function` = `protected_function`，走 `lua_pcall`。

Linux Release（clang 23）实测：

| 场景 | breeze::lua_binding Call | CallNoTraceback | CallUnprotected | sol2 unsafe | sol2 safe |
|---|---:|---:|---:|---:|---:|
| C++→Lua noop（缓存） | 14.8 | 12.0 | 7.7 | 10.2 | 19.3 |
| C++→Lua add（缓存） | 19.2 | 18.7 | 14.1 | 15.2 | 25.4 |
| C++→Lua 按名字 | 27.5 | — | 22.1 | — | — |
| Lua→C++ add | 15.8 | — | — | 18.4 | 19.4 |
| Lua→C++ 读表 4 字段 | 56.0 | — | — | 91.9 | 95.1 |

这轮 C++→Lua 优化（`LuaFunction` 只取一次 state、`Call` 先压 msgh 免 `lua_insert`、`CallNoTraceback` void 跳过 `settop`）把受保护的缓存 noop 从 Windows 17.2 / Linux 18.6 降到 **14.5 / 14.8**；`CallNoTraceback` **12.7 / 12.0**；`CallUnprotected` **8.8 / 7.7**。

**sol2 的 unsafe 列不是同一口径**：sol2-solpp 在 Release 下 `SOL_SAFE_FUNCTION_OBJECTS` 默认关闭，`sol::function` 等于 `unsafe_function`，内部走 `lua_call`（`unsafe_function.hpp:43`），不捕获错误。打开 `SOL_SAFE_FUNCTION_OBJECTS=1` 后重新实测：

- Windows：noop **16.8**、add **24.1**（breeze::lua_binding `Call` 14.5 / 19.5，`CallNoTraceback` 12.7 / 19.5）；
- Linux：noop **19.3**、add **25.4**（breeze::lua_binding `Call` 14.8 / 19.2，`CallNoTraceback` 12.0 / 18.7）。

**同为受保护调用时，breeze::lua_binding 比 sol2 safe 快 2~6ns**；sol2 unsafe 仍然最快（9.2 / 10.2），但它不捕获错误。breeze::lua_binding 的同口径逃生门 `CallUnprotected` 是 8.8 / 7.7，和 sol2 unsafe 持平或略快。默认 breeze::lua_binding 仍然是 pcall。

读表 4 字段在 Linux 从 74ns 降到 56ns：热路径改用 `LuaTableView::TryGetField`（`lua_getfield` + `bool`，不构造 `Result`），已经接近裸 API 的 40.5ns（Linux）/ 45.6ns（Windows）。

对照组：同样的 handler 用 `SetRawFunction` 绑裸 `lua_CFunction` 时，Lua→C++ add 13.9（Windows）/ 12.8（Linux）、读表 4 字段 44.1 / 43.4ns——已经与 luakit 的 15.0 / 49.8 持平或更快。luakit 在基准里赢的两项，是因为它的 handler 本来就是裸 `lua_CFunction`；breeze::lua_binding 通用绑定多出的 ~4ns / ~20ns 是 ErrorSlot、参数检查、tuple 与 noinline trampoline 的安全成本，不是运行时差异。

**没验证的**：大量并发协程/沙箱实例的长期稳定性；真实 quanta 脚本的迁移测试；GCC 本机没有单独跑（Linux 用 clang 23 + GCC 16 的 libstdc++）。

## 未决项

- `LUA_USE_APICHECK` overlay 端口的 feature 机制在其它 vcpkg 版本上是否稳定（当前用 2026.09.26 vcpkg + 5.5.1 port）。
- `Reference` 拷贝已定：不做隐式拷贝，提供显式 `Clone()`（独立 registry 槽、owner 线程）。
- 无：跨线程 `Reference` 析构已定为“记录并泄漏 + 文档约束”，不做队列。
- `LuaTableView` 批量读取 API（当前逐字段 55ns，可进一步合并成一次 C 调用）。
- 是否提供 C++26 `std::expected` 互转（现在只有 `Result`）。
