# 主循环与 RPC 设计（C++ 侧）

**状态**：设计阶段，没有代码。文件名保留历史路径；原 `Actor 与线程模型设计` 已废弃，仅作历史备份：[Actor与线程模型设计-废弃.md](Actor与线程模型设计-废弃.md)，**不要参考**。

> **范围**：本文只设计 C++ 侧的进程内执行、Dispatcher、RPC/IDL、实体管理和进程拓扑。
>
> **已明确不做**：Actor 模型、`ActorSystem` / `ActorExecutor`、actor 池、actor 分片、进程内多 Shard、在线迁移/自动重平衡。原来的 Actor/Shard 设计全部删除。
>
> Lua 业务层是另一套：`base/lua_binding` + `LuaModule` / `AsyncWorkerModule`，见 [Lua绑定库设计.md](Lua绑定库设计.md) 和 [Lua脚本框架设计.md](Lua脚本框架设计.md)；本文不混入 Lua 线程模型。

---

## 0. 结论速查

1. **进程内主循环是唯一的业务执行器**：业务代码默认单线程，在 main loop 上直接执行。
2. **没有 Actor 模型、没有进程内 Shard**：不实现 `ActorSystem`、`ActorExecutor`、actor 池、actor kind、邮箱、分片、迁移。
3. **网络 IO 线程只做解码和投递**：按消息号把消息投到 main loop 的队列，不在 IO 线程执行业务。
4. **`Dispatcher` 是 App 成员的登记表**：消息号 → 主循环处理函数；`Init` 结束、`Start` 前冻结，IO 线程只读。
5. **阻塞/重计算走线程池**：`Offload` 投到 `ThreadPool`，结果作为消息回投 main loop；工作线程不碰模块/实体状态。
6. **RPC 一律异步**：没有同步版本；`CallHandle` 发起、`ReplyHandle` 应答，超时用 main loop 的 `TimerWheel`。
7. **IDL 用 gRPC schema 写法**：只用 schema 语法，不用 gRPC 运行时；自研生成器 + libprotobuf，`rpc` 签名决定 `Call` 或 `Send`。
8. **实体由 `EntityManager` 在 main loop 上管理**：`Adopt` / `Activate` / `Remove`，模块持有，不引入 actor。
9. **进程间是微服务拓扑**：服务工作在进程/服务实例之间拆分，扩容靠多进程；不做进程内多 Shard、不做在线迁移/自动重平衡。
10. **Lua 业务层独立**：Lua 的 `LuaModule` / `AsyncWorkerModule` 是另一套运行时设计，不和要求 Actor 模型挂钩。
11. **RPC 运行时独立成 `RpcModule`**：`MessageModule` 只管消息号/信封/派发与 transport 发送，`RpcModule` 在其上做注册、挂起、超时、`CallHandle`/`ReplyHandle`；payload codec 按 service/method 选 C++ protobuf 或 Lua protobuf 库。见 [RPC与IDL设计.md](RPC与IDL设计.md)。

---

## 1. 需求

- **实体**：玩家（数量多、单次处理短）、房间（数量中等）、场景（每帧 tick，单帧耗时可能大）。
- **业务单线程是默认约定**：main loop 上的模块不加锁；多线程只用于 IO、阻塞任务、Lua worker 等明确边界。
- **消息串行**：同一连接发来的消息在 main loop 上按顺序处理；业务状态只由 main loop 修改。
- **网络路径短**：IO 线程解完信封直接投 main loop 队列，不绕其他线程。
- **RPC 与 main loop 配合**：请求/应答都回到发起方所在的 main loop 处理。
- **停机排空**：main loop 停止前给模块机会存盘/清理。
- **不做进程内并行业务**：需要更多并行时，多开进程/服务实例，不用进程内 shard。

---

## 2. 总体结构

三层，互相正交：


| 层          | 回答的问题         | 选项                    |
| ------------- | -------------------- | ------------------------- |
| 进程内执行  | 处理函数在哪里跑   | **main loop 直接执行**  |
| 阻塞/重计算 | 怎么不卡 main loop | 线程池 + `Offload`      |
| 进程间拓扑  | 工作怎么拆成进程   | 单体 / 微服务 / sidecar |

数据流：

```text
网络 IO 线程
  │  解信封、校验
  ▼
Dispatcher（App 成员；消息号 → main loop handler；Start 前冻结）
  │
  ▼
MainLoopInbox 队列
  │
  ▼
App 主循环 / 业务模块 / EntityManager
  │
  ├─ RPC：发往 Agent / 远端服务；应答回到 main loop
  └─ Offload：投 ThreadPool；结果回投 main loop
```

**没有 ActorExecutor、没有分片入口车道、没有 actor 邮箱。**

主循环由 `App` 直接驱动；网络事件回投由 `MainLoopInbox` 负责（当前由 `NetModule` 持有），业务模块按需装配。

`MainLoopInbox` 是**网络事件回投器**，不是通用执行器抽象：它只处理连接事件（Connected / Message / Closed）、每连接保序和入口背压。RPC 应答、`Offload` 结果等其它跨线程回投，以后用独立的小队列或 `App::Post`，不引入通用执行器抽象。

---

## 3. 主循环执行

- 一个进程一个 main loop 线程；
- `App::run()` 每帧：
  1. `socket_mgr.wait` / IO 事件；
  2. 处理定时器；
  3. 出队执行消息（`Dispatcher` 投递的）；
  4. 执行模块 `Update`/tick；
- 业务模块直接在主循环上跑，不允许阻塞；
- 阻塞、DB、重计算用 `Offload`（§7）。

### 模块

- 业务模块实现 `IModule`，在 `Init` 注册消息、定时器、实体管理；
- 模块是进程级单例，持有自己的实体管理器；
- 模块之间直接调用 C++ 接口；不要跨进程直接访问状态。

### 线程边界


| 线程               | 干什么                           | 不能干什么               |
| -------------------- | ---------------------------------- | -------------------------- |
| main loop          | 所有业务状态、模块、实体、定时器 | 阻塞、长时间计算         |
| IO 线程            | 收包、解信封、Dispatcher 投递    | 执行业务、碰模块状态     |
| `ThreadPool`       | 阻塞 IO、CPU 计算                | 碰模块/实体/LuaState     |
| Lua worker（可选） | 独立 `LuaRuntime` 执行脚本任务   | 碰 main loop 的 Lua 对象 |

Lua 的 worker 模型只属于 Lua 框架，不在此文的 C++ 执行模型里；C++ 侧只把 Lua 任务当作一种可选的后台任务来源。

---

## 4. Dispatcher

`Dispatcher` 是 `App` 的成员，不是 Module；没有线程、没有生命周期，只是一张登记表 + 路由。

### 登记

- 模块在 `Init` 阶段登记：`消息号 → handler`；
- 同一消息号不能重复登记；重复报错，在 `Init` 阶段暴露；
- `App` 在所有模块 `Init` 结束后、`Start` 之前**冻结**；
- 冻结后只读，IO 线程无锁读；
- 冻结后再登记返回错误。

### 投递

- IO 线程解完信封，按消息号查表；
- 未登记：
  - 请求消息回“无处理者”的明确错误；
  - 单向消息计数并限频记日志，不静默丢；
- 投递只入 main loop 队列，不在 IO 线程执行 handler；
- main loop 出队时再判一次过期/取消。

### 接口草图

```cpp
class Dispatcher {
public:
    Status On(MsgId, Handler);                 // Init 阶段
    DispatchResult Dispatch(const Envelope&, SharedBuffer) noexcept; // IO 线程
    void Freeze();                             // App 调
};
```

不引入 `IExecutor` / `IExecutorProvider` / `ActorExecutor`；网络事件回投由 `MainLoopInbox` 负责，但它是网络专用的具体工具，不是通用执行器抽象。

`Dispatcher` 属于 `MessageModule` 内部实现（消息号登记表 + 冻结 + 投递）；RPC 的消息号由 `RpcModule` 注册，`MessageModule` 只按 `msg_id` 派发。分层见 [RPC与IDL设计.md](RPC与IDL设计.md)。

---

## 5. 模块与生命周期

- `App` 负责装配模块，并直接驱动主循环；
- 网络事件回投由 `NetModule` 持有的 `MainLoopInbox` 负责；
- `AsyncWorkerModule` 是 Lua 框架的模块，不在本文的 C++ 执行模型里；C++ 侧可选的线程池任务用 `ThreadPool`；
- 模块生命周期：`Init` → `Start` → `Stop` → `Shutdown`；
- 模块依赖通过显式装配顺序/依赖声明解决，不靠 actor/kinds；
- 停机时：
  1. 停止接收新消息；
  2. 排空 main loop 队列；
  3. 给模块 `Stop` 存盘；
  4. 关闭线程池/IO；
  5. 退出。

---

## 6. 消息与背压

- 信封类型：`kRequest` / `kResponse` / `kOneway`；
- main loop 队列有上限；满时：
  - 请求回“忙/拒绝”错误；
  - 单向消息计数丢弃；
- 每个连接有独立的入队上限，避免一个连接打满全局队列；
- 消息终态明确：已执行、已拒绝、已过期、未找到处理者；
- main loop 出队时判断 deadline，过期不再执行；
- 同一连接的消息保序。

---

## 7. 异步任务与线程池

`Offload` 用于不阻塞 main loop：

```cpp
mainLoop.Offload(work, on_done);
```

- `work` 在工作线程执行，**不碰模块、实体、LuaState**；
- `on_done` 作为消息回投 main loop，在 main loop 上执行；
- 发起方实体在任务期间被删除时，`on_done` 丢弃（带代次）；
- 底层用 `ThreadPool::Post`（共享队列、`max_pending` 满返回 false、`Stop` 排空）；
- 线程池任务不能直接修改业务状态。

---

## 8. RPC 与 IDL

### 8.1 基本规则

- **RPC 一律异步，没有同步版本**；
- 调用方发起后立即返回 `CallHandle`；
- 被调方处理完调用 `ReplyHandle::Complete(status, payload)`；
- 超时由 main loop 的 `TimerWheel` 驱动；
- 应答和超时竞争时终态唯一：先到的赢，后到的丢弃并计数；
- 分开统计 `late_reply`（应答晚于超时）和 `unknown_seq`（没有对应挂起项）。

### 8.2 调用过程

```text
调用方 main loop：
  ① 分配 seq；挂起表登记；设超时
  ② 投递：同进程 → Dispatcher；跨进程 → 信封 → Agent → 对端 IO 线程

被调方 main loop：
  ③ 出队；判过期；执行 handler
  ④ ReplyHandle::Complete(status, payload)

调用方 main loop：
  ⑤ 应答回到挂起表；执行回调；清超时
```

- 挂起表放在 main loop（不放全局）；
- 信封带 `seq` / `remaining_ms` / `err_code`；
- 被调方出队时再判一次 `remaining_ms`；
- 下游调用默认超时 `min(显式值, 当前请求剩余时间 − 预留)`；
- 框架错误映射为 `kUpstream`，业务错误透传。

### 8.3 `CallHandle` / `ReplyHandle`

- `CallHandle`：发起端持有；不能阻塞取结果；不挂 future/promise；
- `ReplyHandle`：应答端持有；没回过就析构，自动回“无应答”错误；
- 命名不叫 future/promise。

### 8.4 IDL

- 用 gRPC 的 schema 写法（`service` / `rpc`），**只用 schema 语法，不用 gRPC 代码和运行时**；
- 自研生成器生成统一接口；
- `rpc` 签名决定 `Call`（有应答）还是 `Send`（返回 `google.protobuf.Empty`，单向）；
- `stream` 生成器报错；
- 第一版所有 `msg_id` 在 IDL 中显式写，生成器输出常量，不做哈希；
- 消息体 codec 按 service/method 选择：C++ handler 用 C++ protobuf，Lua handler 用 Lua protobuf 库；`RpcModule` 只处理 RPC 头，payload 不透明传递；
- 请求体在处理函数执行时由 handler 按 codec 解码（main loop 线程），不在 IO 线程；
- 解码失败：回框架错误码“解码失败”；单向消息计数 + 限频日志，不断连接；
- 客户端命令与服务间 RPC 用 service 级 `audience` 区分；
- `CLIENT` service 的请求消息不许标 `route_key`。

### 8.5 Target 与寻址

- 调用方一般只给对象 id，不写节点；
- IDL 在 service 级声明 `service_type` / `object_type`，请求消息用 `(route_key)` 标出对象 id 字段；
- `Locator` 按 `(服务类型, 对象类型, id)` 解析节点；
- 同进程调用和跨进程调用用同一个入口；
- 不做 ActorKind；对象类型和实体由 `EntityManager` 管理。

### 8.6 `Status`

- C++ 侧统一 `Status`：框架错误码 + 可选消息；
- 业务错误放 payload；框架错误走 `Status`；
- 超时、路由失败、无处理者、解码失败都有明确错误码。

### 8.7 不做

- 同步 RPC；
- 阻塞等待应答；
- 进程内 actor 分片；
- 在线迁移/自动重平衡。

---

## 9. 线程与队列

### 9.1 线程

- main loop：一个线程，业务串行；
- IO 线程：可多个，只做收发/解码/投递；
- `ThreadPool`：`Offload` 阻塞/计算；
- Lua worker：属于 Lua 框架，各线程独立 `LuaRuntime`，不在本文 C++ 执行模型中。

**没有 Shard 线程，没有 actor 分片。**

### 9.2 队列

- `base/queue` 提供 SPSC/MPSC 无锁队列；
- `PeakApprox` 是**近似下界**：首次与每 1024 次批量出队才用 `size_approx` 采样，其余用取出条数；
  - **别改成每次都采**：它要遍历生产者/全部块，随队列深度增长，会让消费者越慢越积压；
  - 对照实验见本文 §9 / `queue_test` 的 `TestMpscPeak*`、`TestSpscPeak*`；
- 批量出队一次取 256 条左右最快；
- 长寿生产线程要用 `ProducerToken`；
- 队列满时按 §6 背压；不在 IO 线程阻塞等待。

---

## 10. 实体、寻址与进程拓扑

### 10.1 EntityManager（main loop）

- 主循环上的实体由 `EntityManager<T>` 统一管理；
- 业务模块持有 `EntityManager`，不是全局单例；
- 业务用 `Adopt` / `Activate` / `Remove` 管理实体；
- 登记/注销在接口里自动联动对象目录，业务不直接调 `Register`；
- 实体生命周期：创建 → 激活（可异步加载）→ 使用 → 移除；
- 加载期间到达的消息：排队，加载失败回错误给发送方；
- 实体只由 main loop 修改。

### 10.2 进程拓扑

- 单体 / 微服务 / sidecar 是进程间拓扑，不是进程内执行模型；
- 服务类型由配置/IDL 定义；
- 扩容靠多进程/多服务实例，进程内不切 shard；
- 对象寻址：`(服务类型, 对象类型, id)` → `Locator` → 节点；
- `Locator` 的实现与注册中心唯一性保证未定；进程内缓存查询结果。

### 10.3 不做

- 进程内多 Shard；
- Actor kind / actor 实例 / actor 池；
- 在线迁移、自动重平衡、全集群唯一激活目录；
- 同进程多个执行器/多个 ActorSystem。

---

## 11. 可观测与停机

- main loop 队列深度、出队延迟、拒绝数；
- RPC 挂起数、超时数、`late_reply` / `unknown_seq`；
- `Offload` 队列深度、失败数；
- 实体数量、激活耗时、加载失败；
- 服务间路由失败、Agent 不可用；
- 停机时输出未完成 RPC / 未排空队列。

---

## 12. 验证计划

- Dispatcher：重复登记、冻结后登记、未登记消息号、请求/单向不同处理；
- main loop：队列满背压、过期消息、保序；
- RPC：异步调用、超时、late reply、unknown seq、无应答析构、链式预留；
- IDL：Call/Send 生成、stream 报错、解码失败、audience、route_key；
- `Offload`：结果回投、实体删除后丢弃、线程池满拒绝；
- EntityManager：Adopt/Activate/Remove、加载失败回错误、自动登记/注销；
- 停机：排空、存盘、未完成调用统计；
- 队列：PeakApprox 采样、批量出队、MPSC/SPSC 压测。

---

## 13. 明确不做

- Actor 模型、`ActorSystem` / `ActorExecutor`、actor 池、actor kind；
- `MainLoopSystem` / `IExecutor` / `IExecutorProvider` 这层通用执行器抽象；
- 进程内多 Shard、分片线程、哈希分片；
- 在线迁移、自动重平衡、全集群唯一激活；
- 同步 RPC；
- C++ 原生热更；
- 进程内多执行器 / 多 `ActorSystem`。

原 Actor/Shard 设计已备份到 [Actor与线程模型设计-废弃.md](Actor与线程模型设计-废弃.md)，**只作历史，不要参考**。
