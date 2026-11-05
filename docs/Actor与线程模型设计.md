# Actor 与线程模型设计（C++ 侧）

**状态**（2026-11-03）：**设计阶段，没有代码。** §0 列的"已定"是用户明确同意过的；其余是我提出、尚未确认的，文中标"**提议**"，汇总在 §19。
**设计不受现有代码约束**：`framework/app`、`MainLoopExecutor`、`ThreadPool`、信封等大多是半成品，本文从需求出发，哪些沿用、哪些改、哪些重写见 §15。
旧的实验性设计 `Actor模型与RPC设计.md`（偏 Go）已于 2026-11-05 删除，不是事实源，本文不继承它的结论；要查旧内容：`git show fa84592:docs/Actor模型与RPC设计.md`。
**线程安全没有在 tsan 下验证过**（本机跑不了，见 [网络库设计.md](网络库设计.md) §11.2）：S1 以后的多线程部分上真机前不算验证过。
Agent、信封、共享内存通道见 [共享内存IPC设计.md](共享内存IPC设计.md)。

## 0. 结论速查

**已定**（用户明确同意）：

1. **两种执行方式**：主循环直接执行、有状态 actor（§3）。每种对应一个执行器：`MainLoopExecutor`、`ActorExecutor`。**无状态并行不单设执行方式**：用 actor 池（N 个固定键的 actor）加 `Offload`（§3）。原先设计过的第三种"无状态 worker"已取消（用户确认）。
2. **网络入口注入一个 `Dispatcher`，各执行器向它注册**；IO 线程解出消息后直接分发给对应的执行器（§4）。**没有进程级的"模型"开关**：进程里有哪些模型，由 `main` 装配了哪些模块决定，进程只用一种模型是只装配一个的特例。
3. **结构**：`MainLoopSystem`、`ActorSystem` 都是 Module，走 Module 生命周期，各自内部持有对应的执行器。**主循环由 `App` 自动安装、永远存在**（用户选定），`ActorSystem` 按需装配。`App` 持有 `Dispatcher`（成员，不是 Module）；`mainLoop()` 是引用，`actors()` 是非拥有指针（没装配为空）。**新增执行模型 = 实现 `IExecutor`、做成实现 `IExecutorProvider` 的 Module**（用户确认，§4）。
4. **一个 actor 类（kind）一个 Module，在 `Init` 里向 `ActorSystem` 注册；实例由 `ActorSystem` 管，不是 Module。** 多个 actor 模块都注册到同一个 `ActorSystem`。
5. 房间服（休闲多人）与场景（MMO）是不同游戏类型的进程的需求，互不耦合；一个进程里可有多种 kind。
6. **多线程用哈希分片就够了。** kind 的注册结构里预留可选的"分片组"名字，默认大家同一组，需要时给场景单独一组。
7. **借鉴五条**：激活是带状态的异步过程；卡死检测；不存在的实例有明确终态；按 kind 的指标；邮箱出队时再判一次过期。
8. **实例加载失败时，已排队的消息回错误给发送方。**
9. **不做**：在线迁移、自动重平衡、全集群唯一激活的分布式目录。等真的有多进程扩缩容的需求再评估。
10. **设计不受现有半成品代码约束**：能用就用、能优化就优化、不满足就改、必要时重写。项目还没上线，信封、握手、协议都可以直接改，不考虑兼容。
11. **RPC 必须异步**，没有同步版本（§8）。
12. **应答用 `ReplyHandle`**：处理函数没回过就析构，自动回"无应答"错误（§8）。
13. **借鉴 GameserverNew 的几处**（§8、§15）：编排辅助 `Join` / `Each` / `Seq`；接口定义里由 `rpc` 签名决定生成 `Call` 还是 `Send`（原先的 `_Req` / `_Rsp` / `_Send` 命名规则已被第 21 条取代）；`Target` 链式构造（值类型，返回副本）；启动就绪等待。
14. **App 层面的编排（停机阶段等）暂缓**，先由 `main` 手动调（§15、§19 第 1 项）。
15. **命名**：发起端 `CallHandle`，应答端 `ReplyHandle`，应答方法 `Complete`；不叫 future / promise，因为它们不能阻塞取结果，也不挂续接（§8）。
16. **链式调用**：B 的处理函数返回 `void`，应答经 `ReplyHandle` 晚点回；下游调用的默认超时取 `剩余 − 预留`；下游的框架错误映射成 `kUpstream`，业务错误原样透传（§8）。
17. **Shard 入口按 kind 分车道**（`inbox[shard][kind]`，每条自带上限），**不让网络线程直接投进实例邮箱**，实例仍只归所属 Shard 线程（§7、§9）。
18. **一个进程只有一个 `ActorSystem`**，里面可有多个 kind，一个 kind 可实现多个生成的服务接口；不支持同进程多个 `ActorSystem` 实例，隔离靠分片组（§5）。
19. **IDL 生成统一的 RPC 接口，业务通过各执行模型的登记器（`IMethodRegistrar`）绑定**，不是每个模型各有一套登记 API（用户提出的方向，§8）。
20. **客户端命令与服务间 RPC 要区分**（用户确认）；机制是 service 级的 `audience`，见 §8（**提议**）。
21. **接口定义用 gRPC 的 schema 写法**（用户选定，2026-11-05，§8）：proto 里写 `service` / `rpc`，**只用 schema 语法，不用 gRPC 的代码与运行时**，由自研生成器生成。`rpc` 签名决定 `Call`（有应答类型）还是 `Send`（返回 `google.protobuf.Empty`，单向）；`stream` 生成器报错；`CLIENT` service 的请求消息不许标 `route_key`；**第一版所有 `msg_id` 手写**，不做哈希；**单向方法只生成 `Send` 存根，处理函数没有 `ReplyHandle`、不涉及应答类型**。
22. **消息编解码用 libprotobuf**（用户选定，2026-11-05，§8、§19 第 35 项）。**请求体在处理函数执行时解码**（执行器线程，不在 IO 线程）；解码失败：请求回框架错误码"解码失败"，单向消息计数加限频日志，不断连接（用户选定，依据见 §17 的对照）。

**提议**（未确认，见 §19）：统一的 Shard 结构（§9）、`Locator` 接缝（§14）、`ActorContext` 不给 `App` / 模块引用（§5）、显式创建为默认（§6）、本地消息统一用字节（§10）、actor 池的分片函数与选择器（§3、§9）、`Correlation` 结构与回调契约（§8）、`Status` 的形状（§8）、`HandlerContext` 与 IDL 生成器的细节（`audience`、`msg_id`、登记器，§8）、未覆盖需求的清单（§16）等。

## 1. 需求

- **实体**：玩家（数量多、单次处理短）、房间（数量中等）、场景（每帧 tick，单帧耗时可能大，MMO 才有）。
- **一个进程多种 kind**，同一进程里的组合因游戏类型而异；**多种进程类型**（微服务）协作，进程之间经 Agent 路由、用 RPC 互相调用。
- **业务单线程是默认约定**（§10）：actor 内部的代码不应该加锁。多线程只在确有 CPU 压力时打开，且不改业务代码。
- **玩家要从库里加载**，所以激活是异步的；加载期间到达的消息不能丢，也不能处理。
- **同一个 actor 的消息严格串行**；同一连接发给同一个 actor 的消息保序。
- **网络消息进来的路径要短**：IO 线程直接到目标执行器的入口车道，不多绕线程。
- **RPC 要和两种执行方式都配合**：应答必须回到发起调用的那个执行上下文里处理。
- **停机要排空**：所有 actor 有机会存盘。

## 2. 总体结构

三层，互相正交：

| 层 | 回答的问题 | 选项 |
|---|---|---|
| 进程内执行 | 处理函数在哪里跑 | 主循环直接执行 / actor 中执行（§3） |
| 进程间拓扑 | 工作怎么拆成进程 | 单体 / 微服务 / sidecar（§14） |
| 寻址 | 一条消息发给谁 | 节点 / 服务类型 / `(kind, key)` 实体（§14） |

进程内的数据流：

```
网络 IO 线程：解信封
   │
   ▼
Dispatcher（App 成员；登记表：消息号 → 执行器 + 处理函数；Start 前冻结）
   ├─ MainLoopExecutor（MainLoopSystem）：入主循环队列，主循环线程上直接执行
   └─ ActorExecutor  （ActorSystem）   ：入目标分片的入口车道，在 actor 的回合里执行
RPC 应答：不查登记表，按 seq 回到发起方的执行上下文（§8）
```

## 3. 两种执行方式

| | 主循环（直接执行） | 有状态 actor |
|---|---|---|
| 执行器 | `MainLoopExecutor` | `ActorExecutor` |
| 对外的系统 | `MainLoopSystem` | `ActorSystem` |
| 谁向它登记 | 模块 | `ActorKind` |
| 状态 | 模块的全局状态 | 每个实例私有 |
| 串行 | 整个进程串行 | 同一个实例严格串行 |
| 并行 | 无 | 不同实例并行（哈希分片） |
| 线程 | 主循环线程 | 所属分片（S0 = 主循环线程） |
| 能不能碰模块 | 能 | 不能 |
| 上下文 | 模块自己 | `ActorContext`（有 `self()` 和键） |
| 典型用途 | gate、match、online 这类管理服务 | 玩家、房间、场景 |
| 实现阶段 | 现有 `MainLoopExecutor` 演进 | S0 |

### 选择规则

1. **有状态、按实体串行**：actor，哈希分片。
2. **无状态、要并行**：先多起实例，走微服务这一层的 `Any` 路由，每个进程仍然单线程（§14）。只有多个实例会重复占用大块只读数据（策划表、地图）时，进程内并行才值得（这是判断，不是已有需求）。需要时用 actor 池（下面）。
3. **会阻塞或重计算**：`Offload`（下面）。
4. **全局状态、管理类逻辑**：主循环模块，直接执行。

### 无状态并行：actor 池（取代原先的 worker 执行方式）

原先设计过第三种执行方式"无状态 worker"（独立的 `WorkerSystem` / `WorkerExecutor`），**已取消**。无状态并行用一个**普通 `ActorKind`** 表达：

- **池 = 一个 kind，N 个固定键（`0..N-1`）的实例**，启动时显式创建（§6）。`ActorKind::pool_size` 大于 0 表示这是池 kind。成员是普通 actor：同一成员内串行，成员之间并行；处理函数不存状态靠约定，框架不强制（别家同样靠约定）。
- **目标写 `Pool(kind)`**：调用方不选键，由**选择器**挑一个成员，等价于 `Actor(kind, 选出的键)`。网络消息的消息号登记给池 kind 时，`Dispatcher` 同样用选择器挑成员，不读信封的 `route_key`。
- **选择器第一版是轮转**（原子计数，IO 线程、主循环、分片线程都能调）。以后要"最空闲优先"，读各分片入口车道的深度计数即可；但只看排队数看不见"正在跑的慢任务"（Akka.NET 的 `SmallestMailbox` 就只读 `NumberOfMessages`；Orleans 的 `StatelessWorker` 先找"没在执行"的成员，都忙才比等待数）。
- **池只在 S1 起有并行收益**：S0 只有一个分片，池退化为串行。S1 起成员在各分片线程上，IO 线程直接投入分片入口车道，不绕主循环。池 kind 的分片函数见 §9。
- **慢任务和阻塞不要放进池**：成员占着分片线程，会拖住同片的别的 actor。重计算和阻塞走 `Offload`（共享队列，哪个线程空闲谁取，没有分片的队头阻塞）。

**为什么取消**：读过的框架里，"无状态并行"都是普通 actor 加选择器，没有专门的 worker 执行方式。
- protoactor-go 的 `router/`：`PoolRouter.OnStarted` 里 `Spawn` N 个普通 actor；轮转是原子计数取模；路由在发送方线程直接转发（`process.go` 注释：避免路由消息在一个 actor 里串行）。
- Akka.NET 的 `Routing/`：`RoundRobin`、`SmallestMailbox`、`ConsistentHash`、`DefaultResizer`（自动增减池大小，伸缩算法我没读）。
- Orleans 的 `[StatelessWorker]` 是放置策略：同一个 grain 多个激活，默认上限为 CPU 数（`StatelessWorkerPlacement`、`StatelessWorkerGrainContext.ReceiveMessageInternal`）。
- skynet 没有内置，`examples/simpleweb.lua:109-116` 手写轮转。CAF 没读。

我们已有 actor、分片组和 `Offload`，再设一种 worker 只多出一套 `WorkerSystem` / `WorkerContext` 要维护；它独有的好处只剩"调用方不选键"，由选择器补上。"无状态由 API 形状保证"（`WorkerContext` 没有 `self()`）这种强制，别家也没有。

### 不是执行器的两个东西

- **`Offload`**：辅助设施。主循环或 actor 自己发起一次阻塞或重计算：`ctx.Offload(work, on_done)`，`work` 在工作线程里跑、不碰任何上下文，`on_done` 作为消息回投到发起方的上下文里执行。发起方在此期间被回收时 `on_done` 丢弃（带实例代次）。底层用现有的 `ThreadPool::Post`（无 key、共享队列、`max_pending` 满了返回 false、`Stop` 排空），基本够用。
- **RPC 应答的独立通道**：不是执行器，是 `Dispatcher` 对应答类消息的单独路由（§8）。

### 与 silver-server executor 的对照

| silver-server | 这里 |
|---|---|
| `Normal`（proc 循环直接执行） | `MainLoopExecutor` |
| `IdMod` / `TransferExecutor`（按 `roleid % N` 分片） | `ActorExecutor`（哈希分片） |
| `Single`（承载 RPC 应答） | 无对应执行器：应答独立通道（§8） |
| `WorkerPool` / `SelfExecutor`（goroutine 池，无序；`component/agent/API.go` 的默认模式，池大小 4096） | 没有对应的执行器：actor 池加 `Offload`。goroutine 便宜、阻塞不占 OS 线程，C++ 线程不能照搬这个默认值 |

silver-server 的先例值得保留两点：进程级默认执行器之外，**允许按单条消息覆盖**（`RegisterCustomMsg(recvSSResRpc, recvSSResRpcExecutor)`）；**RPC 应答不和请求排同一个队列**。

## 4. Dispatcher 与执行器

### 登记与冻结

- **`Dispatcher` 是 `App` 的成员，不是 Module。** 它没有线程、没有生命周期，是一张登记表加路由逻辑，属于进程骨架。
- **登记者类型决定执行方式**：模块登记的处理函数走主循环；`ActorKind` 登记的走 actor（池 kind 也是 `ActorKind`）。调用方不传第三个参数，不会传错。
- **允许显式指定执行器覆盖默认**（用于特殊消息）。
- **同一个消息号不能被两边登记**，注册时报错，在 `Init` 阶段暴露。
- **登记项带一个对 `Dispatcher` 不透明的字段（cookie）**，只有对应的执行器能解释：主循环不需要，actor 放 `KindId`，actor 池放选择器信息。`Dispatcher` 只负责"消息号到执行器"，不懂各模型自己的路由。
- **`App` 在所有 `Init` 结束之后、`Start` 之前冻结 `Dispatcher`。** 冻结后只读，IO 线程无锁读；冻结之后再登记返回错误。不靠模块依赖顺序，所以不会出现"还有模块在登记、网络已经在收包"的窗口。
- **没登记的消息号**：请求回"无处理者"的明确错误，单向消息计数并限频记日志；不静默丢，也不悄悄落到主循环。

### 接口草图（**不是定稿**）

```cpp
class IExecutor {                               // 新模型要实现的全部，见下面"新增一种执行模型"
public:
    // 请求 / 单向消息。线程安全、非阻塞。**不得在调用线程里、在 Submit 返回之前内联执行处理函数**，
    // 即使目标恰好就在调用方所在的线程：否则调用方持锁、重入、栈深都会出问题。
    // gRPC 的 EventEngine::Run 也是这条规矩（"Implementations must not execute the closure in the calling thread
    // before Run returns"）。
    virtual SubmitResult Submit(DispatchedMessage&&) noexcept = 0;
    // 应答：投给 Correlation::ctx 所属的上下文，优先于请求，线程安全
    virtual SubmitResult SubmitResponse(DispatchedResponse&&) noexcept = 0;
    virtual CtxRange contexts() const = 0;      // 占用的 ctx 号区间：主循环 {0}，actor 每个 Shard 一个
    virtual ExecutorStats stats() const = 0;    // 统一的指标出口（队列深度、排队延迟、各类拒绝计数）
};

class IExecutorProvider {                       // 能力接口，和 ITickable / IWaker 同一个机制
public:
    virtual ExecutorKind kind() const = 0;
    virtual IExecutor& executor() = 0;
};

class Dispatcher {
public:
    ExecutorId AddExecutor(ExecutorKind, IExecutor&);       // Init 阶段（App 在 Build 后自动调）；按 contexts() 分配 ctx 号空间
    Status On(MsgId, ExecutorId, Handler, Cookie = {});     // Init 阶段，重复登记报错；Cookie 对 Dispatcher 不透明
    DispatchResult Dispatch(const Envelope&, SharedBuffer) noexcept;  // IO 线程调用，只读
    // Freeze 由 App 调
};

class App {
public:
    Dispatcher& dispatcher();
    MainLoopSystem& mainLoop() const;   // 永远非空：App 自动安装
    ActorSystem*    actors()   const;   // 非拥有指针；没装配为 nullptr
};
```

### 指针怎么来

- `App` 在 `Build` 阶段已经按能力接口筛选模块（`ITickable`、`IWaker`，裸指针由 `modules_` 拥有，`app.h` 的注释）。沿用同一个机制：两个系统都实现 `IExecutorProvider`，`Build` 时筛出来、登记进 `Dispatcher`。
- `mainLoop()` 是引用，因为主循环由 `App` 自动安装、永远存在；`actors()` 是指针，没装配时为空，`App` 不构造它。两者都只是筛选结果的便捷访问。
- **代价**：`App` 的头文件要前向声明这两个类，将来加第三种执行器要改 `App`。如果要避免，可以只保留 `GetModule<T>` 加按种类取 `IExecutor*`。已选前者，因为这两种是框架核心概念，值得有一等入口。

### `MainLoopSystem`（**已定**：App 自动安装的内建 Module）

- 它是 Module，内部是 `MainLoopExecutor`，实现 `IWaker`（`Wake` 就是 `Drain`）和 `IExecutorProvider`，所以在 `Dispatcher` 眼里和别的执行模型长得一样。名字与 `ActorSystem` 同构，也避免和 `App::RunLoop` 混淆。
- **`App` 自动安装它，`main` 不用也不能漏装**：`Build` 时 `App` 在用户模块之前放进去，所有用户模块**隐式依赖它**，拓扑序里排在最前。`mainLoop()` 因此永远非空，模块在 `Init` 里向它登记不用判空，也不需要 `DependsOn("mainloop")`。备选是显式 `RegisterModule`，会有"忘了装"的失败；或做成 `App` 成员，生命周期上和别的模型不对称。
- **析构顺序**：模块按拓扑逆序析构，它排在最前，所以最后才析构，寿命比所有用户模块都长；未消费的消息直接丢弃，不回调业务（和现在 `MainLoopExecutor` 的析构一致）。
- **要先把网络专用的 `MainLoopExecutor` 拆成两半**（对现有代码的重构，**提议**）：
  - **通用部分**放进这个 Module，不依赖 `base/net`：`Submit` / `SubmitResponse` / `Drain(budget)`、入口限额、统计，以及主循环这个上下文的 RPC 挂起表和 `TimerWheel`。队列用 MPSC，因为生产者不止 IO 线程：S1 的分片线程、`Offload` 的工作线程、Agent 适配器都会投。
  - **网络适配器**留在 `framework/net`：`IConnectionHandler` → 解信封 → `Dispatcher::Dispatch`，每连接和全局的字节预算与 `PauseRead`（据 `Submit` 的入口结果）。`NetModule::Wake` 里调 `Drain` 的那段随之消失。
- **定时器精度**：主循环上的 RPC 超时用 `TimerWheel`，沿用 `App` 的 tick 粒度（默认约 33 ms 一帧，`app_options.h`），"时间轮非空"要算进 `RunLoop` 的 `ticking` 条件；要更精确再改成 `wait_until(min(下次 tick, 时间轮最近到期))`（§19）。

### 新增一种执行模型要做什么（**已定**：做成实现 `IExecutorProvider` 的 Module）

1. **实现 `IExecutor`**：`Submit`（不得内联执行）、`SubmitResponse`、`contexts()`、`stats()`；自己的线程、队列和排空；线程用 `jthread` 并监听 `StopToken`（`~App` 的约束）；在文档里写明处理函数能不能阻塞、会不会拖住别的消息（gRPC `EventEngine` 的 README 也要求实现者说明回调会不会阻塞轮询）。
2. **做成实现 `IExecutorProvider` 的 Module**：`Build` 时 `App` 把它登记进 `Dispatcher`（`AddExecutor`，同时按 `contexts()` 分配 ctx 号空间）。
3. **提供这个模型的登记器**（实现 `IMethodRegistrar`，§8）：业务（IDL 生成的 `Register*`）通过它把消息号绑到这个模型；登记项可以带模型自己的不透明 cookie（actor：`KindId`）。
4. 登记在 `Init`，`Start` 前冻结；之后消息按登记表**自动**派发，应答按 `Correlation::ctx` **自动**找到发起方所在的执行器。

不会自动发生的事：没有任何消息号绑定过来，它收不到消息；一个消息号只能绑一个模型；不支持运行期热插拔；停机排空要它自己提供（App 层的停机阶段暂缓）。

按这个办法，"多线程 worker 分片模型"这类将来的需求是**新增一个 `IExecutor`**，不动核心；现在的分片线程由 S1 的 `ActorExecutor` 提供。

### 网络入口

- IO 线程上放一个适配器（实现 `IConnectionHandler`，和 `AgentRouter` 同一个位置）：解信封，调 `Dispatcher::Dispatch`。
- `NetModule` 增加一个收 `app().dispatcher()` 的入口；`AddAgentClient` 增加一个在 IO 线程直接处理的重载（现在只收 `IMainLoopHandler`）。
- 载荷在 IO 线程上拷成 `SharedBuffer`（回调里的视图只在回调内有效），这是一次拷贝。
- **`Dispatch` 按信封类型分两路**：请求和单向消息查消息号表，得到执行器后 `Submit`；应答不查表，按 `Correlation::ctx` 找到所属执行器、调它的 `SubmitResponse`（§8）。

### 装配

```cpp
// 主循环（MainLoopSystem）由 App 自动安装，不用写
app.RegisterModule(NetModule);        // 入口收 app.dispatcher()
app.RegisterModule(ActorSystem);      // 要 actor（含 actor 池）才装
// 业务模块和 actor kind 在 Init 里登记处理函数
```

### 命名

`Dispatcher`、`IExecutor`（头文件 `executor_interface.h`）、`IExecutorProvider`、`MainLoopSystem` / `MainLoopExecutor`、`ActorSystem` / `ActorExecutor` / `ActorKind` / `ActorContext`。文档里 Executor 统一称"执行器"。

注意 `Dispatcher` 在 Akka 圈里含义相反：Akka 和 protoactor 的 `Dispatcher` 指**线程调度器**（把邮箱放到线程上跑），也就是这里的执行器；我们的 `Dispatcher` 是"按消息号路由到执行器"。保留这个名字是因为 GameserverNew 里就有 `core/dispatcher`，同一个意思。

## 5. 对象模型

```
ActorSystem（Module）
 ├─ kind 表：KindId → ActorKind（Init 阶段注册，Start 时冻结）
 └─ 每个 Shard 一张本地表：(KindId, Key) → 实例
```

- **kind 是静态的**：`PlayerModule` / `RoomModule` / `SceneModule` 各自 `DependsOn("actor")`，在 `Init` 里 `actors->Register(kind)` 拿到这个 kind 的**登记器**（`IMethodRegistrar`），再调 IDL 生成的 `Register*Service(...)`，把服务的方法按消息号绑到这个 kind 上（§8）。`ActorSystem::Start` 冻结 kind 表。
- **实例是动态的**：由 `ActorSystem` 按 kind 的工厂创建，只在所属 Shard 的线程里创建、运行、销毁。
- **地址是 `(KindId, Key)`**，`Key` 是 64 位无符号整数（玩家 id、房间 id、场景 id）。业务代码里不出现节点号。
- **一个进程只有一个 `ActorSystem`，里面可以有多个 kind**（已定）。一个 kind 可以实现多个生成的服务接口（对同一个 kind 的登记器多次调用 `Register*`，消息号全局唯一）；每个 kind 有自己的配置（分片组、tick、邮箱和车道上限、激活策略、`pool_size`）。**不支持同进程多个 `ActorSystem` 实例**：`App` 只有一个入口，要不同的线程配置就用分片组。别的项目：protoactor 的 `NewActorSystem()`、Akka.NET 的 `ActorSystem.Create(name, ...)` 允许创建多个（系统是普通对象，不是全局单例），Orleans 有在一个进程里起多个 silo 的测试宿主（`InProcTestCluster`），主要用于测试和多名字场景。将来要在一个进程里模拟多节点，应该起多个 `App`，不是多个 `ActorSystem`（`App` 是否被进程级单例挡住，没查）。
- **注册时检查冲突**：kind 重名、消息号被两个登记者同时认领，在 `Init` 阶段返回带原因的错误。
- **消息号决定 kind**（**提议**）：一个消息号只属于一个登记者，注册时登记"消息号 → 执行器"。游戏服按命令号派发是常见做法。有了 IDL，这个绑定在登记时由生成的 `Register*` 完成，不再手写消息号列表。备选是在信封里加显式的 kind 字段，见 §19。

### 接口草图（**不是定稿**）

```cpp
struct ActorKind {
    KindId id;
    std::string_view name;
    // 处理哪些消息号不在这里写：由 IDL 生成的 Register*Service 经登记器绑定（§8）
    std::move_only_function<std::unique_ptr<IActor>(Key)> create;
    std::chrono::milliseconds tick_interval{0}; // 0 = 不 tick（场景才设）
    std::string group = "default";              // 分片组，§9
    std::uint32_t pool_size = 0;                // >0：这是 actor 池，成员键 0..pool_size-1，§3
    ActivationPolicy activation = ActivationPolicy::kExplicit; // 提议默认显式创建
    MailboxLimits mailbox;                      // 每实例条数 / 字节上限
    InboxLimits inbox;                          // 本 kind 在每个 Shard 上的入口车道上限，§7
};

class IActor {
public:
    virtual ~IActor() = default;
    virtual void OnActivate(ActorContext&, ActivationHandle) = 0;   // 异步：加载完成后 handle.Complete(status)
    // 业务消息不走 OnMessage：actor 类同时实现 IDL 生成的服务接口，方法签名统一为
    //   void Login(HandlerContext&, const LoginReq&, ReplyHandle<LoginRsp>);   // 单向（返回 Empty）的方法没有第三个参数，§8
    // 需要 actor 特有的能力（self()、定时器、Fail）时，用 OnActivate 里保存下来的 ActorContext。
    virtual void OnTick(ActorContext&, Clock::time_point now, std::chrono::milliseconds dt) {}
    virtual void OnDeactivate(ActorContext&, DeactivationReason, DeactivationHandle) = 0; // 异步：存盘
};

class HandlerContext {                          // 所有执行模型都提供；IDL 生成的方法只依赖它（§8）
public:
    const RequestInfo& request() const;                     // src、trace_id、剩余时间；客户端来源时有 client_role
    void Send(Target, MsgId, SharedBuffer);                 // 位置透明，异步
    CallHandle Call(Target, MsgId, SharedBuffer, Timeout, ReplyFn); // §8；返回值只用于 Cancel，可以忽略
    bool Cancel(CallHandle);                                // 只能在发起方的上下文里调，§8
};

class ActorContext : public HandlerContext {    // 提议：只有这些能力，不给 App / 模块引用
public:
    Key self() const;
    TimerId SetTimer(std::chrono::milliseconds, std::move_only_function<void()>);
    void CancelTimer(TimerId);
    void PostToMain(std::move_only_function<void()>);       // 要用主循环模块（数据库等）时，结果再以消息回来
    void Offload(std::move_only_function<R()> work, std::move_only_function<void(R)> on_done);
    void Fail(Status reason);                               // §13
};
```

`ActorContext` 不暴露 `App` 和模块，是**为将来换执行模型留路**：业务代码从第一天起就不直接调别的模块，将来把某个 kind 切到工作线程时业务不用改。

## 6. 实例生命周期

借鉴 Orleans 的激活状态（`ActivationData` 里的 `Creating → Activating → Valid → Deactivating → Invalid`），我们的状态：

```
Activating ──成功──▶ Active ──回收 / 停机 / Fail──▶ Deactivating ──▶ Gone
    └─失败──▶ Gone
```

- **Activating**：`OnActivate` 在异步加载。邮箱照常接收（受上限），**不投递**。`ActivationHandle` 线程安全，完成时以系统消息投回本 actor 的邮箱，所以加载结果在 actor 自己的回合里生效。
- **激活失败（已定）**：已排队的**请求**逐条回错误响应（错误码"激活失败"）；**单向消息**按信封约定本来就是"可以丢、不通知发送方"，只计数并限频记日志（**提议**，用户未单独表态）。失败后实例回到"不存在"，下一条消息会触发新的激活尝试，**必须有退避**，避免库出问题时每条消息都去打库一次（参数见 §19）。
- **Deactivating**：`OnDeactivate` 在异步存盘。期间到达的消息**拒绝**并回"正在停用"（可重试）（**提议**；备选是排队后重新激活，Orleans 对非 Valid 的激活有单独的处理，我没细读）。
- **创建策略**（**提议**）：默认**显式创建**——消息到了而实例不存在，回"不存在"，不自动建，避免键拼错就凭空造出一个玩家。玩家登录时显式 `Activate`，登出时回收；房间、场景由分配方显式创建和销毁。按 kind 可选 `kOnDemand`（消息到了就建，Orleans 式）。
- **空闲回收**：默认不开，按 kind 可选。参考 Orleans 的 `ActivationCollector`：按"最后访问时间加存活期限"分桶，周期定时器扫描。
- **停机**：App 层的停机阶段暂缓（§19 第 1 项），`ActorSystem` 自己提供排空，由 `main` 在合适的时机调用。排空顺序：先拒绝新消息（回"正在停机"），再对所有实例 `Deactivate`，等存盘完成或到截止时间。

## 7. 消息投递与终态

### `Deliver` / `Submit` 只返回入口级结果

`kOk / kNoSuchKind / kLaneFull / kShuttingDown`。"这个实例不存在、邮箱满了、处理失败"是在 Shard 线程上才知道的，所以是**异步的响应或计数**，不是返回值。

### 背压：两层限额

1. **每条入口车道的上限**（生产者一侧，原子计数）：每个 Shard 对每个 kind 各有一条入口车道（§9），每条自带上限。超限时 `Submit` 返回 `kLaneFull`，**只拒这个 kind**；适配器据此对来源连接 `PauseRead`（沿用 `MainLoopExecutor` 的入口背压思路），不阻塞 IO 线程。
2. **每个实例的邮箱上限**（消费者一侧，在 Shard 线程上从入口车道取出、放进实例邮箱时检查）：超限的请求回"邮箱满"，单向消息丢并计数。**邮箱必须有界，入队必须有结果，不悄悄丢**——只有 Orleans 在配置后明确拒绝并通知发送方，skynet 只告警，protoactor 丢最旧。

**为什么入口按 kind 分车道，不是每个 Shard 一个共用队列**：共用队列只有一个总计数，不知道消息属于哪个 kind。第二层限额只在消息被 Shard 线程取出之后才起作用，保护不到入口；Shard 线程被某个 kind 占住（重的 tick、洪峰）时，所有 kind 发往这个分片的消息都堆在同一个入口里，计数到顶后**别的 kind 也被拒**。分车道之后每个 kind 的积压和上限互相隔离。更重的隔离用分片组（独立的 Shard 线程）。

**队头阻塞**：一条连接上的消息可能发往不同的执行器或不同的实例，一条车道满了就暂停整条连接会连累别的。所以对"按实体"的目的地，满了应该给请求回错误、单向消息丢并计数；只有整体过载时才暂停读连接。

### 终态：每条消息都有明确的结局

| 情况 | 请求 | 单向 |
|---|---|---|
| 消息号没登记 | 回"无处理者" | 计数 + 限频日志 |
| kind 不存在 | `Submit` 返回 `kNoSuchKind` | 同左，计数 |
| 实例不存在（显式创建策略） | 回"不存在" | 计数 + 限频日志 |
| 实例激活失败 | 回"激活失败" | 计数 + 限频日志 |
| 邮箱满 | 回"邮箱满" | 丢并计数 |
| 正在停用 / 停机 | 回"正在停用"/"正在停机" | 计数 |
| 已过期 | **不回**（调用方早已判超时） | 丢并计数 |
| 处理函数失败 | 回"处理失败" | 计数 + 日志 |
| 请求体解码失败（**已定**，依据见 §17） | 回"解码失败"（框架错误码） | 计数 + 限频日志，不记载荷内容 |

错误码见 §8 的 `Status`：框架错误 1..999（沿用 Agent 现有的 1、2：`kRouteErrNoRoute`、`kRouteErrUnavailable`），业务错误 ≥ 1000。

### 过期

信封已有相对剩余毫秒 `remaining_ms`，Agent 路由器转发时判过一次。**邮箱里排久了的消息，出队时再判一次**（用入队时刻扣减）。Orleans 在收到消息时判（`IsExpired`），我们加一次出队判，因为排队时间才是过期的主要来源。

### 保序

同一条连接发给同一个 actor 的消息保序（每个生产者内部有序，`MpscQueue` 的契约）；不同连接之间、不同执行器之间不保序。

## 8. RPC 的配合

### 已有的和缺的

线上格式基本已有：`EnvType` 的 `kRequest` / `kResponse` / `kOneway`、`seq`、`remaining_ms`、`err_code`，Agent 路由器对送不到的请求也会回错误响应（`kRouteErrNoRoute`、`kRouteErrUnavailable`）。**缺的是调用方的挂起表、超时和续接。**

### 设计

1. **挂起表放在调用方的执行上下文里，不放全局**：主循环一份，每个 Shard 一份。silver-server 用全局 `rpcMgr` 加 channel（`recvSSResRpc` 往 channel 里发，等待的协程继续），是因为 Go 协程可以阻塞等；C++ 没有这个条件，也不该用锁。
2. **信封里的 `seq` 换成 `Correlation { seq, ctx, epoch }`（**提议**，见下）。** 应答到达时 IO 线程不查全局表，取 `ctx` 就能路由。信封 `seq` 的注释写"单调递增、永不复用、命名空间是发送方"，要一并改掉。
3. **应答独立且优先，不占请求的邮箱额度。** 给每个上下文的在途调用数设上限，应答的数量就不会超过挂起表上限，应答队列天然有界，不需要丢。silver-server 的 `SingleExecutor` 队列满会丢应答，丢了调用方只能等超时，这个代价可以避免。
4. **超时用该上下文的 `TimerWheel`，应答和超时竞争时终态唯一。** 先到的赢，后到的丢弃并计数，而且**分开计数**：`late_reply`（应答晚于超时）和 `unknown_seq`（没对上）。silver-server 的 `recvSSResRpc` 注释里写了这两种情况原来在日志里分不清。
5. **deadline 沿用信封的 `remaining_ms`**，被调方出队时再判过期（§7）。
6. **主循环模块和 actor 都能发、都能收。** 回调在发起方自己的上下文里执行；被调方只管 `ReplyHandle::Complete`，不关心对方在哪种执行方式里。
7. **统一的 `Call`**：目标是 `Actor(kind, key)`、`Pool(kind)`（§3）或 `Service(service_type, 路由方式)`（§14）。同进程调用和跨进程调用用同一个入口，由 `Locator` 决定本地投递还是走信封加 Agent。

### 异步（**已定**：没有同步版本）

RPC 一律异步。主循环或 Shard 上同步等，等于自己等自己：应答要由这个上下文自己消费，它被阻塞就永远处理不了，只能死等到超时，期间这个上下文上的所有模块或 actor 都停了。"等待时转嵌套事件循环"更糟：栈上留着半成品状态，等待期间别的消息改了同一个对象的状态。GameserverNew 的停服存档代码里有同款教训（`app.go` 的 `saveAndWait` 注释：不能裸等完成回调，否则主循环自锁）。失败路径也异步，见下面的回调契约。

### 一次 Call 的过程

同进程和跨进程是同一条流程，只在第 ② 步和第 ⑤ 步的投递方式不同：

```
调用方上下文 C（主循环 或 某个 Shard）            被调方执行器
① Call(Target, 消息, 回调) → 返回 CallHandle
   分配 Correlation；挂起表登记；C 的 TimerWheel 设超时
② 投递
   同进程：Dispatch → 目标执行器 Submit（只入队，不内联执行）
   跨进程：信封 → Agent 路由 → 对端 IO 线程 → Dispatch
                                               ③ 入主循环队列 / 分片入口车道 → 邮箱
                                                  出队时再判一次过期 → 处理函数
                                               ④ ReplyHandle.Complete(状态, 载荷)
⑤ 应答：Direct 发给请求的 src，Correlation 原样带回
   IO 线程不查消息号表，取 ctx 投到 C 的应答通路（优先于请求）
⑥ C 出队 → 查挂起表 → 回调在 C 里执行（actor 以发起实例的身份）
⑦ 超时：定时器触发 → 回调收到"超时"；之后晚到的应答计数后丢弃
```

调用方与目标的组合只在落点上不同：

| 调用方 → 目标 | 请求落点 | 应答落点 |
|---|---|---|
| 主循环 → 主循环（同进程） | **不走 `Call`，直接调模块接口** | — |
| 主循环 → actor | 目标分片的入口车道 | 主循环的应答通路 |
| actor → 主循环 | 主循环队列 | 发起方所在 Shard 的应答通路 |
| actor → actor | 同片：邮箱；异片：目标片入口车道 | 发起方所在 Shard |
| 跨进程（四种都一样） | 多一段信封、Agent 路由、对端 IO 线程 | 多一段 Agent 回程 |

现有代码里和这个过程相关的几点：
- 信封的 `src` / `dst` 是进程级 `NodeId`，没有"上下文号"字段，所以上下文号放进 `Correlation`（下）。
- Agent 的 `Route` 对所有类型（含应答）都做过期判断：`remaining_ms` 为 0 无效，被丢弃并计入 `invalid_expire`；过期的计入 `stale_drops`。所以正常的应答必须带正数剩余毫秒。Agent 自己代回的错误应答用固定的 `error_reply_ttl_ms`（5000，`agent_router.h`）。
- 送不到的 `Response` 和 `Oneway`，Agent 不再通知。
- `MainLoopExecutor` 现在只有"连接事件"队列（每个 IO 线程一条 SPSC），没有应答专用通路，要新增。

### `Correlation`（**提议**）

```cpp
struct Correlation {          // 取代信封里的 u64 seq：仍是 8 字节，偏移 48 不变
    std::uint32_t seq;        // 每个 (纪元, 上下文) 内单调递增的计数
    std::uint16_t ctx;        // 发起上下文：0 = 主循环，1.. = Shard
    std::uint16_t epoch;      // 发起进程的启动纪元：启动时刻秒数的低 16 位
};
```

- **应答整块回显**（`h.corr = req.corr`），不逐字段抄，新字段不会漏。Agent 不改写它。
- **为什么要纪元**：保留机制下，Rx 环里写给旧进程的应答会交给重启后的新进程（`shm_connection.h` 的 `Establish` 注释："App 重启后恢复时里面就有积压"）。新进程的计数从 0 起，会撞上旧应答的号，把应答喂给错的回调。
- **三种对不上分开计数，不需要墓碑表**：纪元不同计 `stale_epoch`；纪元相同、`seq` 小于当前计数却不在挂起表里，是发出过、已超时或已完成的，计 `late_reply`；`seq` 大于等于当前计数，是从没发过的，计 `unknown_seq`。
- **为什么不是新字段**：信封前 48 字节是路由区，要留给 Agent 的第一个缓存行（`envelope.h`）；`seq` 本来就在区外，这 8 字节够用，不需要扩大信封。位宽是草案：32 位计数在每上下文每秒 100 万次时约 71 分钟回绕，而挂起项最多活几十秒，回绕无害。

### 回调契约（**提议**）

`Call(Target, 消息, 选项, 回调) -> CallHandle`，语义全收在这个原语里：挂起表、超时、纪元与代次校验、在途上限、异步失败、取消。回调的契约：

1. **永远异步完成**：失败（在途数超限、无路由、`Locator` 答"非本地"）也投回自己的应答通路，下一轮执行，不在 `Call` 的栈里调。否则回调会在调用方处理函数执行到一半时重入。GameserverNew 的 `Core.Call` 同样如此（关闭或发送失败都经 `Poster` 投回，`core.go`）。
2. **回调在发起方的上下文里执行**，不需要加锁。
3. **回调被调用，就说明发起实例仍活着、代次相同。** 挂起项记录发起实例 `(kind, key, 代次)`，应答到达或超时触发时核对，对不上就丢弃。每一项都有超时，必然回收，所以惰性清理就够，不建按实例的索引。回调里可以放心用捕获的 `this`。
4. **至多一次**：成功、错误、超时各调一次；发起实例被回收时不调；`Cancel` 之后不调。`Cancel` 在发起方的上下文里同步生效，不存在"取消的同时应答正好来了"的竞争。
5. **必须带超时，没有"无限等"。**

### `ReplyHandle`（**已定**）

- 处理函数从 `OnMessage` 的参数里拿到一个只能移动的 `ReplyHandle`（IDL 生成的单向方法没有这个参数，见 §8）。应答只经这个句柄回，`ActorContext` 上没有 `Reply(request, ...)`。
- `Complete(status, body)` 只生效一次；**析构时还没回过，自动回"无应答"错误。** 这样处理函数忘了回、实例被回收时，调用方不用等到超时。
- **可以跨回合存活**（延迟应答，如等库加载完再回）。它只持有请求头的拷贝和回送通路，不引用实例，也不引用邮箱里的消息缓冲，所以实例回收之后、或在别的线程上析构，自动回错误都还能发出去。回送通路（同进程投应答通路、跨进程发信封）因此必须线程安全。
- **带剩余预算**：回之前核对，扣完处理时间不剩就不回（Agent 对 `remaining_ms` 为 0 的消息按无效丢弃）；应答的 `remaining_ms` 取剩余预算。
- **`Complete` 返回 `void`**，另给 `explicit operator bool`（句柄有效且还欠一个应答）。重复调用：`ASSERT_*`（Debug 下 abort，Release 忽略并按调用点限流记日志），计 `duplicate_reply`。处理函数不碰句柄就返回，句柄析构时自动回"无应答"，计 `no_reply`，忘了回的 bug 就会暴露出来。
- 对照 GameserverNew：`OnceReply`（`rpc/dispatcher.go`）只保证至多回一次，不保证至少回一次；处理函数忘了回，调用方等默认的 120 秒超时。

### `CallHandle`、`CallResult` 与返回值（命名**已定**，其余**提议**）

**`Call` 返回 `CallHandle`**：
- 就是那条挂起项的 `Correlation`，8 字节，可以随便拷贝，不持有任何资源。忽略它没有代价，回调照常到，所以不标 `[[nodiscard]]`。
- 唯一的用途是 `bool Cancel(CallHandle)`，只能在发起方的上下文里调（挂起表是上下文私有、不加锁的），在别的上下文里调是编程错误，断言处理。取消后删除挂起项、取消定时器，回调不再调；对端不会被通知，仍会处理完，靠 deadline 兜底；之后晚到的应答计 `late_reply`。对已完成、已超时或无效的句柄调 `Cancel` 是空操作，返回 `false`。
- 同步失败（无路由、在途满）也返回有效句柄：失败的合成应答走同一个应答通路，所以失败之后再 `Cancel` 的语义一致。
- **为什么保留**：成本几乎为零；将来的协程层需要它（协程帧因实例回收被销毁时，awaiter 的析构要取消挂起的调用，否则应答到来时会去恢复已销毁的协程。这是推断，没有代码验证）。给函数加返回值对忽略返回值的调用点源码兼容，所以去掉也没有后悔成本。
- **不叫 future / promise**：它们不能阻塞取结果，也不挂续接；`future` 这个名字会让人去找 `.get()`。以后协程层升级成真 future，只改 `Call` 的返回类型。

**回调收到 `CallResult { status, body }`**（`ReplyFn` 是 `std::move_only_function<void(CallResult&&)>`）：成功时 `status` 为 0，`body` 是 `SharedBuffer`；失败时 `body` 为空。`status` 装两类错误：框架错误（超时、无路由、不可达、过载、停机、"无应答"，以及 §7 终态表里的"不存在""邮箱满""正在停用""激活失败"）和业务错误（`err_code` 的业务区间）。类型是 `Status`（`base` 里没有现成的 `Status` / `Result`，只有 `framework/app/errors.h` 的 `Error`），见下一小节。

**各处的返回值**：

| 位置 | 返回 | 理由 |
|---|---|---|
| `Call` | `CallHandle` | 见上 |
| `Cancel(handle)` | `bool` | 是否真的取消了 |
| `ReplyHandle::Complete(status, body)` | `void` | 应答发出去就不管了：调用方已死、Agent 的环满了只能计数，处理函数补救不了；返回 `bool` 反而诱导重试和分支 |
| `OnMessage` 处理函数 | `void` | 单一应答路径：要失败就 `Complete(错误)`，不碰句柄就返回，句柄析构自动回"无应答"。GameserverNew 的 `RouteHandler` 返回 `error` 还能调 `reply`，同步错误和异步应答两条路并存，两边都用了就有歧义 |
| `Call` 的回调 | `void` | — |
| `ctx.Send`（单向） | `void` | 单向本来就"可以丢"，失败只计数；以后要给调用方背压提示（像网络层的 `SendResult::Full`）再加返回值，同样源码兼容 |

`Submit` / `Dispatch` 的入口级结果（`kOk` / `kNoSuchKind` / `kLaneFull` / `kShuttingDown`）是框架内部给适配器用的，业务看不到，见 §7。

### `Status`（**提议**）

```cpp
struct Status {                  // 平凡可拷贝；线上就是信封的 err_code（u32）
    std::uint32_t code = 0;      // 0 = 成功；1..999 框架错误；≥ 1000 业务错误
    bool ok() const;  bool is_framework() const;  bool is_business() const;
};
```

- **只有码，没有消息串**：信封的 `err_code` 是 u32，线上没有地方放字符串；诊断靠 `trace_id` 加日志。业务要给细节，约定放进应答的 `body`。gRPC 的 `Status` 是整数码加字符串（`doc/statuscodes.md`），我们不为每个错误在热路径上带串。
- **分区**：框架错误 1..999、业务错误 ≥ 1000，业务码按服务在 proto 里枚举。GameserverNew 的 `errcode.proto` 是同样的分区（"框架级错误码 (0-999)"、"客户端可见业务错误码 (1000+)"）。
- **框架错误**（码值实现时再定；Agent 现有的 `kRouteErrNoRoute = 1`、`kRouteErrUnavailable = 2` 沿用）。和 §7 终态表是同一批情形：

| 名字 | 含义 | 谁产生 |
|---|---|---|
| `kNoRoute`（1） | 路由表里没有目标 | Agent |
| `kUnavailable`（2） | 目标存在但送不到 | Agent |
| `kTimeout` | 超时 | 发起方的 `TimerWheel` |
| `kOverloaded` | 在途数超限、入口车道满、邮箱满（§7 的"邮箱满"） | 发起方或被调方 |
| `kNoHandler` | 消息号没登记 | `Dispatcher` |
| `kNoSuchActor` | kind 或实例不存在（显式创建策略） | `ActorSystem` |
| `kActivationFailed` | 激活失败 | `ActorSystem` |
| `kDeactivating` | 正在停用 | `ActorSystem` |
| `kShuttingDown` | 正在停机 | 各执行器 |
| `kHandlerFailed` | 处理失败（`ctx.Fail` 或处理函数返回失败） | 被调方 |
| `kNoReply` | 无应答 | `ReplyHandle` 析构 |
| `kUpstream` | 下游失败（链式调用时由中间节点映射，见下） | 中间节点 |

- **不抄 gRPC 的 17 个规范码**：它们为通用服务设计，`FAILED_PRECONDITION` / `ABORTED` / `OUT_OF_RANGE` 这些在我们没有对应场景，而且它的 1、2（`CANCELLED`、`UNKNOWN`）与 Agent 现有的 1、2 含义不同。但借鉴它的一条做法：库自己产生的码只是一个小子集，`INVALID_ARGUMENT`、`NOT_FOUND` 等"库从不产生"，所以应用看到就知道是对端业务返回的（`doc/statuscodes.md`）。我们用数值分区做同样的事。
- **能不能重试由调用方决定**（自动重试不做，见附录）。gRPC 的文档也说没有一份对所有应用都适用的可重试码清单。

### 链式调用（A→B→C）（**已定**：预留与错误映射）

B 的处理函数不返回结果（`void`）：它发出对 C 的 `Call` 就结束，把 A 那条请求的 `ReplyHandle` 移进回调里；等 C 的应答到了、回调在 B 的上下文里执行时，再 `Complete` 给 A。A 的 `Call` 在那一刻才收到 `CallResult`。

```
A.Call(B) ───────▶ B 处理函数(reply_A)
                    B.Call(C, 回调{reply_A})，处理函数返回，B 的邮箱继续处理别的消息
                    ─────────▶ C 处理 → C 回应答
                    ◀───────── B 的应答通路 → 回调在 B 的上下文里执行
                    reply_A.Complete(状态, 载荷)
A 收到 CallResult ◀──
```

- **B 回给 A 的内容**：成功时 B 自己组装 body；如果 A 期望的响应类型正好就是 C 的，也可以把 C 的 `SharedBuffer` 原样转回，零拷贝。
- **错误映射**：C 的失败到了 B 的回调里，B 必须显式决定回什么。框架提供默认映射 `MapUpstream(Status)`：框架错误（< 1000）一律映射成 `kUpstream`，业务错误原样透传。这样 A 能分清"B 自己的框架错误"和"下游的错误"（应答里的 `src` 只说是谁回的，不说是谁出的错），具体是哪一跳靠 `trace_id`（自动透传）在日志里查。业务错误是否适合原样透传由 B 判断，不适合就自己映射。
- **超时预留**：处理请求期间发出的 `Call`，默认超时取 `min(显式值, 当前请求剩余时间 − 预留)`。预留量是配置（`deadline_reserve`），**数值要实测，现在没有数据**。没有预留时：A 给 3000ms，B 收到时剩约 2995ms，B 调 C 的超时也是 2995ms，C 慢到极限时 B 的超时和 A 的几乎同时触发，B 来不及回错误，Agent 对已过期的应答直接丢弃（`stale_drops`），A 只看到自己的超时。每多一跳，预留再扣一份。剩余时间不足以覆盖预留时，不再发下游调用，回调直接收到 `kTimeout`（仍然异步，回调契约第 1 条）。
- **B 等 C 期间实例被回收**：回调被丢弃，回调里的 `ReplyHandle` 随之析构，自动给 A 回 `kNoReply`。丢弃是惰性的（C 的应答到达或 B→C 的超时触发时才发生），所以 A 最迟在 B→C 的超时点听到，早于 A 自己的 deadline（有预留）。
- **其他形态**：B→C 是单向消息时，B 不等 C，直接 `Complete`；B 要同时调 C 和 D，用 `Join`，在汇合回调里回；B 只是转发，更适合走 Agent 的服务路由，不要让 B 转调。

### 其他细节（**提议**）

- **deadline 沿调用链**：见上面的"链式调用"，默认取 `min(显式值, 剩余 − 预留)`；`trace_id` 自动透传。GameserverNew 的 `Core.Call` 取 `min(ctx.Remaining(), Target.Deadline)`，没有预留。
- **`Call` 只允许单目标**：`Request` 加 `Broadcast` 会得到多个应答，`Correlation` 相同。信封层现在不拦，在 `Call` 里校验，`Broadcast` 只允许 `Oneway`。
- **同进程**：不过 Agent、不过环；头直接用结构体传，载荷仍是 `SharedBuffer`（§10）。同一个上下文内部的调用不走 `Call`，直接调模块接口。
- **交叉互调（A→B→A）不死锁**：处理函数发出 `Call` 就返回，邮箱继续跑。副作用是等应答期间，发起实例的状态可能被别的消息改动，这是回调模型的固有性质。
- **主循环上下文自己停机**：排空自己的挂起项，回调收到"停机"错误（GameserverNew 的 `Core.Close` 同做法）。actor 实例被回收则直接丢弃它的挂起项，不调回调。

### 编排辅助（**已定**借鉴）

借鉴 GameserverNew 的 `Join2/3/4`（并行汇合几个异构调用）、`Each`（对多个同类目标做同一调用）、`Seq`（顺序执行异步步骤）。它们是 `Call` 之上的薄封装，不进内核。我们的写法与它的差别：

- 回调都在发起方的上下文里，汇合不需要原子计数。
- 步骤同步完成时也投回应答通路，不递归（它的 `Seq` 里同步调 `Next` 会递归加深）。
- 起步和每一步之间核对发起实例是否仍有效；实例被回收后回调被丢弃，链自然终止（回调契约第 3 条）。
- **聚合层要不要自带 deadline、`Join` 要不要快速失败、多个错误怎么汇总**，它的做法是：没有聚合超时，`Join` 等全部返回，`Each` 取第一个错误其余丢弃。我们怎么定见 §19。

### 接口定义与 `Target`（**已定**）

- **接口用 gRPC 的 schema 写法，生成器自研**（用户选定）：proto 里写 `service` 与 `rpc`，**只用它的 schema 语法，不用 gRPC 的代码与运行时**（不跑 `grpc_cpp_plugin`，不用 `cc_generic_services`）。生成器读 `protoc --descriptor_set_out`，和 `tools/configgen` 同一条路（自带 wire 解码器读 descriptor、自定义 option 放 `options.proto` 一类的文件里）。规则：
  - `rpc M(Req) returns (Rsp)` 生成 `Call`；`returns (google.protobuf.Empty)` 生成 `Send`（单向）。proto 没有 void 类型，`Empty` 是惯例。
  - 要"只回成功 / 失败、没有数据"的请求，定义一个空字段的 `Rsp`，**它仍是 `Call`，不是单向**。单向方法只生成 `Send` 存根（只有请求参数，不涉及应答类型），处理函数没有 `ReplyHandle`、没有返回值（用户选定）；不生成 `Call` 存根，调用方想 `Call` 会编译失败，不会静默变成不等应答。
  - `stream`（任一侧）：生成器报错，设计里没有流式。
  - 命名后缀（`_Req` / `_Rsp` / `_Send`）不再决定语义，取代原先的规则；留作 lint 与否待定。GameserverNew 用的是命名后缀（`_Ntf`），我们改用签名。
  - option 编号要避开 `config/schema/options.proto` 的 52001–52107（**提议**：另取一段，如 53xxx；改 option 时还要看 `configgen.py` 的 `OPTION_NUMBERS` 核对）。
  - **生成器本身还没有设计**，何时做、生成什么见 §19 第 24 项。
- **`Target` 是值类型，链式方法返回副本，不改原对象**（GameserverNew 的 `At` / `ByHash` / `Broadcast` / `Timeout` 同款）。草图，不是定稿：

```cpp
Target::Service(service_type).Any();            // 某类服务的任一实例
Target::Service(service_type).Hash(route_key);  // 按键哈希（取模，§14）
Target::Service(service_type).To(node);         // 指定节点（Direct）
Target::Service(service_type).Broadcast();      // 只允许 Oneway
Target::Actor(kind, key);                       // 实体
Target::Pool(kind);                             // actor 池（§3）
target.Timeout(std::chrono::milliseconds{3000}); // 单次调用的超时上限，返回副本
```

### IDL 生成统一的 RPC 接口（**提议**，方向是用户提出的）

目标：业务看到的 RPC 接口只有一套，不是每个执行模型各有一套登记 API。

**IDL 管契约和寻址，不管谁来执行；生成的代码与模型无关，模型只实现一个很小的登记器：**

```cpp
class IMethodRegistrar {                        // 每个执行模型提供一个（模型作者实现，业务不直接碰）
public:
    // 消息号 + 类型擦除的处理函数（已含解码、调用类型化方法、包装 ReplyHandle）。
    // thunk 带一个 impl 参数，由模型在调用时给出：主循环是登记时绑定的模块，actor 是邮箱里取到的那个实例。
    virtual Status Bind(MsgId, MethodThunk) = 0;
};

// IDL 生成（对所有模型都一样）。生成的方法统一签名：
//   void Login(HandlerContext&, const LoginReq&, ReplyHandle<LoginRsp>);   // rpc 有应答类型：Call
//   void Notify(HandlerContext&, const NotifyReq&);                        // rpc 返回 Empty：Send，无 ReplyHandle、无返回值
Status RegisterPlayerService(IMethodRegistrar&, PlayerService* impl_or_null);
```

业务只写一行 `RegisterXxxService(registrar, impl)`，换模型只换传进去的登记器，不用重新生成。主循环的登记器直接绑到 `Dispatcher::On`；`actors->Register(kind)` 返回的是该 kind 的登记器。

- **前例**：GameserverNew 的 `tools/protoc-gen-svcstub` 同样生成"绑定 route、解码、`Reply`、业务方法到 dispatcher"的注册函数。gRPC 用生成的 `Service` 加 `WithAsyncMethod_X` / `WithCallbackMethod_X` 混入按方法选风格，**生成器要为每种风格各产出一份**，加新风格就要改生成器；我们用运行期登记器，只生成一份，多一次间接调用，和入队相比可以忽略。
- **`HandlerContext`**（§5 草图）是所有模型都提供的公共基类：`request()`（src、`trace_id`、剩余时间，客户端来源时还有 `client_role`）、`Call` / `Send` / `Cancel`；`ActorContext` 派生它，再加 `self()`、定时器、`Offload`、`PostToMain`、`Fail`。**显式参数，不用线程局部的"当前上下文"**：回调回到发起方上下文时要自己恢复隐式上下文，测试要搭环境，tsan 和 asan 也难查。
  - 别家：protoactor 的 `Context` 由小接口组合，非 actor 的 `RootContext` 和 actor 的 `Context` 共用 `SenderContext`；gRPC 有公共基类 `ServerContextBase`，同步和回调风格各派生一个，生成的方法按风格收不同类型（所以生成器要知道有几种风格）；Akka 的 `Context` 和 Orleans 的 `RuntimeContext.Current` 是隐式的。
  - 生成的方法通用的是**签名**，不是实现里依赖什么：想在几种模型间搬来搬去的实现，要遵守最严的规则（比如不碰模块）。

**IDL 里放什么、不放什么**：

| 放 | 不放 |
|---|---|
| 消息与 `service` / `rpc`，`rpc` 签名决定 `Call` 还是 `Send`（返回 `Empty` 是 `Send`） | 在哪个执行模型上跑 |
| 寻址：属于哪类微服务（`Target::Service`）；按实体寻址时用哪个字段当实体键 | 线程、分片、邮箱等承载配置 |
| `audience` 和消息号（见下） | |

**消息号（`msg_id`）**：
- **线上是数字，名字用在 IDL、日志、指标、工具里。** 字符串不上线路：信封路由区"定长、固定偏移、纯数字、没有字符串"是设计前提（`envelope.h`），每条多约 27 字节，不可信输入还多出一块变长解析面。查表本身的 CPU 开销不是主要理由（热缓存微基准，名字约 27 字节，登记 200 到 20000 项：整数键 `FlatHashMap<u32>` 1.3 到 1.8 ns，字符串键 16 到 34 ns，线上带字符串再哈希成 u32 约 6 到 13 ns）。
- **第一版每个 `rpc` 都手写 `(msg_id)`**（用户选定）：缺省、为 0、两个 `rpc` 重复都生成失败，**检查范围是所有 proto 一起**，所以生成器一次要读全部 RPC proto。不做哈希。
- **以后要自动分配**：对没写 `(msg_id)` 的取稳定哈希（`base/hash` 的 `MurmurHash64A` 取低 32 位，哈希只在生成器里算，生成的是常量，Go 侧不用实现同一个哈希），已手写的不变，所以是向后兼容的增量。这等于一个不需要握手的"名字到数字"字典（pitaya 的做法是路由字符串加握手下发字典，`SetDictionary`）。
- 生成表同时带名字，登记时可以按名字查 id，日志和 `MessageStats`（已支持按 id / 名字统计）显示名字。按名字的通用入口（给 GM 工具、压测机器人用，gRPC 有 generic service）需要时再生成 `ResolveMethod(name) → id`。
- GameserverNew 的后端 RPC 用 route 字符串（`Header.Route`），`cmd_id` 只在客户端协议里，手写在客户端消息定义的选项上（`proto/cs/cs_item.proto`），网关把 cmdID 翻译成 route 字符串。

**客户端命令与服务间 RPC 要区分（用户确认），机制是 service 级的 `audience`（提议）。** 形状相同（`rpc` 签名、生成的注册与分发、`HandlerContext`、执行器都共用），区分的理由是四件事：**信任边界**（客户端是不可信输入，不能触达内部方法）、**身份**（客户端命令要玩家 id）、**错误可见性**（`kUpstream` 等不能泄漏给客户端）、**推送**（服务端到客户端的单向 `Send` 目标是客户端连接）。

```
audience = INTERNAL      // 默认：只给内部服务调用，客户端不可达
audience = CLIENT        // 客户端可调：登记处理函数，进网关白名单
audience = CLIENT_PUSH   // 服务端推给客户端：只生成发送端存根，后端没有处理函数
```

- **用 service 级，不用方法级**：service 是承载和评审的单位，不会在一个内部 service 里意外暴露某个方法；GameserverNew 的 `kind` 也是 service 级。要让客户端调某个内部能力，写一个薄的 `CLIENT` service 去显式调内部那个，这一层还能放客户端专有的校验。
- **强制点三处**：①网关用生成的白名单，只转发 `CLIENT` service 的消息号（GameserverNew 的网关也只转发路由表里登记过的 `cmdID`，其余回 `ERR_NO_ROUTE`）；②后端对带"客户端来源"flag 的信封，只允许命中 `CLIENT` service 的消息号，其余回 `kNoHandler`（不暴露方法存在），这个 flag 只有网关会设，客户端协议不是我们的信封，客户端设不了；③网关出口把框架错误（< 1000）统一映射成通用客户端错误码，业务错误透传。
- **`CLIENT` 的消息号必须显式 `(msg_id)`**（生成器没写就报错）：客户端是已发布的二进制，id 要稳定，改名不能让它变。第一版所有 service 都手写，见上。
- **身份**：`request().client_role()` 只对客户端来源有值。**实体键必须来自已认证的会话，不能取自客户端消息体**，否则客户端能直接寻址别的玩家的 actor。GameserverNew 的网关也是从会话里取角色 id（header 的 `ClientRoleID`）。所以 **`CLIENT` service 的请求消息里不许标 `route_key`**（生成器报错，用户确认）：actor 的键由网关从会话取出、写进信封的 `route_key`（**提议**），不来自消息体。
- **网关路由表**（用户：后续在 gatesvr 做）：`cmd_id` → 后端服务类型，再经 `Locator` 定位到该类型服务的具体节点（§14）。上面强制点①的白名单就是这张表的一部分。表从哪来（由 proto 里的服务类型生成，还是配置）没定，见 §19 第 34 项。
- **客户端协议自己的请求关联**（cmd 加客户端 `seq`）由网关翻译，不进内部 `Correlation`：GameserverNew 的 `forwardToBackend` 把客户端 `SeqID` 留在闭包里，用 `RspCmdID` 回给客户端。
- **推送**：`Target::Client(role)` 要经 Agent 路由到持有该会话的网关；"会话到网关"的映射没设计（§19）。

### 回调还是协程

行业分两派：写起来像同步代码（skynet 的 Lua 协程，`skynet.lua` 里 `session_id_coroutine` 把应答对到挂起的协程；Orleans 的 `async/await`；silver-server 的协程阻塞 channel），或回调、future（CAF 的 `request(dest, timeout)` 加 `.then`；protoactor 的 `NewFuture(系统, 超时)`）。

**提议**：第一版用回调（`Call(..., on_reply)`），接口设计成以后能包一层协程。理由：协程跨 `co_await` 挂起时，actor 被回收（`Deactivating`）要能取消它，这块容易出生命周期问题。

### 要避开的坑

- 主循环模块同步阻塞等 RPC：卡住整个主循环。必须异步。
- 应答和请求排同一个队列：慢请求堵住应答，调用方集体超时，容易雪崩。
- 全局挂起表加多线程：要锁，也不知道应答该回哪个线程。

## 9. Shard 与执行细节

### 统一的 Shard 结构（**提议**）

不为 S0 和 S1 做两套机制，用同一个结构：

```
Shard
 ├─ 入口车道：每个 kind 一条，多生产者单消费者（MpscQueue，每个生产者内部有序），每条自带上限（§7）
 ├─ 本地表：(KindId, Key) → 实例（只有本 Shard 的线程碰，不用锁）
 ├─ 每个实例一个邮箱：普通队列（只有本 Shard 的线程碰，不用原子量）
 ├─ 就绪队列：有消息的实例
 ├─ 挂起 RPC 表（§8）
 ├─ TimerWheel：本 Shard 独占（TimerWheel 非线程安全）
 └─ 看门狗计数（§12）

Run(budget)：先处理应答，再轮转着从各条入口车道批量取出消息、分发进各实例邮箱；
             按就绪队列轮转，每个实例每轮最多处理 N 条；然后推进定时器。
```

**跨线程的只有入口车道。** IO 线程定位车道只用两样只读的东西：冻结后的"消息号 → kind"表，和 `hash(kind, key) % N`，不碰本地表（实例只在所属 Shard 线程上创建和销毁）。 实例的邮箱和本地表都是 Shard 私有，所以不需要 protoactor / skynet 那样的"已调度"原子标志：串行是 Shard 单线程保证的。

### 入口层与直接投递（实测）

问题：网络线程能不能不经过 Shard 的入口层、直接投进实例邮箱（skynet、protoactor 的做法）？**结论：保留入口层，不直接投递。**

- **直接投递靠三样东西**：全局的地址表（skynet：读写锁加引用计数；protoactor：1024 个分片的并发 map，加 `PID` 里原子缓存的进程指针和 `dead` 标志）；每个 actor 一个线程安全的邮箱（skynet：自旋锁加环形数组，初始约 1.6 KB；protoactor：Vyukov 节点式无锁队列）；"已调度"标志（skynet 的 `in_global` 加全局自旋锁队列；protoactor 的 CAS `schedulerStatus`）。
- **入口层多出的一跳**只是移动一个小结构（头加 `SharedBuffer` 句柄），不拷贝载荷：单线程微基准约 8.5 ns/条（5.8 → 14.3 ns）。
- **跨线程对比**（1 个 Shard 线程，1000 个 actor，处理函数只做 `state++`，Release，每条消息的总耗时，5 次取中位数）：

| 生产线程 | 现设计（共享入口加私有邮箱） | 直接投递，skynet 式（读锁查表、引用计数、自旋锁邮箱、已调度标志） | 直接投递，乐观下限（生产者直接持指针） |
|---|---|---|---|
| 1 | 15.0 ns | 75.0 ns | 31.5 ns |
| 2 | 13.9 ns | 51.3 ns | 19.6 ns |
| 4 | 14.0 ns | 52.1 ns | 11.3 ns |

  64 和 10000 个 actor 的趋势相同（那两组是在没加 `NDEBUG` 的构建下测的）。乐观下限只有 4 个生产线程时才比现设计快（约 20%），而它忽略了实例回收的安全性，生产环境做不到。
- **保留入口层的理由**：C++ 没有 GC，直接投递要自己做引用计数或 hazard pointer；并发面扩大（地址表、邮箱、调度标志、实例状态机），本机验证不了 tsan；省的只有几纳秒，实现不好反而更慢。
- **局限**：饱和压测；处理函数近乎为空；只有 1 个 Shard；不含延迟；没试节点式无锁邮箱；不含激活和配额。

**`MpscQueue` 实测**（Release，16 字节消息，i9-14900KF 的 WSL2，32 个逻辑核，不绑核，单次测量，数字是百万条/秒；用的是修了峰值采样之后的代码，见下）：

| 生产线程数 | 1 | 2 | 4 | 8 | 16 | 31 |
|---|---|---|---|---|---|---|
| `MpscQueue`（带 token） | 34.9 | 260.1 | 470.0 | 563.0 | 638.6 | 495.8 |
| `MpscQueue`（隐式生产者） | 34.3 | 156.8 | 259.4 | 352.8 | 463.8 | 407.0 |
| 每生产者一条 `SpscQueue`（`MainLoopExecutor` 的做法） | 39.7 | 106.2 | 658.4 | 493.0 | 609.2 | 610.4 |
| 自旋锁加 vector（整块换走） | 30.2 | 24.3 | 19.8 | 14.4 | 10.7 | 8.2 |
| mutex 加 deque（整块换走） | 47.4 | 22.0 | 16.9 | 13.3 | 10.1 | 14.8 |
| Vyukov 节点队列（每条 new） | 10.8 | 16.0 | 17.6 | 16.2 | 16.5 | 18.4 |

P=2 的波动很大（几次运行里 `MpscQueue` 带 token 测到 141 到 265），表里是最后一轮，不要读成精确值。

- **比简单实现快一个数量级**：生产线程多了，锁版本和每条分配的版本下降或持平，`MpscQueue` 带 token 在 4 到 16 个生产线程时是 470 到 639。
- **长寿的生产线程要用 `ProducerToken`**：P=4 时 470 对 259。
- **批量取出的大小影响很大**（4 个生产线程，带 token）：批量 1 → 57.1，4 → 129.8，16 → 210.9，64 → 358.1，256 → 470.7，1024 → 360.4，4096 → 346.9（上一轮 1024 测到 442、4096 测到 281，波动大）。**Shard 线程一次取 256 条左右**，取 1 条慢约 8 倍，超过 256 不再变好。
- **每生产者一条 `SpscQueue` 没有显著更快**：生产线程 ≥ 4 时和 `MpscQueue` 同量级，P=1 相当。
- **单条延迟**（1 个生产者定速 10 万条/秒，消费者自旋）：p50 带 token 170 ns、隐式生产者 177 ns、每生产者一条 `SpscQueue` 118 ns、自旋锁 204 ns、Vyukov 160 ns；p99 在 14 到 33 µs，p99.9 在 80 到 146 µs，max 在 0.3 到 1 ms。尾部是未绑核加 WSL2 的调度抖动，各实现的差别在噪声里，不能据此排名。
- **消费者停摆时的内存**：每条 16 字节载荷约占 19.3 字节（节点式的 Vyukov 是 32 字节）。队列无界，要上限的调用方自己计数。
- **不能拿它当每个实例的邮箱**：每个队列最小约 3.4 KB，4 个生产线程各投过之后约 11.9 KB（默认构造约 61 KB），普通 deque 邮箱约 0.7 KB。

**已修复的问题：峰值采样拖慢批量取出。** `MpscQueue::TryDequeueBulk` 和 `SpscQueue::TryDequeueBulk` 原来每次调用都先 `SamplePeak()`，里面调 `size_approx()`：`ConcurrentQueue` 的要遍历所有生产者、读各自的 `tailIndex`；`ReaderWriterQueue` 的要遍历全部块，**复杂度随队列深度增长**。修复前的对照实验（同样的队列，去掉采样，5 次中位数，百万条/秒）：

| | 修复前（带采样） | 原生（无采样） |
|---|---|---|
| `MpscQueue`，P=4 / 16 / 31 | 452 / 599 / 328 | 483 / 644 / 521 |
| `SpscQueue` 每生产者一条车道，P=4 / 8 | 4 / 6 | 494 / 687 |

`SpscQueue` 会在队列变深时**越慢越积压**：消费者落后，队列变深，每次取出要遍历的块更多，更慢（修复前 P=2 时 7 次里 4 次只有 2 到 4 Mops，其余从 20 到 194，是同一个机制）。

- **修法**（已改，`base/queue/{mpsc,spsc}_queue.h`）：`PeakApprox` 改成**近似下界**：首次、此后每 `kPeakSampleEvery`（1024）次调用才用 `size_approx` 精确采样一次，其余调用用"本次取出的条数"更新（零开销的下界）。`kPeakSampleEvery` 必须是 2 的幂（`static_assert`）。短于采样间隔的积压可能被低估，`MainLoopExecutor` 的 `queue_peak` 因此也是下界。
- **间隔怎么选的**（实测，8 条车道、**积压无界**：生产者不限速，每条车道积压到上百万条，占不采样吞吐的比例）：间隔 64 约 31%，256 约 62%，1024 约 73%，4096 约 86%。
- **修复后**（同样的对照，5 次中位数）：`MpscQueue` P=4 / 16 / 31 是 485 / 649 / 502，原生是 472 / 644 / 523，持平；`SpscQueue` P=4 / 8 是 449 / 636，原生是 469 / 835。
- **`SpscQueue` P=8 的残余差距只在积压无界时出现**（隔离实验，P=8，每轮交替 15 轮，中位数，百万条/秒；每条车道积压上限由生产者自旋限制）：

  | 每车道积压上限 | 原生 | 现状（间隔 1024） | 间隔 64 |
  |---|---|---|---|
  | 无界 | 746 | 574 | 246 |
  | 65536 | 860 | 760 | 730 |
  | 4096 | 745 | 760 | 756 |
  | 1024 | 649 | 666 | 689 |

  - **机制**：`ReaderWriterQueue` 的块最大 512 个元素（`MAX_BLOCK_SIZE`），`size_approx` 顺着 `next` 指针逐块走，每块一次缓存未命中；积压 150 万条约 3000 个块，一次采样几百微秒。**一次采样的代价与深度成正比，而间隔按调用次数算**，所以积压越深开销越大。
  - **排除了的原因**：只去掉 `size_approx`（保留下界更新）就回到原生（735 对 730）；把 `peak_` / `sample_tick_` 隔到独立缓存行没有改善（550 对 589，不是伪共享）；把采样慢路径挪成 `noinline` 没有改善（615 对 621，不是代码生成）。
  - **结论**：积压有界时（§7 的车道上限、`MainLoopExecutor` 的字节预算）没有差距，连间隔 64 也持平；无界积压只出现在不限速的压测里。没做的改进：让间隔随上次采到的深度变大。
  - **局限**：单机、WSL2 未绑核，各格最小到最大跨度很宽（如 426 到 899），"持平"指中位数在噪声内；上限 65536 那一格差约 12%，范围有重叠，不能说清有没有差距。
- **测试**：`queue_test` 新增 `TestSpscPeakSampling` / `TestSpscPeakLowerBound` / `TestMpscPeakSampling` / `TestMpscPeakLowerBound`，在 debug、asan、release 下通过；7 个变异（去掉下界、每次都采样、首次不采样、间隔不是 2 的幂等）全部被杀死。
- **曾经未解释的现象（现已解释）**：把 `SpscQueue` 预分配 400 万个元素后只有约 1 Mops/s（修复前）。预分配是事先建好并串成环的约 8200 个块，`size_approx` 不管装了多少都要走完整个环，每次出队约 0.3 到 0.8 ms，与 1 Mops 吻合；修复后同一压测 P=1 / 2 / 4 / 8 的中位数是 224 / 325 / 422 / 309。**注意**：这次没有重跑修复前的版本做对照，"约 1 Mops"是当时的记录，解释依据是机制与耗时估算加修复前后的变化。

### S0：一个 Shard 跑在主循环上（默认）

- `ActorSystem` 同时实现 `IWaker`（`Wake` 就是 `Run(budget)`）和 `ITickable`（推进定时器）。IO 线程 `Submit` 之后调 `App::Notify`。
- 不起新线程，"业务单线程、无锁"保持不变。
- 单轮预算由模块自定（`IWaker` 的约定），返回 `true` 表示还有积压。
- 这和"主循环直接执行"不同：每个 actor 有自己的邮箱，按预算轮转，一个 actor 刷屏不会饿死别人；直接执行则是到了就执行。这相当于 protoactor 的 `synchronizedDispatcher`（`Schedule(fn) { fn() }`，和 `goroutineDispatcher` 共用一套 actor 接口，只换调度器），以及 Akka.NET 的 `CurrentSynchronizationContextDispatcher`（注释写"在当前同步上下文上分发消息，例如 WinForms 或 WPF 的 GUI 线程"）。

### S1：N 个 Shard 各占一个线程（哈希分片）

- **路由**：`shard = MurmurHash64A(KindId, Key) % N`。`MurmurHash64A`（`base/hash`）结果稳定，`std::hash` 不保证。
- **N 是进程级配置，运行期不改**：改了同一个键会落到另一个线程，串行就破了，所以改 N 要重启进程。
- **没有迁移、没有自动重平衡**：实例终生在一个 Shard，某片过热时只能靠分片组把重的 kind 隔开。
- **分片组**：每个 kind 注册时指定组名（默认 `"default"`），每个组有自己的一组 Shard 线程。MMO 进程里把场景放进独立的组，场景每帧几毫秒的 tick 就不会堵住同片的玩家消息。现在只预留字段，同片互相拖慢的现象有数据再启用。
- **池 kind 的分片函数**（**提议**）：普通 kind 用哈希；池 kind（`pool_size > 0`，§3）用 `shard = key % N`，`N` 是该 kind 所属分片组的分片数，让成员 `0..pool_size-1` 铺到不同分片。哈希可能把几个成员撞进同一个分片，池的并行度就白费；池大于分片数时各片均匀叠放。
- **线程用 `jthread` 并监听 `StopToken`**（`App` 析构的约束）。

### 什么时候从 S0 到 S1

先不拍数字，要实测：**所有场景一帧耗时之和**，对比主循环能容忍的延迟。判据是主循环的 tick 间隔和消息延迟要求，由真实的场景规模决定。玩家和房间默认长期留在 S0。

### S2：专用分片循环

S1 的线程循环怎么写有两个选择（复用 `EventLoop` 的唤醒与定时器，或单独写），先用能工作的，实测到瓶颈再换，见 §19。

## 10. 多线程下的规则（S1 起）

- **业务单线程是默认约定。** 打开分片会打破它：模块不是线程安全的。
- **actor 里不能直接碰模块**（所以上下文不给 `App` 引用）。要用主循环上的模块，发 `PostToMain`，结果以消息回来。
- **actor 之间只用异步消息，不能同步等**（会死锁）。请求的回调作为消息投回调用方的邮箱。
- **消息不可变或移交所有权**：`SharedBuffer` 是引用计数的不可变块，天然合适。
- **本地消息统一用字节**（**提议**）：同进程 actor 之间发消息也用 `SharedBuffer`，换来位置透明（将来 actor 换进程不改业务代码），代价是本地多一次编码解码。玩家和房间在同进程里高频互发时这可能是热路径，**有数据再加同进程的类型化快路径**。
- **共享的可变对象要排查**：例如 `VarType` 的 const 方法也会写缓存，不能多线程读同一个对象；`MessageStats` 一线程一实例。同一个 actor 内部使用没问题。
- **可以在工作线程里用的**：`Now`（全是原子量）、日志（已在 IO 线程上使用）、`SharedBuffer`。

## 11. 定时器与 tick

- **主循环模块的 tick**：`ITickable::Tick`，由 `App` 的主循环直接调用（"先 tick、后 wake"），和"直接执行"是同一个线程、同一个回合。
- **actor 的定时器和 tick**：每个 Shard 一个 `TimerWheel`，用单调时钟 `Now::SteadyMs()` 驱动，S0 由 `ITickable::Tick` 推进，S1 由 Shard 线程自己推进。实例的定时器回调在该实例自己的回合里执行，和消息串行（Orleans 的 `GrainTimer` 内部也是构造一条消息，`GrainTimer.cs` 里设置 `IsAlwaysInterleave`）。
- **场景的每帧 tick**：kind 设了 `tick_interval`，`OnTick(now, dt)` 由 Shard 按间隔调用。这是主循环 tick 模型和 actor 模型的交汇点。
- **单个实例 tick 耗时要有预算**，超了要告警（§12）。场景耗时是最容易撑爆主循环的，这也是它最先可能需要 S1 的原因。

## 12. 可观测与卡死检测

### 按 kind 的指标（不按实例，实例太多）

邮箱深度峰值、排队延迟（入队到开始处理）、处理耗时直方图、各类拒绝和丢弃的计数（§7 表）、激活数、激活失败数、RPC 的 `late_reply` / `unknown_seq` / `stale_epoch` / `duplicate_reply` / `no_reply`。排队延迟用入队时刻，和共享内存环里的 `queue_delay_ns` 同一个思路。

### 卡死检测

- **skynet 的做法**：每个工作线程有一个版本计数器，处理每条消息前后各更新一次并记下"谁发给谁"；监控线程每 5 秒看一遍，计数器没变而且当前在处理某个服务，就标成 endless 并记日志，不杀。
- **Orleans 的做法**：处理一条请求超过 `RequestProcessingWarningTime`（默认 20 秒）生成诊断；判定卡死后把激活从目录注销，让后续消息建新实例，旧的悬在那里。
- **我们的做法**（**提议**）：每个 Shard 一个"版本号、当前 kind 与键、当前消息号、开始时间"，加一个低频的监控线程，发现版本没变就记日志（带 kind、键、消息号）并计数。**C++ 的处理函数无法被取消**，所以只能告警；是否同时让该 Shard 拒绝新消息（回"过载"），见 §19。**S0 的主循环有同样的风险，同一套机制覆盖。**

## 13. 失败处理

**不做监督树、不做父子关系。** 监督（protoactor 的 `SupervisorStrategy`、Akka）依赖"处理函数出错能被捕获并隔离在 actor 边界内"，我们的库不抛异常，一个处理函数真崩了（空指针、越界）就是整个进程崩，没有可捕获的边界。我们的 actor 是 `(kind, key)` 平级寻址，互相不拥有，"房间包含玩家"是业务里按键引用，不是创建关系。代价是**没有自动级联**：房间销毁时不会自动通知玩家，要业务显式发消息。

保留简化版，思路与 Orleans 一致（`InsideRuntimeClient.Invoke` 把异常变成 `Response.FromException` 返回给调用方，grain 仍然活着；运行时只做"按需重建"，要不要重试由调用方决定）：

- **处理函数返回失败**：请求回"处理失败"，该 actor 继续。
- **actor 不变量被破坏（`ctx.Fail(reason)`，或防御式断言触发）**：该**实例**标记失败，进入 Deactivating，`OnDeactivate` 收到原因 `kFailed`，由它决定是否存盘（存盘可能把坏状态写进库，**默认不存**，见 §19），随后回收；同片其他实例不受影响。玩家下次登录重新从库里加载。这等价于"重启"，只是没有树。
- **进程级崩溃**：和现在一样（`ASSERT_*` Debug 下 abort），不在 actor 层处理。

## 14. 进程间拓扑与寻址

### 微服务是另一层，不是第三种执行方式

微服务回答的是"工作怎么拆成多个进程、进程之间怎么互相调用"，和"处理函数在进程里哪里执行"正交，两者叠在一起用。每个微服务（进程类型）按自己的性质选进程内的执行方式。

**示意**（不是已定的划分）：

| 进程 | 进程内执行方式 | 理由 |
|---|---|---|
| gate | 主循环 | 连接多、逻辑少、主要是转发 |
| 玩家所在的服务 | actor（玩家） | 每个玩家有私有状态 |
| match、online 这类管理服务 | 主循环 | 全局视图，单线程最简单 |
| 房间服 | actor（房间） | 实体多，互相独立 |
| MMO 场景服 | actor 加 tick，场景单独分片组 | 每帧计算重 |

本仓库规划的服务（`CLAUDE.md`，目录现在是空的）：`gatesvr`、`lobbysvr`、`loginsvr`、`matchsvr`、`namesvr`、`onlinesvr`、`routesvr`、`routeagent`、`battlesvr`、`dbsvr`。GameserverNew 是 `gatesvr`、`lobbysvr`、`matchsvr`、`onlinesvr`、`roomsvr`、`routeragent`。silver-server 有几十个 `*svr`，服务之间走 NATS（`component/natsagent`、`natsmq`）。

### 寻址三级

`Call` 的目标是 `Actor(kind, key)`、`Pool(kind)`（§3）或 `Service(service_type, 路由方式)`：

- **`Service(service_type, 路由)`**：微服务这一层。Agent 路由已经支持：信封的 `service_type` 配合 `Direct` / `Any` / `Hash` / `Broadcast`（`envelope.h`、`route_table.h`），含义是"某类服务的某个实例"。Agent 本身就是 sidecar。
- **`Actor(kind, key)`**：实体这一层。先由 `Locator` 查位置得到节点，再发 `Direct`，信封填 `dst = 节点`、`route_key = 实体键`，到目标进程后进 `Dispatcher`。
- **`Pool(kind)`**：actor 池（§3）。选择器挑出一个成员后等价于 `Actor(kind, 选出的键)`，同样先过 `Locator`。
- 接收方不区分来源：消息到了目标进程，进同一个 `Dispatcher`、同一套处理函数和 RPC 应答路径，不管调用方是本进程的 actor、本机另一个进程还是别的机器。

### `Locator` 接缝（**提议**）

- `ActorSystem` 里留一个 `IActorLocator` 接口，返回"本地 / 远端节点 / 未知"。S0、S1 恒答"本地"。非本地时 `Submit` 返回明确错误，这也是"不存在的实例有明确终态"的一种。
- `ActorContext::Send` / `Call` 位置透明，业务代码里不出现节点号。
- **跨进程的显式分配**由独立的服务（规划里的 `matchsvr`、`onlinesvr`、`routesvr` 等，我按名字推测职责，目录现在是空的）按负载挑进程。框架只导出 §12 的按 kind 指标。

### 不做

在线迁移、自动重平衡、全集群唯一激活的分布式目录（已定）。脱水和复水只做持久化：`OnDeactivate` 写库、`OnActivate` 读库，已在 §6。带着内存状态在线迁移不需要：房间等结束，场景靠玩家切场景或排空后重建，热更靠重新加载（这是我的判断）。

### `Hash` 路由是取模

`Hash` 路由现在是取模（`route_table.cpp:39`，`nodes[route_key % nodes.size()]`），不是一致性哈希：实例数一变，所有键的归属都会洗牌。**有状态的实体不要靠 `Hash` 路由定位**，要用 `Locator` 加显式分配；`Hash` 适合无状态服务按键分流。

### 微服务这一层还缺什么

1. **服务发现和路由表的真实来源**：`RouteTable` 接口和内存版 `StaticRouteTable` 有了，真实来源（控制面）没有。etcd、redis、mongo 目前只有配置 schema，没有客户端代码。
2. **跨进程 RPC 的调用方部分**：§8。
3. **服务治理**：熔断、限流、过载保护、健康检查。silver-server 有 `component/breaker`、`ratelimit`、`ActorHeartbeat`、`router_heartbeat_design.md`，我只看到了名字，没读实现。我们这边还没设计。
4. **灰度**：信封里有 `gray_tag`，目前只透传，路由表不据此选实例。

### silver-server 的 `component/actor` 作参考

我只读了文件头：`ActorModule` 是一个模块，只管一种 actor 类型，持有 `actorMap`（roleId 到 actor）；每个 actor 一个协程，带消息队列，有 `OnInit` / `OnStop` / `OnTick`，tick 由框架通知并和消息处理串行；处理器有**两张表**：RPC 按 `rpcId`，客户端消息按 CS 命令号；通过一个注入契约（`agent.ActorRpcRouter`）和 agent 组件接起来。这和"一个 kind 一个 Module"几乎同构。两张表这一点见 §19。

## 15. 与现有代码的关系

| 现有 | 处理 | 理由 |
|---|---|---|
| `framework/app` 的启动编排（`Module` 的 `Init` / `Start`、拓扑排序、冻结） | **沿用** | 满足"每个 kind 一个 Module、`Start` 冻结 kind 表" |
| `App` | **改**：新增 `Dispatcher` 成员、`Build` 时筛选 `IExecutorProvider`、`Init` 结束后冻结 `Dispatcher`、**自动安装主循环这个 Module 并让所有用户模块隐式依赖它**、`mainLoop()` 引用和 `actors()` 指针 | §4 |
| `ITickable` / `IWaker` | **沿用** | `MainLoopSystem` 和 S0 的 `ActorSystem` 正好是这两个能力，`Wake` 的"单轮预算由模块自定、返回 true 表示还有积压"够用 |
| **App 的停机**（现在"只管启动，不管停机"，由 `main` 手动调各模块的 `Shutdown`） | **暂缓**：维持现状 | App 层面的编排先不考虑（用户 2026-11-03）。actor 的排空由 `ActorSystem` 自己提供，`main` 手动调。将来做时参考 GameserverNew 的 `Stopper.SaveAndStop(done)`，见 §19 第 1 项 |
| **启动就绪**（GameserverNew 的 `ReadyWaiter`） | **借鉴**，形状为**提议** | 需要异步初始化的模块实现 `WaitReady`，App 在 `Start` 之后、进主循环之前等它们，各有超时。我们的应答要经主循环才处理，所以**等待期间要泵主循环**，不能阻塞（GameserverNew 等就绪时没泵，只能由模块自己的线程触发就绪）。这是 App 层的改动，细节见 §19 第 23 项 |
| `MainLoopExecutor` | **拆成两半**（提议）：通用部分（`Submit` / `SubmitResponse` / `Drain(budget)`、入口限额、统计、主循环上下文的挂起表和 `TimerWheel`，MPSC 队列）成为 `MainLoopSystem` 的内部角色，不依赖 `base/net`；网络适配器（解信封、每连接和全局字节预算、`PauseRead`）留在 `framework/net` | 现在的实现是网络专用的（每个 IO 线程一条 `SpscQueue<Event>`，依赖 `base/net/connection/*` 和 `IMainLoopHandler`），`framework/app` 不能依赖它；生产者也不止 IO 线程。带预算的 `Drain`、入口背压的思路保留 |
| `NetModule` | **改**：增加收 `Dispatcher` 的入口；`AddAgentClient` 增加在 IO 线程直接处理的重载 | §4 |
| `ThreadPool::Post` | **沿用**，给 `Offload` 用 | 无 key 共享队列，满了返回 false，`Stop` 排空 |
| `ThreadPool::PostKey` | **不用它做分片执行器** | 它的全部队列共用一把锁（头注释明说面向微秒级以上的任务），也没有每片的定时器；分片要的是"每片一个线程加入口车道加自己的 `TimerWheel`"，这是另一个结构。`ThreadPool` 本身不改 |
| `EventLoop` | **不直接用**，S2 时评估能否抽取它的唤醒和定时器部分 | 它是网络内部类型，带 epoll 和连接表；Shard 不需要这些 |
| `MpscQueue` / `SpscQueue` / `SharedBuffer` / `TimerWheel` / `Now` / `MurmurHash64A` | **沿用** | 契约都满足（每个生产者内部有序、定时器单线程使用、`Now` 是原子量、哈希结果稳定）。`TryDequeueBulk` 里的峰值采样问题已修（见 §9「入口层与直接投递（实测）」），`PeakApprox` 现在是近似下界 |
| 信封与 Agent 路由（`dst`、`route_key`、`msg_id`、`remaining_ms`、`Direct` 路由） | **沿用，`seq` 改成 `Correlation`**（提议，§8） | 其余够用。还没上线，信封直接改、不考虑兼容；如果选了"信封加显式 kind 字段"也直接改 |
## 16. 未覆盖的需求（**提议**：明确列出，不在两种执行方式里解决）

执行模型这一层，两种加 actor 池和 `Offload` 是够的，这个判断依据是读过的几个框架里"无状态并行"都是普通 actor 加选择器（§3），不是对真实负载的统计。Orleans 还有进程级的 `SystemTarget`，我只确认了名字和 `IsSystemTarget()` 的处理。**"覆盖了多少"给不出百分比。** 下面是两种之外的需求：

| 需求 | 行业常见做法 | 在这里 |
|---|---|---|
| **单个大场景内的数据并行**（几千个实体每帧分给多核算） | ECS、job system、fork-join；MMO 更常见的是把世界按区域切到多个进程，每个单线程（通用印象，没核实具体产品） | **不覆盖**。`Offload` 能手工做 fork-join，但没有一等支持。倾向于明确不做，靠切分区域解决 |
| **重 actor 独占线程** | CAF 的 detached（`private_thread_`）、Akka 的 Pinned | 只有哈希分片，热点靠分片组隔离，**不能保证一个场景独占一个线程**。可留一个"一个组只有一个分片且只放一个 kind"的接口 |
| **跨实体事务**（两个玩家交易） | 一个协调者 actor 串行化、saga、数据库事务 | 不是执行模型的事，要在业务层做；需要给出推荐的模式，现在没有 |
| **发布订阅和组播**（场景内广播、聊天频道） | skynet 的 `multicastd`（服务目录里有，没读）、Orleans 的 streams | 设施，不在两种里 |
| **可持久的定时器**（每日 5 点刷新、离线倒计时） | Orleans 的 reminders（`Orleans.Reminders` 项目存在，没读） | 只有内存里的定时器；仓库里有 `TimeUtil` 的日期计算，没有持久化 |
| **消息优先级**（踢人、停机指令要插队） | CAF 的紧急消息、protoactor 的系统邮箱 | 没设计 |
| **确定性帧同步**（战斗服的固定步长、输入缓冲） | 固定时间步加按帧收输入 | 能在 actor 加 tick 上自己实现，需要约定 |
| **状态持久化框架**（读写库、快照、journaling） | Orleans 的 persistence、journaling、event sourcing（目录存在，没读） | 只留了 `OnActivate` 读、`OnDeactivate` 写 |

**注册结构保持开放**：执行方式用枚举加登记者类型表达，将来加第三种执行器不改现有接口。

## 17. 对照其他框架

读了 skynet、protoactor-go、Akka.NET、CAF、Orleans 的相关源码（只看邮箱、调度、网络入口、队列满处理、卡死与失败处理，没有通读）。

| | 串行保证 | 调度 | 邮箱默认上限 | 满了怎么办 | 网络线程到 actor |
|---|---|---|---|---|---|
| skynet | `in_global` 标志 | 共享线程池，按线程权重批量取 | 无 | 只告警 | socket 线程直接 `skynet_context_push` 进目标服务邮箱 |
| protoactor-go | CAS 空闲到运行 | `Dispatcher` 接口，每轮 300 条 | 默认无界，可选有界 | 丢最旧 | `endpointReader` 解出消息后直接 `Root.Send` |
| Akka.NET | 状态位 `Scheduled` | 多种调度器，每轮 10 到 30 条 | 可选有界 | 阻塞发送方至超时，转 dead letters | 没读 |
| CAF | LIFO 栈的读者阻塞标记 | 工作窃取，可独占线程 | 无 | 网络侧有容量的缓冲背压 | multiplexer 加有容量的缓冲 |
| Orleans | 每个激活一个循环加调度组 | .NET 线程池 | 默认不限，可配 | 拒绝并通知发送方 | `MessageCenter.ReceiveMessage` 按 `GrainId` 找或建激活，再投递 |
| **这里** | **Shard 单线程** | **哈希分片，S0 在主循环** | **有界** | **拒绝并通知发送方（请求）** | **IO 线程经 `Dispatcher` 直达执行器** |

**执行方式的选择**：skynet 和 Orleans 按"登记者"选（服务、grain 类特性）；protoactor 邮箱创建时注入调度器；Akka.NET 配置里命名多种调度器；silver-server 进程级 `Mode` 加按消息覆盖；Netty 的 `pipeline.addLast(executorGroup, handler)` 可以给每个 handler 单独指定执行器（通用印象，没在本地核实）。**按登记者选加网络线程直达**是主流，进程级单一模式是更早、更简单的做法。

**借鉴**：Orleans 的激活状态机、慢请求与卡死检测、非 Valid 激活的单独处理、异常回给调用方；skynet 的版本计数器监控；protoactor 的两方法 `Dispatcher` 接缝思路、`MailboxMiddleware` 的指标钩子；Akka 的命名 dispatcher、`Stash`。
**不借鉴**：监督树与父子关系、工作窃取（负载均衡好，但 actor 迁线程、验证难）、无界邮箱、Orleans 的迁移与重平衡（它自己也标为实验性：`AddActivationRebalancer`、`AddActivationRepartitioner`；重平衡按激活数与内存的熵衡量，重分区按 grain 之间的调用次数把通信密的搬到一起）。

### 与 gRPC C++ 的对照（RPC 这一层）

读了本机的 gRPC 源码（`doc/statuscodes.md`、`include/grpcpp/` 下的 `client_context.h`、`server_context.h`、`support/server_callback.h`、`impl/propagation_bits.h`、`include/grpc/event_engine/`），没测过它的性能，不下快慢的结论。它们不是同一类东西：gRPC 是通用的跨语言 RPC 框架，我们是游戏服进程内加同机 sidecar 的消息和 RPC 层，和 actor 执行模型绑在一起。

| 维度 | gRPC C++ | 我们（设计，未实现） |
|---|---|---|
| 回调在哪跑 | gRPC 自己的线程上，可并发，不许阻塞（`server_callback.h`），没有回到发起方的亲和性，业务要自己同步或切线程 | 回到**发起方的执行上下文**，串行、不加锁，实例回收则丢弃 |
| 服务端必须应答 | 必须 `Finish(Status)`，库不替你收尾 | `ReplyHandle` 析构自动回 `kNoReply` |
| 异步风格 | 同步、`CompletionQueue`、回调三套并存 | 一套回调，协程以后加，没有同步版本 |
| 超时 | `set_deadline`；`FromServerContext` 默认把 **deadline 和取消**一起传给子调用（`GRPC_PROPAGATE_DEFAULTS`） | deadline 沿链传，且显式预留回程时间；**没有取消传播** |
| 取消 | `TryCancel` 传到服务端，服务端 `IsCancelled` / `OnCancel`（尽力而为） | `Cancel` 只在本地丢弃，对端仍处理完，靠 deadline 兜底 |
| 错误模型 | 整数码加字符串，17 个规范码，库只产生其中一小部分 | 只有码，框架与业务数值分区（§8 `Status`） |
| 流式 | 一元、服务端流、客户端流、双向流 | 没有，用 `Oneway` 推送代替 |
| 元数据、拦截器 | 变长 key-value；有拦截器 | 信封固定字段；没有拦截器 |
| 服务发现、负载均衡、重试、健康检查、TLS | 都有 | Agent 路由表有 `Any` / `Hash` / `Broadcast`；控制面没做，重试明确不做，跨机无 TLS |
| 代码生成 | protoc 插件，强类型存根，每种风格各生成一份 | 方向是一份生成加运行期登记器（§8），生成器没设计 |
| 传输 | HTTP/2 over TCP | 同机共享内存环加 Agent，跨机 TCP |

**借鉴的**：`Submit` 不得内联执行的契约（来自 `EventEngine::Run`）、新执行模型的实现清单（来自 `EventEngine` 的 README，§4）、库产生的码和应用的码分清（`doc/statuscodes.md`，我们用数值分区）。**不借鉴的**：三套风格并存且规则各不相同（它自己在 `SetServerApiType` 里要为"把方法覆盖成另一种类型"打日志，说明这是个坑）。**可能要补的**：拦截器（中间件）钩子，追踪、指标、鉴权会用到（§19）；取消传播，收益对游戏服不大，先不做。

### 请求体在哪一步解码、解码失败怎么办（对照别家，**已定**）

2026-11-05 读了各项目 GitHub 上 master / main 分支的源码（只看这一个问题，没通读，没测性能）。表里"没查到"的格子是这次抓取的文件里没有，不是不存在：

| 框架 | 在哪一步解码 | 解码失败 |
|---|---|---|
| gRPC Go（`rpc_util.go` 的 `recv`、`server.go` 的 `wrapUnaryHandler`） | 处理这条 RPC 的流程里，由生成的 handler 调 `RecvMsg` 时触发 | 回 `codes.Internal`，"failed to unmarshal the received message" |
| gRPC C++（`method_handler.h`） | 框架驱动的 `Deserialize` 回调，不在 `RunHandler` 里；跑在哪个线程头文件里没有 | 状态非 OK 就不调业务，经 `ServerSendStatus` 回给调用方；具体状态码没查到 |
| brpc（`baidu_rpc_protocol.cpp` 的 `ProcessRpcRequest`） | 同一个函数里同步解析，解析完才调业务方法（业务方法可以再转线程池） | `SetFailed(EREQUEST, "Fail to parse request=…")`，回错误应答 |
| Akka Artery（`Codecs.scala`） | 远程流里单独的 `Deserializer` 阶段，**在投递到 actor 邮箱之前** | 记 error 日志、丢弃；不进 dead letters，不隔离连接；不通知发送方 |
| protoactor-go（`endpoint_reader.go`） | 收包的 goroutine，**在 `Root.Send` 之前** | 记日志后 `return err`，**整条接收流被终止**；不通知发送方 |
| skynet（`skynet.lua`） | 目标服务自己的线程上、恢复处理协程之前（`p.unpack`） | `unpack` 本身抛错时，"给调用方回 `PTYPE_ERROR`"那段不会执行（只有处理函数抛错才回），没有错误应答 |

**能看出的**：
- **两派。** actor 框架（Akka、protoactor）在入口解码，邮箱里是类型化对象，失败时它们丢弃或断流、不通知发送方（这是它们的选择，不是入口解码必然如此：我们的信封带请求类型，入口也可以直接回错误）；RPC 框架（gRPC、brpc）解码失败回错误应答，gRPC Go 用 `Internal`，brpc 用专门的 `EREQUEST`。
- **做 RPC 的没有静默丢请求的。** skynet 是反例，调用方拿不到错误。
- **protoactor 的"断整条流"代价最大**：一条坏消息连累同一连接上所有消息。

**已定**（用户 2026-11-05 选定执行时解码，按下面四条）：
1. **解码放在 thunk 里、处理函数执行时**（执行器线程，不在 IO 线程）。理由：IO 线程只做定长解析（§4）；邮箱里存字节，和 §10 的"本地消息统一用字节"一致；邮箱满、实例不存在、已过期这些拒绝都发生在解码之前，过载时不白花 CPU（Akka、protoactor 的入口解码做不到这点，坏消息和好消息一样先花解码的代价）。代价：坏消息先占一个邮箱额度；解码耗时算进该实例的回合，大消息会拖慢同 Shard 的别的实例，所以载荷大小要在入口限制。
   **入口解码的好处（反方）**：解码的 CPU 落在多个 IO 线程上，不占串行的 Shard 线程（S0 时就是主循环）；解不开的坏消息不进邮箱；入口可以直接回错误。它的代价：邮箱要存类型化对象（每种消息一个类型的队列，或 `unique_ptr<Message>` 加堆分配与虚析构），和 §10 的"本地消息统一用字节"冲突；被邮箱拒绝的消息也先花了解码；同进程调用没有 IO 线程，"入口"变成发起方线程。**哪边更划算没测过**，要等有真实消息和负载再比。
2. **终态**：请求回框架错误码"解码失败"（1..999 段，码值见 §19 第 12 项），单向消息计数加限频日志；日志只记 `msg_id`、来源、长度，不记载荷（不可信输入）；计数按 `msg_id` 分，不按实例。回错误由 thunk 直接发，处理函数没被调用。
3. **不做"断整条连接"。** 是否对"同一来源连续解码失败"踢连接，留给网关：`CLIENT` 来源本来就不可信；`INTERNAL` 来源的解码失败是 bug，应当报警。
4. proto3 没有 `required`，解码失败只来自线格式损坏；字段取值是否合法是业务校验，不算框架的解码失败。

## 18. 验证计划与落地路线

| 阶段 | 内容 | 要证伪 / 验证什么 |
|---|---|---|
| **S0** | `App` 增加 `Dispatcher` 和能力筛选；`MainLoopSystem`（演进 `MainLoopExecutor`）；`ActorSystem` 加统一的 Shard 结构跑在主循环上，加一两个 kind 与测试 | `Dispatcher`：重复登记报错、冻结后登记报错、未登记消息号的终态、执行器缺失时的明确错误；同键严格串行；同一连接发给同一 actor 保序；跨 kind 发消息；每轮预算的公平性（一个刷屏的 actor 不饿死别人）；激活期间的消息处理与加载失败；定时器；停机排空；两层背压与所有终态（§7 表）；RPC：挂起表、超时、应答与超时竞争、`late_reply` / `unknown_seq` / `stale_epoch` 计数、应答不被慢请求堵住；`ReplyHandle` 析构自动回错、延迟应答、跨线程析构；`Call` 失败路径异步回调；`Cancel` 的各种时机（完成后、超时后、失败后调用，取消后晚到的应答计 `late_reply`）；`duplicate_reply` / `no_reply` 计数；发起实例回收后回调不被调用；`Join` / `Each` / `Seq` 的汇合与终止；链式调用：下游超时早于上游、剩余不足预留时不发下游调用、下游框架错误映射成 `kUpstream`、等待期间实例回收则上游收到 `kNoReply` |
| **S1** | 把场景 kind 放进独立分片组，每片一个线程；有"进程内无状态并行"的真实需求时，加 actor 池 | 同键仍然串行；分片间互不阻塞；入口车道的延迟和吞吐、各 kind 的车道互不拖累（**需要压测**）；asan 下的并发用例；**tsan 上真机**；池 kind：成员落在不同分片（`key % N`）、选择器轮转均匀、同一成员内串行、`Offload` 不占分片线程 |
| **S2** | 评估是否换更专用的分片循环、抽取 `EventLoop` 的通用部分 | 只在 S1 实测出瓶颈时做 |

## 19. 未决项

1. **App 是否增加停机阶段**（§15）：**暂缓**，先由 `main` 手动调（用户：App 层面的编排先不考虑）。将来做时参考 GameserverNew 的 `Stopper.SaveAndStop(done)`：非阻塞、`done` 回主循环、等待期间继续泵主循环、每模块有超时；超时检查要放在循环顶部，放在"队列空"分支里会被持续到达的任务饿死。会改变 `framework/app` 与 Go 版的对照关系。
2. **实体 kind 来自消息号还是信封里的显式字段**（§5）：消息号定 kind 零改动，但"一个消息号只属于一个登记者"；显式字段更灵活，要改信封（现在 `reserved0` 要求为 0，需升版本）。有了 IDL，绑定在登记时由生成的 `Register*` 完成（§8），这里只剩"信封要不要带显式 kind"。
3. **客户端命令和服务之间的 RPC 要不要区分**：**已决**，要区分（用户确认）；不用两张表，用一张表加 service 级 `audience`（INTERNAL / CLIENT / CLIENT_PUSH），见 §8。silver-server 和 GameserverNew 都是两张表，这里没有照搬。
4. **Deactivating 期间到达的消息**：拒绝并回"正在停用"（默认），还是排队后重新激活。
5. **`ctx.Fail` 后是否存盘**：默认不存，避免把坏状态写进库。
6. **激活失败后重试的退避**：谁触发（下一条消息，还是调用方）、间隔多少、最大次数。
7. **关停时各 kind 的先后**：默认按注册序逆序，还是每个 kind 声明优先级；存盘的截止时间。
8. **卡死的阈值和处置**：超过多久算卡死；是否让该 Shard 拒绝新消息。
9. **S1 的分片循环**：复用 `EventLoop` 的唤醒与定时器，还是单独写。
10. **场景是否、何时需要分片**：要靠实测（§9）。
11. **本地消息是否要类型化快路径**：有数据再说。
12. **错误码区间**：已提议框架 1..999（沿用 Agent 的 1、2）、业务 ≥ 1000（§8 `Status`）；具体码值实现时定。
13. **单向消息在激活失败时只计数记日志**：按信封约定的提议，用户未单独表态。
14. **创建策略默认显式**：提议，用户未单独表态。
15. **RPC 回调还是协程**（§8）：提议第一版回调。
16. **actor 池的选择器与分片**（§3、§9）：第一版轮转；是否加"最空闲优先"（读各分片入口车道深度，只看排队数看不见在跑的慢任务）；池 kind 的分片函数是否用 `key % N`；以及是否真的需要进程内无状态并行（没有需求就不做）。
17. **独占线程的分片组**、**场景内并行**：倾向于明确不做（§16）。
18. **`MainLoopExecutor` 拆成通用部分与网络适配器的重构细节**（§4、§15）；`IExecutor` / `IExecutorProvider` 的形状 §4 已有草图；主循环自动安装的实现细节（隐式依赖、拓扑序最前）。
19. **未覆盖需求清单**（§16）里哪些要在后续设计里补。
20. **`Correlation` 的位宽与纪元取法**（§8）：16 / 16 / 32 位是草案；纪元取启动时刻秒数的低 16 位，还是别的。
21. **回调契约**（§8）：至多一次、发起实例回收后不调、`Cancel` 之后不调，是否都接受。
22. **编排辅助的语义**（§8）：`Join` 要不要快速失败；聚合层是否自带 deadline；多个错误怎么汇总。
23. **启动就绪等待的形状**（§15）：等待期间泵主循环；超时默认值；失败是否中止启动；与第 1 项同属 App 层，要不要一起暂缓。
24. **接口定义与生成器**（§8）：方向已定（一份生成加运行期登记器；gRPC schema 写法，`rpc` 签名决定 `Call` / `Send`，`Empty` 是单向，`stream` 报错）。待定：生成器何时做、生成什么（强类型存根、注册函数、网关白名单）；`thunk` 的 `impl` 参数的具体形状；命名后缀是否留作 lint。**已定**：单向方法只生成 `Send`、处理函数没有 `ReplyHandle`；处理函数执行时解码，请求回"解码失败"，单向计数加限频日志，不断连接（§17 对照了 gRPC、brpc、Akka、protoactor、skynet；§7 终态表已加一行）。
25. **`Target` 的具体写法**（§8）：草图不是定稿。
26. **`Status` 的形状**（§8，提议）：只有码、没有消息串是否够用（细节放 `body`）；`MapUpstream` 对业务错误是否一律透传；`deadline_reserve` 的默认值（要实测）。
27. **入口车道的细节**（§7、§9）：每条车道的默认上限和配置位置；Shard 线程轮转各车道时每轮每条取多少（公平策略）；kind 很多时车道数（分片数 × kind 数）的开销。直接投递的重新评估要等有实测瓶颈（§9 记了对比数据）。
28. **拦截器（中间件）钩子**（§17）：追踪、指标、鉴权要不要在 `Dispatcher` 层加。GameserverNew 的 `core/dispatcher` 有 `Middleware`，gRPC 有 interceptors，我们现在没有。
29. **"会话到网关"的映射与推送路由**（§8）：`Target::Client(role)` 怎么找到持有该会话的网关。
30. **`msg_id` 的细节**（§8）：第一版手写，冲突检查范围是所有 proto 一起（已定）；手写要不要按服务划号段、避免多人改同一区间冲突；是否提交一份生成的 id 清单文件，让改名或改号在评审里可见；何时加"没写就取哈希"的自动分配；`ResolveMethod(name)` 何时做。
31. **`HandlerContext` 与 `audience` 的细节**（§8）：`RequestInfo` 里放什么；`CLIENT` service 的处理函数是否都带 `client_role`；`CLIENT_PUSH` 的发送端存根形状。
32. **主循环 RPC 超时的精度**（§4）：沿用 tick 粒度（约 33 ms），还是改成 `wait_until(min(下次 tick, 时间轮最近到期))`。
33. **峰值采样间隔**（§9）：`kPeakSampleEvery` 现为 1024；积压有界（每车道 ≤ 4096）时间隔 64 也与不采样持平，只有无界积压才慢（见 §9 的隔离实验），不必再放大；要不要让间隔随上次采到的深度变大；`queue_peak` 是下界是否够用。
34. **网关路由表**（§8，用户：后续在 gatesvr 做）：`cmd_id` → 后端服务类型 → `Locator` → 节点；表由 proto 里的服务类型生成还是单独配置；与第 29 项（会话到网关的映射）互为反向，一起设计。
35. **消息的编解码运行时**（§8）：**已定**用 libprotobuf（用户选定）。要做：给 C++ 的 `vcpkg.json` 加 protobuf（现在没有；Go 侧 `go.mod` 已有 `google.golang.org/protobuf`）；生成器除了 RPC 存根，还要调 `protoc --cpp_out` 生成消息类。待查：本机 `protoc` 是 3.21.12，要和 vcpkg 里的 libprotobuf 对上版本（生成代码会校验版本，不一致编译就报错，**没在本仓库试过**）；`ParseFromArray` 可以直接读 `SharedBuffer` 的连续内存，但 `string` / `bytes` 字段默认会拷进消息对象，这点**没测过**，是否要紧要等有真实消息。

## 附：不做的事

在线迁移、自动重平衡、全集群唯一激活的分布式目录、监督树与父子关系、工作窃取、无界邮箱、自动重试（重试是调用方的策略，不是机制）。
