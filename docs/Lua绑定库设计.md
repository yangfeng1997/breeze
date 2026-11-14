# Lua 绑定库：`base/fastlua`

`fastlua`：Lua 5.5.1 的 C++ 绑定库。小型编译库（`breeze_fastlua`）加头文件模板；链式 `Class<T>` 构建器为主，luakit 风格键值列表是同一个构建器上的糖衣；默认冒号调用零分配；错误不依赖 C++ 异常（两段式 trampoline + `Result<T>`）；可选 `breeze_fastlua_ext`（table / string / os 工具）、`sandbox.h`（内存 / 指令 / 时间预算）、`coroutine.h`。

依赖 vcpkg 的 `lua` 端口（5.5.1，按 C 编译）。**C++26 为基线**（仓库根 `CMakeLists.txt` 设 `CMAKE_CXX_STANDARD 26`，`breeze_fastlua` 也以 `target_compile_features(... cxx_std_26)` 声明）；不需要网络库，Linux 与 Windows（MSYS2 g++ 16）都能构建。

C++26 下可用的 `std::expected`、`std::move_only_function`、`std::to_underlying`、`std::unreachable` 等（多数从 C++23 起就有），fastlua **没有为了用而用**：`Result<T>` 仍是主错误类型，内部也不依赖 `std::move_only_function`。升级标准只是为了不再为低版本兼容性做取舍。

本文记录**为什么是这个样子**。API 的细节与约束写在头文件注释里，本文不重复。

**范围约束**：不属于 Lua 绑定库的职责——跨线程 `Ref` 析构队列、协程超时定时器、sandbox/coroutine 的 hook 复用器、C++ 栈采集、二进制 codec（`luabuf` / `slice` / `encode` / `decode` / `serialize`）——**一律不做**，需求由框架 / 运行时 / 诊断库或独立包承担；详见 §3“职责边界”。

**状态**：debug、release、`-fno-exceptions`、C++26 构建、以及带 `LUA_USE_APICHECK` 的 Lua 下测试通过；**asan + ubsan 与 tsan 预设下 9 个 fastlua 测试全部通过**（修掉了一处 `StateData` 桶数组泄漏）。release 基准在 Windows（MinGW g++ 16）与 Linux（clang 23）上跑过。Lua 的 apicheck 只在 debug/asan/tsan 预设里通过 vcpkg overlay 打开。

---

## 1. 来源与取舍

参考了三个现成实现：

- **quanta 的 `extend/luakit`**：游戏行业习惯的来源（`new_class` 键值列表、`new_enum`、按名字调用、`object_call` 钩子、工厂 + `__gc` 所有权）。它的实现问题见 §2。
- **sol2（solpp 分支）**：C++→Lua 缓存调用的性能最好（接近裸 API），因为它把错误处理函数存成引用、`sol::function` 默认走不受保护的 `lua_call`；但上游不支持 Lua 5.5，必须用分支。
- **LuaBridge3**：Lua 5.5 兼容最明确、单头、轻量；但每个参数都走 `Stack<T>::get` + `TypeResult`，读表字段 269ns，且 `raise_lua_error` 在 catch 块里执行（C 编译的 Lua 下会泄漏异常对象）。

取舍：

- **不要每个调用都查 `debug.traceback`**：luakit 的 C++→Lua 慢 4 倍就是每次 `getglobal` + `getfield`。fastlua 的 `Function::Call` 用无 upvalue 的 light C 函数作 msgh（`lua_pushcfunction` 不分配），热路径另提供 `CallFast` 不挂 traceback。
- **参数先全部检查、再读取**：检查阶段只用平凡存储，避免在 `lua_error` longjmp 前留下带析构的临时对象；错误消息用 `luaL_typeerror` / `luaL_argerror`（带参数序号与期望/实际类型）。
- **不用 `std::function` 做每次调用**：自由函数 / lambda 注册时放在 userdata upvalue 里，调用走模板蹦床；成员函数同理（upvalue 1 成员指针，upvalue 2 声明类）。
- **类身份不用字符串哈希**：`kTypeTag<T>` 的地址作 key；基类转换用注册时生成的 `static_cast`，虚继承也正确。
- **错误不抛异常**：核心在 `-fno-exceptions` 下编译；C++ 调 Lua 一律 `lua_pcall` 并返回 `Result`；绑定函数在 `Invoke`（noinline）里返回，之后才在只有平凡局部变量的那一层 `lua_error`。
- **不用 `std::expected` 替换 `Result<T>`**：`Result<T>` 是公共 API，成功时不分配堆内存；C++26 下可以提供互转，但不改主接口。

## 2. 原实现里发现并处理的问题

luakit（`extend/luakit/include`）：

- 每次调用重新查 `debug.traceback`（`lua_function.h:259-261`）：C++→Lua 44~58ns vs 裸 API 10~25ns。
- 访问方法每次都分配一个闭包（`lua_class.h:49-54`），`__index` 还做多次哈希查找；fastlua 默认 `__index = 方法表`，只有属性类才用 C 闭包。
- 对象是“代理表 + lightuserdata”缓存在按地址索引的弱表里：地址复用会拿到陈旧代理，C++ `delete` 后 Lua 侧不失效。fastlua 直接用 userdata + cookie，裸指针不再按地址缓存。
- 没有类型 / 参数检查（`lua_to_object` 把任意带 `__pointer__` 的表或 userdata 强转），传错类型是未定义行为；nil 悄悄变成 0 / "" / nullptr。
- `luaL_error(L, e.what())` 是格式串漏洞，而且写在 catch 块里（`lua_codec.h:309/399`）；`printf(msg.c_str())`（`lua_logger.h:19`）同理。
- map / set 转换里对 key 调 `lua_tolstring` 会原地把数字转成字符串，破坏 `lua_next`。
- `thread_local` 全局（`lbuf`、`glogger`）按 DLL / 线程各一份；logger 绑定到第一个 `L`，`lua_close` 后悬空。
- `reference` 从 `lua_State*` 非 explicit 构造并弹栈、无拷贝赋值、`lua_close` 后析构是未定义行为。
- 头文件不自包含、`_mm_pause` 没有 include、`WIN32` 写成 `WIN32`、`typeid(T).name()` 作元表 key、未对齐的 `*(T*)` 读写等可移植性问题。

sol2 / LuaBridge3：前者上游不支持 5.5（`compat-5.3.h` 的 `#error`、`LUA_ERRGCMM`、`lua_newstate` 参数）；后者在 catch 块里 `lua_error`，且逐字段读表很慢。fastlua 把两者的优点（sol2 的 handler 缓存与 trampoline、LuaBridge3 的类型身份与 `ArgStorage` 思路）拿过来，避开这些问题。

## 3. 设计

- **`LuaState` / `LuaStateView`**：`LuaState` 拥有 `lua_State`（`lua_newstate` 或 `luaL_newstate`），`LuaStateView` 是非拥有视图，用于 `luaopen_*`。一个 state 绑定一个线程（一个 Shard）。
- **`Stack<T>`**：一个 customization point。内置整数（范围/可表示性检查）、浮点、bool、enum、`std::string`、`std::string_view`、`const char*`、`optional`、`vector`、`map`、`tuple`/`pair`（仅返回值多值）、对象指针/引用、`TableView` 栈视图。
- **两段式 trampoline**（`trampoline.h`）：`Invoke` 里检查参数、构造临时对象、调用用户函数、压返回值；它 normal return 之后，`Trampoline`（只有 `ErrorSlot` 等平凡局部变量）再调用 `RaiseError` → `luaL_typeerror` / `luaL_error`。这样就算 Lua 按 C 编译、`lua_error` 是 longjmp，也不会跳过任何析构。
- **`Result<T>`**：`Ok()` / `Value()` / `ValueOr()` / `GetError()` / `NotFound()`；错误码含 kNotFound / kRuntime / kSyntax / kMemory / kType / kStack / kBudget / kCancelled / kTimeout 等。缺函数是 `kNotFound`，不是异常。
- **调用**：`CallGlobal`（按名字，晚绑定，热更友好）、`CallPushed`（函数已在栈上）、`Function::Call`（缓存引用，带 traceback）与 `Function::CallFast`（不挂 traceback）。每次调用一次 `lua_checkstack`，`lua_pcall` 后 `lua_settop` 恢复。
- **类（`class.h` / `class.cpp`）**：
  - userdata 布局 `[ObjHeader][外部对象指针]`；`ObjHeader` 带 cookie、ClassInfo、对象指针、所有权标志。
  - 类型身份 `kTypeTag<T>` 地址；`GetObjectAs<T>` 先比类型，再沿 `ancestors` 应用注册时生成的 `up` 转换。
  - 每类一个 metatable；纯方法类的 `__index` 直接是方法表，属性类用 C `__index` 查 getters 表（lightuserdata `PropDesc*`，直接调用 getter/setter，无嵌套 Lua 调用）。
  - 所有权：`Policy::kOwned`（默认，`__gc` delete）、`Policy::kBorrowed`（C++ 管，`__gc` 不做事）；`ObjHeader::flags` 决定是否 delete。
  - `AllowFields` 把未知字段存到 uservalue 表（`lua_newuserdatauv` 的 uservalue 数在创建时按类决定）；`DotCall` 只对显式开启的类在方法访问时包装一个闭包，供 quanta 点号脚本迁移。
  - 构造函数：`Ctor<Args...>` 设置类表的 `__call`；参数从索引 2 开始，`new T(args...)`，失败转 `ErrorSlot`。
- **引用（`ref.h`）**：`Ref` / `Table` / `Function` 是 registry 引用，move-only；`Control` 引用计数块，`StateData` 的 `__gc` 在 `lua_close` 时把 `state` 置空，所以 `lua_close` 之后析构 `Ref` 是空操作。**线程规则见下，库不实现跨线程引用释放队列。**
- **线程规则（必须遵守）**：
  - 一个 `LuaState` 只属于一个线程；`Ref` / `Table` / `Function` 必须在该线程创建、使用、析构。
  - 异步 worker / IO / DB 线程只传 ID、令牌或序列化数据，不传 `Ref`；需要 Lua 回调时把 ID 投回属主线程，再查表取 `Ref`。
  - 任意线程可析构的 C++ 句柄（如 breeze 的 `ReplyHandle`）不得直接持有 `Ref`，包装层只存 ID。
  - 违反是使用错误：Debug/Release 都只记录一次并泄漏该 registry 槽，不碰 VM，避免 UB。**这是刻意不做跨线程释放队列**：sol2、LuaBridge3、luakit 等主流绑定库同样不做，线程归属由框架/使用者保证。
  - 其它绑定库也不能跨 OS 线程用同一个 `lua_State`：sol2 文档直接写 “Lua has no thread safety”，建议每个线程一个 state、跨 state 传值靠 C++ 侧序列化；`sol::main_*` / `lua_xmove` 只解决 **Lua 协程线程**（同一 OS 线程）的引用归属，不是 OS 线程安全。LuaBridge3 的 `LuaRef` 同样绑定到构造时的 state/thread，不提供同步；luakit 的 `reference` 析构直接 `luaL_unref`，换线程就是 UB。它们“能拷贝”不等于“能跨线程”——拷贝本身也要访问 VM。
- **可选模块**：
  - `ext/`：table.copy / deepcopy（visited 表处理环，深度上限 64）、size / keys / vals / merge；string.split / starts_with / ends_with / trim；os.setenv / getenv。显式传栈索引，不走硬编码索引。
  - `sandbox.h`：库白名单（`luaL_openselectedlibs`）、自定义 `lua_Alloc` 内存上限、`lua_sethook` 指令/时间预算；超预算后把 hook 计数设为 1，防 `while true do pcall(f) end` 吞错误。
  - `LuaStateView::SetRawFunction`：最热路径的逃生门，直接挂裸 `lua_CFunction`（不经过 trampoline/参数检查）。
  - `coroutine.h`：`Coroutine::Create` 把函数移到新线程并在 registry 锚定；`Resume` 返回 `{yielded, results}`；`lua_closethread` 回收。`ResumeToken` 可在任意线程析构（只减原子引用计数，不碰 VM）；`Cancel()` 置原子标志，运行中的协程由 count hook 中断，挂起中的协程下一次 `Resume` 返回 `kCancelled`。超时由宿主定时器触发 `Cancel`。
  - `compat/luakit.h`：`new_class` / `new_enum` / `new_table` / `call` / `table_call` / `set_function` / `set` / `get` 的 snake_case 转发，迁移时只换 include。

### 职责边界（硬约束：不属于绑定库的一律不做）

评审规则：下面四项归**框架 / 运行时 / 诊断库**，fastlua **不实现**；绑定库最多提供最小接入点（例如 `Cancel()`），不提供策略、线程归属、定时器或栈采集。实现这些的提交应直接拒绝，需求落到对应层。

| 项 | 真正归属 | fastlua 的处理 |
|---|---|---|
| 跨线程 Ref 析构队列 | 框架/业务的线程模型（Shard 线程归属、消息回投） | **不实现**；非 owner 线程只记录一次并泄漏槽，规则见上 |
| 协程超时定时器 / 看门狗 | 运行时/框架（`base/timer` 的 `TimerWheel`） | **不实现**；只提供 `Cancel()`，宿主 timer 到点调用 |
| 沙箱与协程的 hook 冲突 | Lua 本身只有单一 hook 槽 + 可选模块集成 | **不做 hook 多路复用器**；文档要求同一协程不要同时开预算与取消 hook |
| C++ 调用栈 | 诊断库（`base/backtrace`）或业务错误处理 | 核心只做 `luaL_traceback`；需要 C++ 栈由 framework 在错误路径接 |

**不做清单（禁止实现）**

- `Ref` / `Table` / `Function` 的跨线程析构队列、延迟释放、GC 线程回收：线程归属由框架保证，异步只传 ID；库的行为固定为“记录一次 + 泄漏槽”，不为使用错误加运行时设施。
- 协程超时定时器、自动 `Resume`、看门狗线程：超时策略属于调度器。
- sandbox 与 coroutine 的 hook 复用：Lua 只有一个 hook 槽，库不维护第二个策略层。
- 核心里的 C++ 栈采集、符号化、崩溃上报：属于 `base/backtrace` / 业务诊断。
- 二进制 codec、协议、缓冲区管理（luakit 的 `lua_codec.h` / `lua_buff.h` / `lua_slice.h`，即 `encode` / `decode` / `serialize` 那套）：**一律不提供**，也不在 `compat/luakit.h` 里伪造同名接口；需要时用独立的协议/缓冲区包。

**其他“看起来像 Lua 库、但不是绑定库”的东西**

这些经常被顺手塞进绑定库，实际各有归属。fastlua 只提供最小的接入点，不实现它们：

| 项 | 真正归属 | 绑定库只负责 |
|---|---|---|
| 定时器 / 调度器（`ltimer`、skynet 的 timer、breeze 的 `TimerWheel`） | 运行时 | 不做调度；协程超时只暴露 `Cancel()`，由宿主定时器触发 |
| 沙箱 | 独立的 sandbox 层 | `luaL_openselectedlibs` 白名单、`lua_Alloc` 接入；`sandbox.h` 的预算 hook 只是可选接入，隔离等级和策略由宿主定 |
| 热更 | 框架的 reload 体系（quanta 的 `import`、skynet 的 hotfix） | 只保证 `CallGlobal` 按名字晚绑定、能重新解析；不做文件 mtime、import 缓存、`__reload` 钩子 |
| 序列化（protobuf / JSON 等） | 独立库 | 不做编解码；二进制 codec 是单独包 |
| 线程池 / 协程池 | 运行时资源管理 | 只提供 `Coroutine` 原语与 `ResumeToken` 取消；不做池、队列、调度 |
| 对象池 / 内存分配器 | 运行时 | 只接受宿主传入的 `lua_Alloc`（`LuaStateOptions`），不做池化策略 |

其中 **`luabuf` / `slice` / codec 最容易越界**：fastlua 不提供 `luabuf`、`slice`、`lua_codec`，也不提供 `encode` / `decode` / `serialize` 接口，`compat/luakit.h` 故意不转发这些名字。sol2、LuaBridge3 同样没有（sol2 的 `sol::bytecode` 只是 Lua chunk 的 dump/load，不是对象编解码）；luakit 有是因为 quanta 把 RPC 编解码和绑定做在一起，fastlua 不复制这条路线。

**属于绑定库、v1 未覆盖（按需求排期，不能推给业务）**

| 项 | 说明 |
|---|---|
| `__tostring` / `__eq` / `__lt` / `__close` | 绑定库核心能力，sol2 / LuaBridge3 都有；v1 未做 |
| `shared_ptr` / `weak_ptr` 所有权策略 | 绑定库所有权模型；v1 未做 |
| `array` / `span` / `unordered_map` 等容器转换 | 绑定库转换层；v1 未做 |
| Lua 5.1–5.4 / LuaJIT / Lua 按 C++ 编译 | 绑定库兼容层；当前 scope 只支持 5.5.1 |

这些是绑定库自己的功能缺口，**不要用“别的库也没做”当理由跳过**；是否实现只看业务需求，不改变“框架职责不越界”的约束。

**已定的 API 策略（保持，不是遗漏）**

- `Result::Value()` 失败 abort；核心不依赖 C++ 异常，可在 `-fno-exceptions` 下编译。
- `Ref` / `Table` / `Function` move-only，**v1 不提供拷贝**：拷贝必须 `lua_pushvalue` + `luaL_ref` 新槽，只能在 owner 线程做；一旦类型可拷贝，`std::function` / 容器在 worker 线程的隐式复制会无声破坏线程规则。需要独立引用时显式 `Push()` 后 `FromStack`；短期使用优先 `TableView` / `FunctionView` / `CallGlobal`。
- 点号 / 冒号按类显式选择，不做自动识别（启发式有歧义）。

这三项在其它绑定库里的对应情况（避免把“我们的选择”误说成“行业的做法”）：

| 项 | sol2 | LuaBridge3 | luakit | fastlua |
|---|---|---|---|---|
| 不抛异常、返回错误 | 没有 `Result<T>`；提供 `SOL_NO_EXCEPTIONS`，错误走 abort / panic | 有 `Result` / `TypeResult`（`std::error_code`）；`value()` 不检查，`throw_on_error()` 仅在启用异常时抛 | 没有，返回 `bool` 或 `luaL_error` | `Result<T>`；`Value()` 失败 abort；核心可在 `-fno-exceptions` 下编译 |
| `Ref` 可拷贝 | 可拷贝（复制出新的 registry 槽） | 可拷贝 | 可拷贝（复制出新的 registry 槽） | **move-only，刻意偏离** |
| 二进制 codec | 无（`sol::bytecode` 只是加载 Lua chunk，不是对象编解码） | 无 | 有 `lua_codec.h` / `lua_buff.h` / `lua_slice.h` | 无，独立包 |

结论：**无异常构建是行业常见需求**，sol2 / LuaBridge3 都支持；`Result` 也不是我们独有（LuaBridge3 有，只是 `value()` 不检查）。**`Ref` move-only 是 fastlua 自己的 API 选择，不是行业惯例**——主流三家都可拷贝。**二进制 codec 与 sol2 / LuaBridge3 一致（不做），与 luakit 不同**（luakit 绑定和序列化是一体的）。

**关于 `Ref` 是否要支持拷贝**：v1 明确**不加隐式拷贝**。核心理由不是“别的库不做”（它们都做），而是拷贝要访问 VM、只能在 owner 线程执行；一旦可拷贝，`std::function`、容器在 worker 线程的隐式复制会绕过线程检查。现在已有显式替代：`Push()` 后 `FromStack` 即可得到独立引用。若后续出现明确需求（例如必须放进 `std::function`），优先加**显式** `Clone()`（仍要求 owner 线程），而不是恢复拷贝构造/拷贝赋值。

单线程工程里也基本用不到拷贝：常见的回调落地方式是“按名字调用”（`CallGlobal` / luakit 的 `object_call`）、“按 ID 分发”（定时器、网络回调在 Lua 表里查），或每个 C++ 对象只存一份（移动进去）。luakit 虽然可拷贝，但 quanta 的 C++ 扩展并没有把 `reference` 存成成员，也印证了这一点。真需要两份独立引用时，`Push()` + `FromStack` 成本只有一次 `luaL_ref`，足以覆盖。

### 没做的

- 二进制 codec / `luabuf` / `slice` / MPSC ring（luakit 的 encode/decode/serialize 那套）：强依赖协议与缓冲区，单独包（见“不做清单”）。
- 跨线程待释放引用队列、协程超时定时器、hook 复用器、C++ 栈采集：见“不做清单”，属于框架/运行时/诊断库，fastlua 不做；协程池也暂不做（取消 token 已实现）。
- LuaJIT / Lua 5.1–5.4 兼容；Lua 按 C++ 编译（会改变 `lua_error` 语义）。
- 点号兼容类的冒号调用自动识别（启发式容易歧义；一个类要么冒号，要么点号）。
- 完整 asan / ubsan / tsan 已在 x86_64 Linux（clang 23）跑过；ARM、GCC 单独编译和真机压测没做。
- Free-threaded Lua / 多线程共享一个 state（Lua 本身不支持）。

## 4. 验证

测试（与源码同目录，`CHECK` 失败 abort，不引测试框架）：

- `fastlua_core_test`：全局读写、C++ 函数/lambda、多返回值、脚本错误、optional/vector、Table/Function、`lua_close` 后 `Ref`、可调用对象 GC、C++ 异常转 Lua 错误。
- `fastlua_check_test`：`ENSURE` 每个调用点只记一次；`CHECK` 在 fork 子进程里 abort。
- `fastlua_class_test`：构造、方法、属性、继承（属性/方法 cast）、AllowFields、DotCall、所有权/GC、枚举。
- `fastlua_sandbox_test`：内存上限（`AllocStats.failures`）、指令预算（`while true do end` 被中断）、库白名单。
- `fastlua_coroutine_test`：yield / resume / done / 再次 resume 失败；取消（挂起时与运行中）、token 跨线程析构。
- `fastlua_ext_test`：table/string/os 工具、循环引用 deepcopy。
- `fastlua_compat_test`：luakit 风格 `new_class` / `new_enum` / `new_table` / `call` / `table_call`。
- `fastlua_core_noexc_test`：同一份核心测试用 `-fno-exceptions` 再编一遍。
- `fastlua_apicheck_selftest`：debug/asan 预设链接带 `LUA_USE_APICHECK + LUAI_ASSERT` 的 Lua；子进程故意 `lua_pushvalue(L, 100)`，必须断言 abort，证明开关真的生效。

跑法：

```
cmake --preset debug && cmake --build build/cpp && ctest --test-dir build/cpp -R fastlua
cmake --preset release -DBREEZE_BUILD_FASTLUA_BENCH=ON && ./build/cpp-release/fastlua_bench
```

Windows 上单独用 `build/fastlua_dev` 的临时 CMake（MinGW g++ 16 + quanta 里的 Lua 5.5.1 源码）跑过同一套测试与基准。

实测（Windows，MinGW g++ 16，Release，ns/op；竞品数据来自 `breeze/build/lua_bench` 的临时对比基准）：

| 场景 | 裸 C API | fastlua | sol2-solpp | LuaBridge3 | luakit |
|---|---:|---:|---:|---:|---:|
| C++→Lua noop（缓存） | 10.5 | 17.2（fast 14.0） | 9.2 | 19.4 | 43.9 |
| C++→Lua add（缓存） | 15.6 | 19.6（fast 16.6） | 15.5 | 27.6 | 50.5 |
| C++→Lua 按名字 | — | 25.5 | — | — | — |
| Lua→C++ add | 14.9 | 17.0 | 20.2 | 22.9 | 15.0 |
| Lua→C++ 读表 4 字段 | 45.6 | 55.2 | 103.8 | 268.6 | 49.8 |
| 热路径分配 | 0 | 0 / 10 万次调用 | 0 | 0 | 0 |

读表 4 字段从 74ns 降到 55ns：热路径改用 `TableView::TryGetField`（`lua_getfield` + `bool`，不构造 `Result`），已经接近裸 API 的 45.6ns。

Linux Release（clang 23）实测：noop 缓存 18.6（`CallFast` 14.5）、add 缓存 23.2（`CallFast` 20.0）、按名字 26.5、table get 16.7、Lua→C++ add 17.2、读表 4 字段 55.2ns（`TryGetField`）；热路径 0 分配 / 10 万次调用。

对照组：同样的 handler 用 `SetRawFunction` 绑裸 `lua_CFunction` 时，Lua→C++ add 13.9（Windows）/ 12.8（Linux）、读表 4 字段 44.1 / 43.4ns——已经与 luakit 的 15.0 / 49.8 持平或更快。luakit 在基准里赢的两项，是因为它的 handler 本来就是裸 `lua_CFunction`；fastlua 通用绑定多出的 ~4ns / ~20ns 是 ErrorSlot、参数检查、tuple 与 noinline trampoline 的安全成本，不是运行时差异。

**没验证的**：大量并发协程/沙箱实例的长期稳定性；真实 quanta 脚本的迁移测试；GCC 本机没有单独跑（Linux 用 clang 23 + GCC 16 的 libstdc++）。

## 未决项

- `LUA_USE_APICHECK` overlay 端口的 feature 机制在其它 vcpkg 版本上是否稳定（当前用 2026.09.26 vcpkg + 5.5.1 port）。
- `Ref` 是否要支持拷贝（现在 move-only；luakit 的脚本习惯不需要）。
- 无：跨线程 `Ref` 析构已定为“记录并泄漏 + 文档约束”，不做队列。
- `TableView` 批量读取 API（当前逐字段 55ns，可进一步合并成一次 C 调用）。
- 是否提供 C++26 `std::expected` 互转（现在只有 `Result`）。
