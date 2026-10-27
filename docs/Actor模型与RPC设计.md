# Actor 模型与 RPC 设计

本文是 `breeze` 服务间通信的设计基线：**结论、约束、代价、不变量**。

实现前先读本文。若发现本文与源码/测试冲突，以源码和测试为准，并回来改本文。
本文不写"怎么做"的细节实现，只写代码读不出的东西——为什么这样、代价是什么、失败模式是什么。

**参考实现**：`GameserverNew`（同源上一代，Go 1.26，6 个服务，210 次提交，1143 个 Go 文件）。
下文所有"参考实现"均指它。本文同时记录它做对的、做错的、以及**不能照抄**的部分。

---

## 0. 结论速查

### 要做

1. **先补队列与预算，再谈 RPC**。顺序不能反（理由见 §3）。
2. RPC 机制只负责报事实：相关性、deadline 传播、一次性回信、失败分类。**不做任何自动重试**。
3. actor 是**状态所有权单位**：一个 actor 一个执行流，服务间 RPC 最终落到某个实例上执行。
4. 地址是三元组 `(节点, actor, 方法)`；actor 实例地址是二元组 `(Kind, Key)`。
5. **取消必须自己设计**——gRPC 有一等公民，参考实现没有。
6. 凡是"必须接线、漏了不报错"的东西，一律改成**类型/构造期约束**。

### 不做（理由见 §9）

流式（bidi/streaming）、插件化名字解析与 LB、mTLS、结构化错误详情、hedging、自动重试。

### 不变量（见 §8，改设计前先看）

每个请求恰有一个终态；deadline 只能缩短；取消幂等；一个 actor 一个执行流；ActorSystem 没有邮箱；
实例地址是 `(Kind, Key)`；不存在的实例必须有明确终态；Build 之后不再注册模块。

---

## 1. 背景、约束与现状

### 1.1 已确立的约束（不可动摇）

| 约束 | 出处 | 对 RPC 的影响 |
|---|---|---|
| 主循环单线程，`Update`/`Wake`/`Reload` 与 `Stop`/`Shutdown` 不重叠 | `src/go/core/app` | pending 表可以是纯单线程结构，**不需要锁** |
| Build 之后模块集合冻结，`RegisterModule` 在 Build 后 panic | `src/go/core/app/app.go` | **actor 是运行期动态的，绝不能进 `modulesArr`**，必须由宿主 Module 记账 |
| 框架不做任何运行时兜底（无上限、无告警） | `src/go/core/app/CLAUDE.md` | 上界/告警是模块契约的一部分，不是框架责任 |
| 模块集合的差异靠 `DependsOn` 拓扑排序表达 | `src/go/core/app/app.go` | 拓扑序只用于**启动**；actor 之间的依赖是运行期消息，不进拓扑 |

### 1.2 参考实现的核心教训（本文多处引用）

| 教训 | 事实 | 后果 |
|---|---|---|
| **必须接线的东西不能做成可选** | `WithPoster` 只出现在测试里，两个生产构造点都没传 | 回调实际跑在网络读协程上，与文档写的"串行"相反。同类问题在 `genrpc.Init(core)` 上重演一次 |
| **并发结构是被需求逼出来的，不是默认要抄的** | pending 表 16 分片 + 每分片 `sync.Mutex` | 它需要这些，是因为回调来自 3 个不同的 goroutine。**breeze 单线程回投后，分片和锁全部多余** |
| **相关性 ID 必须带发送方命名空间** | 每个 RouterAgent 的远端 seq 都从 1 开始，而接收方只按 seq 索引 | ≥3 个 RouterAgent 时回包投错来源，且错包在 protobuf 层不报错（静默串话） |
| **"目标不存在"不能是静默的** | 路由层查不到目标节点时直接 `return` | 调用方只能等到超时，把"配置漏了"误读成"对端慢" |
| **文档会漂移，注释比文档更危险** | 设计文档仍写"用 `time.AfterFunc` 触发超时"，实际早已改成集中扫描；`conn.go` 里一句"回调仍由 poster 回投主循环"是假的 | 后来人会引用注释来证明自己不用加锁 |

---

## 2. 三个概念的分工

初学者最容易把它们混成一个：

| 概念 | 是什么 | 不是什么 |
|---|---|---|
| **service**（IDL 里的） | **契约单位**：一组方法的命名空间 + 版本边界 | 不是状态所有者，不是部署单位 |
| **actor** | **状态所有权单位**：一个实例一个执行流，状态私有 | 不是方法命名空间（那是 service） |
| **进程**（`*svr`） | **部署单位**：托管一组 actor 类与基础设施 | 不是契约单位 |

三者是**多对多**：

- 一个进程承载多个 service / actor 类（`roomsvr` = RoomRemote + CombatRemote + …）
- 一个 service 的实现可以出现在多个进程（无状态时），靠 `ServerType` + 节点寻址区分实例进程
- **绝不要按进程划 service**（"一个进程一个大 service"），也不要按 CRUD 或技术分层划（那会切开一致性边界）

### 2.1 地址三级模型

```
route      = "Service/Method"                    → 选方法     （编译期已知）
Target     = (ServerType, RoutingMode, Key)      → 选节点     （运行期，路由层用）
ActorAddr  = (Kind, Key)                         → 选实例     （运行期，actor 层用）
```

参考实现只有前两级：`Target.ByHash(assignmentID)` 选节点，进程内的"roomID → room 对象"完全散落在业务
handler 里（`h.roomMgr.Get(roomID)` 这类手写代码）。**actor 化的本质就是把第三级也做成声明式。**

⚠️ **实例地址必须是 `(Kind, Key)` 二元组，不能只有 Key**——`AuctionActor[1001]` 与 `RoomActor[1001]`
是两条不同地址。只用一个整数做键，迟早串类。

### 2.2 "只能 send" 不是缺陷

actor 里没有同步等待，返回/超时/重试是**同一台状态机的三个面**，不是三个机制：

| 机制 | 实现 | 关键点 |
|---|---|---|
| 返回 | 回信是普通 send（回到发送方地址），靠相关性 ID 匹配 | 调用方**不能等**，只能把等待变成状态 |
| 超时 | 请求方自己记 deadline，到点判自己失败 | 超时逻辑**完全在请求方**，被调方一无所知 |
| 重试 | 请求方状态机 + 退避定时器 + **复用同一相关性 ID** | 机制不自动做（§9） |

"我发出去了一件事，还在等结果"这件事，必须有地方记账。actor 没有调用栈可挂，所以账单必须显式。

---

## 3. 地基：队列、预算、排空

**这一节是 RPC 的前置条件。** 参考实现的 RPC 之所以复杂，一半原因在它先有了队列；breeze 若直接做 RPC，
就必须自己发明这些，而且大概会发明错。

### 3.1 两条投递路径，且必须由类型分开

```go
// 完成回调投递口：接口里根本不存在阻塞方法
type NonBlockingPoster interface {
	PostNonBlocking(fn func())
}

// 入口任务投递：满则阻塞，背压回压到 IO 层（具体方法，不进接口）
func (q *Queue) Post(fn func())
```

两条路径按**能否被推迟**区分，**不可混用**：

| 路径 | 承载 | 满时行为 | 为什么 |
|---|---|---|---|
| `Post` | 入口任务：客户端请求、集群入站帧 | **阻塞**，背压回压到 IO 层 | 入口可以慢下来，这是设计意图 |
| `PostNonBlocking` | 完成回调：DB/Redis 异步操作结果 | 溢出到无界缓冲，**永不阻塞** | 见下面的死锁机理 |

**完成回调为什么不能阻塞**（这是一个只在停服 teardown 才暴露的死锁）：

1. 完成回调是"已被接纳工作的尾巴"，结果已经产生。推迟它不减少任何负载，只会让持有结果的 goroutine 挂住。
2. 发起方通常在 `opsWG.Wait()` 上等这个回调。
3. 阻塞投递把"等回调"变成"等主循环消费队列"。
4. 而停服期间主循环正**同步跑在模块的 `Shutdown` 里**，不消费队列。

→ 死锁。**这条边界必须由类型强制，不能靠文档**：接口上不存在阻塞方法，拿到什么实现都不可能接错。
溢出深度的上界是 in-flight 异步操作数（由各 client 自身的并发上限约束），可用 `OverflowPeak()` 观测。

### 3.2 先入队，后唤醒

```go
func (q *WakeableQueue) Post(fn func()) {
	q.queue.Post(fn)
	if q.notify != nil { q.notify() }   // 顺序不可颠倒
}
```

颠倒的后果：调度器可能先醒来扫到空队列再休眠，随后任务才入队且不再产生通知 → **任务滞留到下一次有人投递**。
表现为偶发延迟，极难定位。

### 3.3 单轮预算：框架给上限，模块可更保守

参考实现用 `WithProcBatch(n)`（默认 256）给**每个 Processor 每轮的统一上限**，模块在上限内消费。
breeze 现在是模块自定（`Wake()` 自己 drain N 条），框架无兜底。

**建议两者合并**：框架给上限，模块仍可在上限内取更小（它才知道自己一条要多久）。这样"忘了设上限"
不再是静默故障。

返回语义要定死：`Drain(limit, run) int`，**`ran < limit` ⇒ 队列已空**。需要完整排空的调用方据此循环。

### 3.4 停服排空

teardown 顺序：

```
stopModules → cancelContext → shutdownModules → drainAllProcessors
```

`drainAllProcessors` 一轮返回 0 即终态，靠两条契约支撑：

1. **谁创建 goroutine，谁在自己的 `Shutdown` 中等待。** `shutdownModules` 返回时所有生产者已 join，
   此后不可能再有任务入队。**排空轮数本身不构成任何保证**——加到三轮也覆盖不了一个没 join 的生产者。
2. **`Shutdown` 不得等待"需要主循环执行才能完成"的回调。** 主循环此刻正同步跑在 `shutdownModules` 里，
   不调度 Processor，等这种回调必然自锁。

需要主循环推进的等待（如玩家存档）放在 `Stop` 阶段——它是唯一"在主循环 goroutine 上、且早于所有模块
`Shutdown`"的槽位，此时基础设施仍可用，可以**边等边调度**并带超时。

---

## 4. RPC 机制

### 4.1 相关性：单调 seq

```go
func (c *Core) Call(t Target, route string, body []byte, ctx Ctx, on func([]byte, errcode.ErrCode))
func (c *Core) Send(t Target, route string, body []byte, ctx Ctx)   // 单向：不分配 seq
```

- `seq` 单调递增、**永不复用**。于是迟到回包不可能串到新请求上。
- pending 表按 seq 索引。
- **breeze 单线程 → 用普通 `map[uint64]*inflight` 即可，不要分片、不要锁。**
  参考实现的 16 分片是被"回调来自 3 个 goroutine"逼出来的；只要回包也回投主循环，那个前提就不成立。
- `inflight` 对象池化可选（减少 GC），不是必需。

### 4.2 超时：集中扫描，挂在周期更新上

```go
// 不要每请求一个 timer；挂在已有的周期更新上
func (m *RPCModule) Update(now time.Time, updateCount uint64) {
	if updateCount%10 == 0 {          // 100ms × 10 ≈ 1s 扫描精度
		m.core.scanExpired(now)
	}
}
```

**为什么不用 `time.AfterFunc`**：

| | 每请求 timer | 集中扫描 |
|---|---|---|
| 成本 | N 个 runtime 定时器堆节点 | O(pending) / 扫描间隔 |
| 回调执行流 | 定时器 goroutine（**容易顺手改状态，破坏单线程**） | 主循环，天然串行 |
| 精度 | 精确 | 受扫描间隔限制 |

**代价要写进契约**：`扫描间隔` 就是超时精度上界。500ms 的超时在 100ms 间隔下最坏晚 20%。

### 4.3 终态唯一

每个请求**恰有一个终态**，四个来源必须互斥：

| 终态 | 触发 |
|---|---|
| 正常回包 | 收到响应 |
| `ERR_TIMEOUT` | 扫描到 deadline 过期 |
| 机制错误 | 发送失败（队列关闭 / 序列化失败） |
| `ERR_INTERNAL` | `Close()` 排空在途（停服） |

实现方式：**在同一个临界区里"取出并删除" pending 项**，拿到项的那条路径才产生终态，其余路径看到的是
`nil`（迟到，静默丢弃并归还资源）。

### 4.4 deadline 传播

| 环节 | 做法 | 反例（参考实现） |
|---|---|---|
| 上线表示 | **相对量**（剩余毫秒），规避跨机时钟漂移 | ✅ 它做对了 |
| 接收方 | 落地转本地绝对时间：`localDeadline = now + d` | ✅ |
| 转发下游 | 按 `localDeadline - now` 重算剩余量再发 | ❌ 它每跳重新给满额，总时长无上界 |
| 已过期 | **立即失败** | ❌ 它回落到默认值（120s），等于"失效即放行" |
| 覆盖 | `Target.Timeout(d)` **只能缩短，不能延长** | ✅ |

`Ctx` 承载跨跳传播信息，**不承载路由意图**（路由意图放 `Target`）：

```go
type Ctx struct {           // 字段私有 + WithXxx 返回副本（不可变链式）
	deadline     time.Time  // 剩余预算
	span         Span       // 轻量链路跨度
	fromNode     uint32     // 来源节点
	clientRoleID uint64     // 已认证角色，供跨节点透传
	stale        func() bool // 实体失效护栏
}
```

**`stale` 护栏**：玩家离线/房间销毁后，整条编排链在**发起子调用前**自动放弃。注意它的语义是
"不再发新请求"，**不是取消在途**（§6）。

### 4.5 失败语义

**错误码是值（int32），不是 Go error。** 跨进程只传数字，不传错误文本（诊断文本留服务端日志）。

```go
const (
	OK           ErrCode = 0
	ERR_INTERNAL ErrCode = 1   // 0-999 框架级
	ERR_TIMEOUT  ErrCode = 2
	ERR_NO_ROUTE ErrCode = 3
	ERR_UNAUTHED ErrCode = 4
	ERR_UNMARSHAL ErrCode = 5
	// 1000+ 业务级
)
func CodeOf(err error) ErrCode   // 任意 error 归一成码，未知一律 ERR_INTERNAL
```

**⚠️ 三态结果：`ERR_TIMEOUT` 是"结果未知"，不是"失败"。**
机制**无法撤回已发出的消息**——超时只意味着"我不等了"，对端可能正在执行、已经执行完、或回包在路上。
API 上必须能区分三种：

| 结果 | 来源 | 能否安全重试 |
|---|---|---|
| 成功 | 正常回包 | — |
| **确定失败·未执行** | 目标不存在、邮箱满被拒、鉴权不过、解包失败 | ✅ 安全 |
| **确定失败·应用拒绝** | 业务码（余额不足、状态不允许） | ❌ 重试也一样 |
| **未知** | 超时、失联、连接断 | ⚠️ **仅当幂等或有去重时**；否则必须先查询或补偿 |

参考实现没有在类型上表达这个区分（`ERR_TIMEOUT` 与 `ERR_INTERNAL` 同为"未知"但看不出来）。它没有重试机制
所以问题不爆发；**只要有人加业务级重试，`ERR_TIMEOUT` 就是重复执行的入口。**

**框架提供的三个失败语义助手**（可直接照抄，125 行）：

| 助手 | 作用 |
|---|---|
| `Register` 校验 + `MustRegister` panic | 空 route / nil handler / **重复注册**都在启动期暴露 |
| `Dispatch` 未注册 → `ERR_NO_ROUTE` | 明确的机制错误，不是静默无响应 |
| `RecoverRoute` | handler panic → `ERR_INTERNAL`，不炸进程 |
| `OnceReply` | 回包只放行第一次（`atomic.Bool.Swap`） |
| `ReplyProto[T]` | nil 响应 / marshal 失败 → `ERR_INTERNAL` |

### 4.6 分发：注册表 + 生成期唯一性

- route 字符串 = `"ServiceName/MethodName"`。
- **业务代码永不手写 route 字符串**，只引用生成器产出的常量（改名即编译期暴露）。
- 分发是 `map[string]RouteHandler` **查表**，不是 switch。
- **生成期必须检查 service 名在 kind 内唯一**，重复直接终止生成——不能留给 Go 编译器，因为 protobuf 允许
  `pkgA.AuctionActor` 与 `pkgB.AuctionActor` 共存，生成到同一个 Go 包才炸，那时报错信息看不出根因。
- FRONTEND（客户端入口）与 BACKEND（服务间）用**两个 `Dispatcher` 实例**物理隔离。

### 4.7 编排：回调解构，不引 Future

| 助手 | 语义 |
|---|---|
| `Join2/Join3/Join4` | 并行等待固定数量**异构**调用 |
| `Each` | 对多个**同构**目标执行同一调用 |
| `Seq` + `Next` | 顺序执行异步步骤 |

为什么不用 Future/await：Go 里自造 Future 需要 `Then`/`Await` 之类的 DSL，而 `Await` 一旦存在就会被在
actor 里误用（死锁）。**没有 `Await` 可调，就不可能在 actor 里等。**

所有编排助手在**发起子调用前**和**每一步之间**都要查 `ctx.Stale()`，避免"玩家已下线还继续编排"的孤儿副作用。

---

## 5. Actor 层设计

### 5.1 ActorSystem 不是父 actor

这是最容易混的一处。区别不是层级，是**种类**：

| | ActorSystem | 监督者 actor |
|---|---|---|
| 有邮箱吗 | ❌ **没有** | ✅ 有 |
| 有私有业务状态吗 | ❌ 没有 | ✅ 有（子 actor 引用、重启计数、配额） |
| 收业务消息吗 | ❌ 不收 | ✅ 收 |
| 提供 spawn / 地址表 / 调度机械 | ✅ 提供 | ✅ 调用 |
| 做监督决策（restart / stop / escalate） | ❌ **不做** | ✅ 做 |

**关键判据：ActorSystem 没有邮箱。** 一旦它能收业务消息，它就成了全局单点——所有请求排一条队，
单线程编排的收益全丢。

监督树里确实有一个根 actor，但它是 **ActorSystem 创建的第一个 actor**，不是 ActorSystem 本身：

```
ActorSystem（运行时：调度、地址表、spawn 机械）
 └─ /user guardian                              ← 框架创建的第一个 actor
     ├─ RoomShardSupervisor[shard 0..N]         ← 框架提供：容器 + 监督 + 空闲回收
     │   ├─ RoomActor[1001]                     ← 业务实例，父 = shard supervisor
     │   └─ RoomActor[1002]
     └─ PlayerShardSupervisor[shard 0..N]
         └─ PlayerActor[9527]
```

**每个 actor 都有父，包括顶层 actor**（顶层的父是 `/user` guardian）。
不要用"无父裸 spawn"（Erlang 的 `spawn/1` 那种）——崩了没人知道。

### 5.2 三种 manager 角色必须严格分开

混在一起是这类设计最典型的翻车方式：

| 角色 | 谁写 | 职责 | 明确不做 |
|---|---|---|---|
| **ShardSupervisor**（分片容器） | **框架** | get-or-activate、监督子 actor、空闲回收、限额 | ❌ 不做业务校验 |
| **Coordinator**（协调者） | 业务 | 跨多 actor 的创建流程：协调、补偿、回滚 | ❌ 不负责单实例激活 |
| **Domain supervisor** | 业务或框架 | 配额/限流策略（"每个玩家最多 3 个房间"） | ❌ 不做单实例创建 |

先例：Akka Cluster Sharding 的结构是 `ShardRegion → Shard → Entity`，**entity 是 `Shard` actor 的子 actor**，
由 `Shard` 负责"不存在就创建"。也就是说负责 spawn 的父 actor 是**框架提供的分片容器**，不是业务监督者，
更不是 ActorSystem。

### 5.3 实例键放哪：不要从 body 里提取

三个方案：

| 方案 | 形态 | 防"调用方忘了填" | 路由层能用 | `.proto` 可移植 |
|---|---|---|---|---|
| A. 调用方显式传 | `stub.SubmitBid(ctx, auctionID, req, cb)` | 靠签名逼 | ✅ | ✅ |
| **B. 声明在请求消息字段上** | `uint64 auction_id = 1 [(actor.entity_key) = true];` | ✅ 生成器自动填 header | ✅ | ⚠️ 需 extension |
| C. 键进路径 | `/actor/Auction/1001/SubmitBid` | ✅ | ✅ | ✅ |

**推荐 B。** 理由：键必须在**寻址层**（header），不能在 body 里，因为：

1. **路由层不能解析 payload。** 参考实现的 RouterAgent 明确不 Unmarshal 业务 body。键在 body 里意味着
   这套设计**只能在单进程内用**——而最需要声明式键的恰恰是跨节点。
2. 多一次反序列化，且位置不对（解包是业务语义操作）。
3. **header/body 双源会导致漂移**：参考实现已经在 `Header.ClientRoleID` 与 `req.PlayerId` 上各填一份，
   无人校验一致性。键如果也在 body 里，这个隐患翻倍。

方案 B 让生成器同时产出两侧代码：

- **发送侧**：`Header.EntityKey = req.GetAuctionId()`，自动填充 → 不可能与 body 不一致
- **接收侧**：Dispatcher 用 **header** 里的键路由（不解析 body）；handler 收到后由生成代码断言
  `header.EntityKey == req.GetAuctionId()`，开发期不匹配直接报错

> 精确说法：从 body 取键在**纯单进程 actor** 下可接受（反正要解包），只在要求路由层保持
> payload-agnostic 时才是错的。既然做 actor 多半要跨节点，就不要选一个将来必须改的位置。

### 5.4 创建语义：不要 `spawn` 标志

**去掉 `option (actor.spawn) = true`。** 正确形态是"**键先由上游权威分配好，作为普通消息投给该键的地址，
不存在就自动激活**"。

先例：虚拟 actor（Orleans / Restate / Kalix）都是"键由调用方选，首次触碰即激活"，或"上游分配 ID"。
参考实现也已经是这个形状：`matchsvr`（权威编排者）分配 `assignment_id` / `room_id`，
再调 `roomsvr.CreateRoom(...)`——**ID 先有，再调目标**。

按创建的性质分三种归属：

| 创建的性质 | 谁处理 | 例子 |
|---|---|---|
| 只需"分配 ID + 激活一个实例" | **框架自动激活**，不需要"创建"入口 | 上游给 `auctionID` → `GetOrActivate(key)` |
| 需要**跨多 actor 协调**（多步、可能回滚） | **Coordinator actor** | 建房：冻结银币 + 占席位 + 选房间节点 |
| 需要**配额/限流/回收策略** | **Domain supervisor** | 每玩家最多 3 个拍卖 |

激活流程（框架内部）：`Tell` 到不存在的实例 → 按 `Kind` 找本节点 shard supervisor（按 key 哈希选分片）
→ 投递 `Activate{Key}` → supervisor spawn 实例（父 = supervisor）→ 把原消息投进去。

```go
// 业务只面对地址，不面对系统
sys.Tell(actor.Addr{Kind: KindAuction, Key: req.GetAuctionId()}, func(a AuctionActor) {
	a.CreateAuction(ctx, req, corerpc.ReplyProto[*CreateAuctionRsp](cb))
})
```

**闭包形式只在单进程成立**（闭包不可序列化）。跨节点必须退回 `(route, payload)`：

```
跨节点：Core.Call(targetNode, route, payload, cb)
          → 对端 Dispatcher.Dispatch(route, …) → 反序列化 → sys.Tell(addr, fn) 落到实例
```

两级寻址边界很清晰：**路由层只选节点、不解析 payload；actor 层选实例、才解 payload。**

### 5.5 执行流归属

| 规则 | 说明 |
|---|---|
| **一个 actor 只能有一个执行流** | 要么主循环调度，要么每 actor 一 goroutine。**注册时定死，运行期不能混** |
| 调度模式按 actor 类选 | 数量少、活短 → 挂主循环（复用 `Notify` + 单轮预算）；数量多或活会阻塞 → 每 actor 一 goroutine |
| 单轮预算在两种模式下都必要 | 主循环模式下，一个 actor 占久了拖所有人 |
| **`Dispatcher` 绝不直接调实现** | 否则状态所有权当场破。它只能 `Tell` |

**只读查询的优化要谨慎。** 常见诱惑是给查询开"绕过邮箱直接读状态"的后门——只有在 actor 对外发布
**不可变快照**（copy-on-write）时才安全，否则就是跨 goroutine 读可变状态。参考实现的 room snapshot
（"按查看者裁剪的揭示等级"）已经是这个形态，值得借鉴：**快照可以无锁读，状态只有 actor 能碰。**

### 5.6 重入与死锁

- **在 actor 的 handler 里绝不能同步等自己**：A 的 handler 里 RPC 调 B，B 又调 A → A 的邮箱被占，死锁。
- 跨 actor 一律**异步 + 回调**（§4.7 的编排助手）。
- 如果某条链路需要"等",那它就不是 actor 边界内的事——把它移到非 actor 的边界上（HTTP handler、
  连接协程），那里有真正的栈可以等。

### 5.7 延迟答复

handler 可能要先问第三方（DB、别的 actor）才能答，此时**不能占着执行流等**。

```go
func (a *AuctionActor) SubmitBid(ctx corerpc.Ctx, req *SubmitBidReq, reply corerpc.Reply[*SubmitBidRsp]) {
	if a.loaded {
		reply(a.compute(req), nil)
		return
	}
	a.deferred = ctx.DeferredSubmitBid()          // 把回信权交出去
	dbsvr.AskLoad(self, a.id, func(d *Data, me MechErr) {
		defer func() { a.deferred = nil }()
		if me != nil { a.deferred.Fail(me); return }
		a.load(d)
		a.deferred.Resolve(a.compute(req))        // 这里才真正回信
	})
	return                                        // 返回"我稍后答"
}
```

四条契约（缺一条就会出诡异 bug）：

1. **句柄一次性**。第二次 `Resolve` 是编程错误（重复回信会打乱对方的 pending 表）。
2. **对方可能已经超时**。此时 `Resolve` 是 no-op，只记 debug 日志——**不是错误**，超时是请求方的私事。
3. **句柄会泄漏**。业务忘了结清就永久悬着。孤儿句柄表要有清理周期，但**按"可观测性从模块侧做"的原则，
   这个观测应由持有句柄的模块自己做**，不是框架在回调边界上反推。
4. **关服时统一结清**所有未结清句柄，否则 `Shutdown` 会等一批永远不会来的答复。

> 参考实现只有 `OnceReply`（防重复回包），**没有延迟答复的专用句柄**——业务只能自己存
> `reply func([]byte, error)`，容易忘清理。这是可以做得比它好的地方。

### 5.8 空闲回收（passivation）

实例数 × 每实例状态是内存的主要来源，回收策略比 spawn 重要得多。需要定死：

- **回收条件**：空闲时长 / 无在途请求 / 无定时任务。
- **回收时的在途请求**：必须先把邮箱排空再回收，或把在途请求以明确错误终结。**不能静默丢**。
- **重建成本**：从 DB 重建 vs 从快照重建；重建期间到达的消息要排队（不要丢）。
- **回收与分片的交互**：回收是 shard supervisor 的职责，不是业务 actor 的。

---

## 6. 取消

**这是唯一"gRPC 有一等公民、参考实现没有、而游戏服是刚需"的一块。**

### 6.1 gRPC 的形态（可直接映射）

| gRPC | 语义 |
|---|---|
| `ClientContext::TryCancel()` | 发起方取消，**幂等**（可重复调） |
| `ServerContext::IsCancelled()` | 服务端**轮询**感知取消 |
| `ServerContext::AsyncNotifyWhenDone(tag)` | 取消/完成时投递一个 tag，用于清理被放弃的调用 |

### 6.2 我们的三个取消触发源

| 触发源 | 语义 | 后果 |
|---|---|---|
| **连接断开** | 玩家/节点失联 | 该连接发起的在途请求全部以明确错误终结（**不能只丢回包**） |
| **实体销毁** | actor 被显式终止或回收 | 该实例的在途请求以明确错误终结或重投 |
| **deadline 到期** | 请求方放弃 | 通知对端"可以停了"（尽力而为，不保证对端真停） |

### 6.3 不变量

1. **取消是幂等的**（重复取消不报错）。
2. **取消必须有终态**：被取消的请求要以明确错误回调，不能悬着。
3. **取消是尽力而为**：取消消息可能不到达；业务对不可幂等操作仍必须靠幂等键兜住。
4. **`stale` 护栏不等价于取消**：`stale` 只阻止**发起新调用**，不撤销在途。

> 参考实现用"`ctx.Stale()` + 业务幂等"替代取消：玩家离线后不发新请求，但**已发出**的请求继续执行副作用、
> 回包到达后丢弃。对"扣钱/发货"这类操作，客户端看不到回包但服务端已生效——它靠业务侧的幂等键
> （`request_id`、`client_nonce`、结算回执）兜住。
> **这是一个自洽的设计选择，但它是一个必须显式声明的缺口**，否则业务会假设"断线了服务端就会停"。

---

## 7. IDL 与代码生成

### 7.1 声明位

```proto
// proto/actor/actor.proto —— 运行时注解，独立于业务 proto
// 注意：非本工具链的消费者会忽略这些注解，看到的是普通单例 service（语义失真）
extend google.protobuf.ServiceOptions {
  bool   keyed_entity = 51001;   // 本 service 有实例：一个 key 一个实例
  string entity_type  = 51002;   // 实例类型名，缺省取 service 名
}
extend google.protobuf.FieldOptions {
  bool entity_key = 51003;       // 本字段是实例键
}
```

```proto
// proto/actor/auction.proto
service AuctionActor {
  option (msg.common.kind)        = BACKEND;
  option (msg.common.server_type) = ST_ROOMSVR;   // 哪个进程宿主它（用于生成 RegisterAll）
  option (actor.keyed_entity)     = true;         // 只声明"有实例"，不声明键在哪
}

message SubmitBidReq {
  uint64 auction_id = 1 [(actor.entity_key) = true];   // 键声明在字段上
  uint64 bid        = 2;
}

message CreateAuctionReq {
  uint64 auction_id = 1 [(actor.entity_key) = true];   // 键已由上游分配
  uint64 seller_id  = 2;
}

message AuctionClosedNtf {
  uint64 auction_id = 1 [(actor.entity_key) = true];
}
```

单向 vs 请求响应**由返回类型 + 命名后缀共同决定，生成器强校验**：

```
输入 _Ntf 且 returns google.protobuf.Empty  → Send（单向）
输入 _Req 且 returns *_Rsp                  → Call
其他组合                                     → 生成失败（不是静默降级）
```

服务名后缀同样强制：BACKEND 必须 `Remote`/`Actor` 结尾，FRONTEND 必须 `Handler` 结尾。

### 7.2 生成物

```go
// proto/gen/actor/auctionactor/auction_actor.go
const (
	RouteAuctionActorSubmitBid = "AuctionActor/SubmitBid"
)

type AuctionActor interface {
	SubmitBid(ctx corerpc.Ctx, req *SubmitBidReq, reply corerpc.Reply[*SubmitBidRsp])
	AuctionClosed(ctx corerpc.Ctx, ntf *AuctionClosedNtf)   // 单向：无 reply
}

// 注册的是"怎么找/建 actor"，不是 actor 实例 —— 这是与普通 RPC 生成最大的差别
func RegisterAuctionActor(d *corerpc.Dispatcher, sys *actor.System, newFn func(AuctionKey) AuctionActor) {
	d.MustRegister(RouteAuctionActorSubmitBid, corerpc.RecoverRoute(RouteAuctionActorSubmitBid,
		func(ctx corerpc.Ctx, body []byte, reply func([]byte, error)) error {
			var req SubmitBidReq
			if err := proto.Unmarshal(body, &req); err != nil {
				return errcode.New(errcode.ERR_UNMARSHAL, err.Error())
			}
			if got := ctx.EntityKey(); got != AuctionKey(req.GetAuctionId()) {
				return errcode.New(errcode.ERR_INTERNAL, "entity key mismatch")   // 生成代码，开发期断言
			}
			return sys.Tell(actor.Addr{Kind: KindAuction, Key: AuctionKey(req.GetAuctionId())}, newFn,
				func(a AuctionActor) { a.SubmitBid(ctx, &req, corerpc.ReplyProto[*SubmitBidRsp](reply)) })
		}))
}
```

**生成期检查清单**（缺一条就会退化成运行期静默故障）：

1. service 名在 kind 内**唯一**（否则同包重复定义）
2. `entity_key` 字段存在且是标量（或复合键的明确组合）
3. `keyed_entity` 的 service 里，**每个 Call 方法必须有且仅有一个 `entity_key` 字段**
4. 单向方法必须返回 `google.protobuf.Empty`，且入参以 `_Ntf` 结尾
5. route 常量与 Register 的键来自同一处

### 7.3 装配：用位置参数构造函数保证编译期完备

多个 actor 类 / 多个 service 装配时，手写 N 行 `Register` 的问题是**漏一个编译照样过**。

```go
// proto/gen/actor/registry.go —— 生成物
type RoomsvrActors struct {
	auction func(AuctionKey) AuctionActor
	room    func(RoomKey) RoomActor
}

// 一个类一个位置参数：新增一个类 → 签名变化 → 所有宿主进程编译报错
func NewRoomsvrActors(
	auction func(AuctionKey) AuctionActor,
	room    func(RoomKey) RoomActor,
) RoomsvrActors { ... }

func RegisterAllRoomsvrActors(d *corerpc.Dispatcher, sys *actor.System, r RoomsvrActors) { ... }
```

宿主进程一行：

```go
actor.RegisterAllRoomsvrActors(dispatcher, sys,
	actor.NewRoomsvrActors(auction.New, room.New))
```

### 7.4 参考实现里不能照抄的三处

| 不抄 | 事实 | 应该 |
|---|---|---|
| **源码正则解析** | `protoc-gen-svcstub` 手里有 descriptor，却仍 `os.ReadFile(f.Desc.Path())` + 正则 + 手搓 `matchBrace` | 全部从 `protogen` 的 descriptor 读。副作用：它把"预生成 descriptor 再离线生成"这条路堵死了 |
| **手工隐形注入** | `genrpc.Init(core)` 漏调编译通过，首次调用才返回 `ERR_INTERNAL "rpc stub not initialized"` | 构造期必填注入，不提供"core 为 nil 也能编译"的状态 |
| **可选 Poster** | `WithPoster` 只出现在测试里 | 构造函数必填参数 |

**这三处是同一个病根**：把"必须接线"做成了可选。参考实现在 `taskqueue` 里做对了（接口上不存在阻塞方法），
在 RPC 层做错了。**凡是"必须由使用者记得做"的接线，都要问能不能让它不可表达。**

---

## 8. 不变量清单

改设计前先过一遍。这些不是风格偏好，是**正确性前提**。

| # | 不变量 | 违反后的现象 |
|---|---|---|
| 1 | 每个请求**恰有一个终态** | 回调跑两次，或永远不回 |
| 2 | deadline **只能缩短**，且逐跳扣减 | 跨跳重新计时，总时长无上界 |
| 3 | **取消幂等**，且被取消者有终态 | 请求悬着；或重复取消报错 |
| 4 | **一个 actor 只有一个执行流** | 私有状态失去意义，退化成到处加锁 |
| 5 | **ActorSystem 没有邮箱** | 全局单点，所有请求排一条队 |
| 6 | 实例地址是 **`(Kind, Key)` 二元组** | 不同类的相同 key 值串类 |
| 7 | **不存在的实例/节点必须有明确终态** | 调用方只能等超时，把配置错误误读成对端慢 |
| 8 | **必须接线的东西不可省略** | 漏了不报错，运行期才以诡异现象出现 |
| 9 | **完成回调的投递路径不可阻塞** | 只在停服 teardown 暴露的死锁 |
| 10 | **谁创建 goroutine，谁在自己的 `Shutdown` 中 join** | 停服排空永不收敛 |
| 11 | `Shutdown` 不等"需要主循环执行的回调" | 自锁 |
| 12 | 跨节点相关性 ID **带发送方命名空间** | 多节点下回包投错来源，且 protobuf 不报错（静默串话） |
| 13 | Build 之后**不再注册模块**；actor 由宿主 Module 记账 | 与"模块集合冻结"冲突，停服关不干净 |

---

## 9. 明确不做（及其理由）

| 不做 | 理由 |
|---|---|
| 四种流式（含 bidi） | 推送走连接层（单向通知）；传大快照用"分片 + 通知"比流式简单 |
| 插件化 Resolver / LB 策略 | etcd + 一致性哈希 + 直连 + 广播四档够用 |
| mTLS / 凭证体系 | 内网 + 网关鉴权；先把"网关重写不可信字段"做对 |
| 结构化错误详情（`google.rpc.Status` 那套 Any details） | 起步只需码 + 服务端日志 |
| hedging（并发多发取最快） | 需要幂等且会放大负载，游戏服收益低 |
| **自动重试** | 重试是策略，不是机制。机制只保证"事实可判"（§4.5） |
| 广播的聚合语义 | 起步只做"首答胜出"，其余当迟到丢弃；**必须写进契约** |

**"不做"必须写进契约。** gRPC 最值得学的一点是它把"不做什么"也定义得很清楚（例如 `wait_for_ready=false`
时 `UNAVAILABLE` 的语义是规范写死的）。反例是参考实现：目标节点不存在时静默无响应，业务无从判断。

---

## 10. 落地路线

每个阶段都能独立验证；**每阶段都要能证伪某个假设**，不能证伪的阶段说明切错了。

| 阶段 | 内容 | 证伪什么 |
|---|---|---|
| **P0** | `pkg/taskqueue`：`Post`/`PostNonBlocking` 双路径 + 类型强制的 `NonBlockingPoster` + `Drain` 的"`ran < limit` ⇒ 空"契约 + 先入队后 Notify | 双路径是否真的能消除 teardown 死锁 |
| **P0** | 预算演进：`Waker` → `Proc(limit) int`，框架给上限（`WithProcBatch`） | "框架给上限"是否比"模块自定"更好用 |
| **P1** | `Core`（单线程版：seq + 单 map + 扫描挂 `Update` + `Close` 排空 + **Poster 构造期必填**） | 单线程是否真的让分片/锁/独立 goroutine 全部多余 |
| **P1** | `Dispatcher` + 失败语义四件套 + `Ctx` + 错误码（生成而非手抄） | 失败分类是否够业务判断重试 |
| **P2** | **单进程 actor：零 IDL 改动**。每个逻辑实体一个邮箱 + 一个执行流，实例路由手写 `map[key]` | **"每实例一个执行流"这个假设成不成立**——如果某个类的活会阻塞，它就不该挂主循环，这会影响 P5 |
| **P3** | 取消（§6） | 断线/销毁/deadline 三源能否统一收口 |
| **P4** | 可观测：在途数 / 队列水位 / 超时数 / 路由表自省 / trace id 串联 | 这一层最容易欠债，晚做代价最高 |
| **P5** | 跨节点寻址 + `actor.proto` 注解 + 生成器契约 | 键在 header 的形态是否真能让路由层保持 payload-agnostic |

**P2 是关键里程碑**：它在**不动 IDL** 的前提下验证 actor 化最根本的假设。P5 的生成器设计正确与否，取决于
P2 的结论——所以**不要先做生成器**。

---

## 11. 参考实现对照表

| 件 | 判断 | 说明 |
|---|---|---|
| `pkg/taskqueue` 的双投递路径 + 类型强制 | ✅ **整套搬** | 225 行，收益最大 |
| `Core` 的 seq / 扫描超时 / `Close` 排空 / dispatch 回投 | ✅ **搬语义，不搬并发结构** | 16 分片 + 锁 + `sync.Pool` 在 breeze 单线程下多余 |
| `Dispatcher` + `OnceReply` + `RecoverRoute` + `ReplyProto[T]` | ✅ **整套搬** | 125 行，把失败语义从业务手里收走 |
| `Ctx` 的不可变链式 + `stale` 护栏 | ✅ **搬** | 职责划分干净 |
| `compose` 的 `Join`/`Each`/`Seq` | ✅ **搬** | 免掉自造 Future |
| 错误码是值 + `CodeOf` 归一化 | ✅ **搬语义** | ❌ 不搬"proto 与 Go 两份手抄 + 370 行 `String()` switch"——应从 proto 生成 |
| 生成链的"接口 + route 常量 + Register"三件套 | ✅ **搬形态** | 核心价值是**漏实现 = 编译错误** |
| `Target` 的链式目标（`At`/`ByHash`/`Broadcast`/`Timeout`，返回副本） | ✅ **搬** | 不可变 + 调用点短 |
| 单向/双向的命名后缀强校验 | ✅ **搬** | 不合规直接生成失败 |
| `Poster` 做成 Option | ❌ **反例** | 两个生产构造点都漏传 |
| `genrpc.Init` 手工注入 | ❌ **反例** | 漏调编译通过，全量 RPC 静默降级 |
| 默认超时 120s 且被网关转发复用 | ❌ **反例** | 打到死节点要干等两分钟；超时应按 route 可配 |
| 断线不 fail-fast | ❌ **反例** | 在途只能等满 deadline；"对端慢"与"链路断"表现相同 |
| 目标节点缺失 = 静默黑洞 | ❌ **反例** | 应产生明确机制错误 |
| 单向通知零保证（marshal/发送错误被丢弃） | ❌ **反例** | 至少要让业务知道它会丢 |
| 跨 RA 相关性 ID 无命名空间 | ❌ **反例** | 多节点静默串话 |
| 生成器读源码做正则解析 | ❌ **反例** | 有 descriptor 就该用 |
| 设计文档与注释滞后于实现 | ❌ **反例** | 解释"为什么这里安全"的错误注释比缺注释更危险 |

---

## 12. 待定问题

以下问题本文没有结论，实现到对应阶段前必须定死：

1. **passivation 策略**：空闲阈值多少、回收时在途请求怎么收口、重建期间的消息排队深度。内存主要来源。
2. **跨节点 actor 地址与 `ServerType`/`ByHash` 的拼接**：`(Kind, Key)` 如何映射到 `Target.ByHash` 的 key；
   一致性哈希的 key 用 `Kind+Key` 还是只用 `Key`（前者更均匀，后者让同类实例聚集）。
3. **广播语义**：目前只做"首答胜出"。若业务需要"聚合全部"，要有独立的 API 而不是改变 `Broadcast` 的语义。
4. **单向通知的可靠性**：是否需要"至少一次"档位，以及谁负责对账。
5. **错误码生成**：从 `proto/common/errcode.proto` 生成 Go 常量与 `String()`，消除双份手抄。
6. **取消的传播深度**：取消是否要穿透多层（A 取消 → B 取消它对 C 的调用）；穿透需要每一跳都记账。
