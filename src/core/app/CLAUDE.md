# CLAUDE.md

本文件是 `src/core/app/` 的局部索引。改进程骨架、模块接口或生命周期顺序前，先读本文件。

## 上级入口

- [../../../CLAUDE.md](../../../CLAUDE.md)
- [../CLAUDE.md](../CLAUDE.md)

## 目录定位

- Go package：`app`，import 路径 `project/src/core/app`。
- 进程骨架：模块注册 → 依赖拓扑排序 → 启动编排（Init → Start）→ **主循环**。
- **只管启动，不管停机**。没有 `Stop` / `Shutdown`，没有超时预算，没有信号处理，
  也不 import `os/signal`。`Run` 阻塞在调用方给的 ctx 上，ctx 结束就返回；
  之后的事（关连接、存档、释放资源）全是调用方的责任，框架一个字都不做。
  代价必须清楚：**已 Start 的模块没有任何人叫停** —— 它们要么自己盯着
  `App.Context()`，要么随进程退出被带走。
- 只做编排：不监听信号、不做 RPC 装配，只依赖标准库。

## 主要文件

- [`app.go`](app.go) — `App` 结构体、`New`、`Option`、`Build`、`Run`，以及 `depsSort` 依赖排序
- [`module.go`](module.go) — `Module` 接口与两个可选能力接口 `Tickable` / `Waker`
- [`lifecycle.go`](lifecycle.go) — `lifecycleState` 与 `Init` / `Start`
- [`loop.go`](loop.go) — 主循环 `runLoop`、`Notify`、能力广播（`modulesTickAll` / `modulesWakeAll`）
- [`errors.go`](errors.go) — 哨兵错误与错误串组装助手

## 启动失败不回收

`Init` 与 `Start` 遇错都只是**返回错误**，不碰任何模块：

- 第 n 个模块失败时，`[0, n)` 已 Init（或已 Start）的模块收不到任何通知，框架也不释放
  它们占用的资源。没有 `rollbackInit` / `rollbackStart`，没有逐模块账本。
- `Module.Start` 的注释约定：**返回错误的那个模块必须自清理** —— 它可能已经起了
  goroutine 或开了监听，框架不会替它收尾。
- `App.Context()` 的 cancel 在 `Run` 的 `defer` 里，所以启动失败那条路 ctx 一样会 done。

这是刻意的简化：启动阶段只有"往前走"一种状态推进。补偿代码只在启动失败时跑一次，
永远缺少真实执行路径，改起来也没有测试盖得住 —— 与其维护它，不如把"失败就退出"
这条说清楚。

## 主循环

- `Run` 里就是 `a.runLoop(ctx)`。循环体只做三件事：等 ctx、等唤醒、等周期 tick。
  平时零 CPU 阻塞在 select 上。
- **循环跑在 `Run` 自己的 goroutine 上，不是另起一条**。于是循环返回之后 `Run` 立刻
  返回，`Update` / `Wake` 与调用方的任何收尾在时间上不可能重叠，`App.mu` 的用法
  一个字不用改。**不要为了"让 Run 早点返回"把循环挪进 goroutine** ——
  那会让 `Update` 与调用方的收尾并发，整套锁约定立刻不够用。
- 三类输入：
  - `ctx.Done()`：调用方给的取消源，**唯一的退出路径**；
  - `wakeCh`：`Notify()` 写、主循环读的**信号**通道，容量 1、不承载数据。
    收到就调 `modulesWakeAll` 跑一遍 Waker 模块。它是可以随意合并、丢弃的信号 ——
    多个生产者同时叫只会产生一个 token，漏叫的那次由下一轮补上（见下条）。
    **必须与"承载数据"的通道分开**：装数据的通道丢了会出事，信号丢了只是晚一轮。
  - `WithTickInterval(d)` 的周期 tick，`d <= 0` 关闭。没有模块实现 `Tickable` 时
    框架自动不开 ticker。
- **一轮真干过活就要自己补 token**：`modulesWakeAll` 返回 true（还有模块报积压）时
  循环自己 `Notify()` 一次。select 已经把进来那个 token 消费掉了，不补的话剩下的
  积压要等到下一次有人投递才动 —— 表现为偶发延迟。用 `Notify()` 而不是 `continue`，
  是为了让周期 tick 在这期间仍有机会被选中。
- `Update` / `Wake` 必须短、不阻塞。需要阻塞的活留在模块自有 goroutine 里
  （`smoke` 包的 `beat` 是那个形状）。
- 已知副作用：ctx 取消那一刻若 select 恰好选中了别的就绪分支，会多跑一轮。
  它发生在 `Run` 返回之前所以安全，但**测试不能断言"cancel 之后零 update"**。
- `tickCount` 是**周期 tick 跑过的次数**，从 1 开始。自增点在 `case now := <-tickCh` 里，
  **不是循环顶部** —— 它数的不是"while 转了几圈"。这样两次 `Tick` 之间穿插多少事件
  都不影响 tickCount 的连续性，模块按 `tickCount % N` 分频才有意义，`tickCount × tick 间隔` 也才近似
  等于运行时长。（这个区别由 `TestTickCountNotBumpedByEvents` 钉住：数轮次的话那两次
  tick 之间隔着一次唤醒，会得到 1、3 而不是 1、2。）
- **tickCount 只作为 `Tick(now, tickCount)` 的参数交付，App 上没有查询方法**。自起 goroutine
  的模块不在主循环的节奏上（随时会被别的轮次推走），读了只会误导；那种模块要计数请
  自己记。**参数是唯一入口** —— 拿不到参数就拿不到 tickCount。
- 它仍**不是精确的时钟**：单轮超时时 `time.Ticker` 会丢 tick，tickCount 会落后于时间。
  要精确的时间差用 `Tick` 收到的 `now`。

## 没有运行时健康告警

框架**不做任何运行时告警**。两个后果要清楚：

- 主循环**卡在某个回调里时没有任何东西能发现** —— 它自己发现不了（select 还没回去），
  也没有外部 goroutine 盯着。停机的兜底（二次信号强杀）在框架外，见下条。
- 模块**积压完全静默** —— 消费追不上生产时队列会一路涨到 OOM，日志里毫无痕迹。

这里曾经有两条告警，都已移除：

| | 曾用阈值 | 判据 |
|---|---|---|
| 卡住告警 | `WithStuckAfter`，30s | `runWork` 在每次回调前后写 `workStart`（atomic），另起 goroutine `watchLoop` 比对当前时间 |
| 过载告警 | `WithOverloadAfter`，10s | 某个模块是不是一直从 `Wake` 返回 `true` |

两者的共同问题是**判据都是二手信号** —— 一个从"回调没返回"推，一个从"Wake 报了还有"
推，都不是"慢"或"积压"本身。真要恢复可观测性，应该从模块侧做（它才知道自己队列多长、
哪一步慢），而不是让框架在回调边界上反推。

## 可选能力接口

```go
type Tickable interface { Tick(now time.Time, tickCount uint64) }
type Waker    interface { Wake() bool }
```

两个接口都**不与 `Module` 耦合**：能力接口只描述能力本身，模块自己把「是 Module」和
「能被主循环驱动」两个身份组合起来。这是 Go 的惯例 —— 接口越小抽象越强，而
`interface { Module; Update() }` 是个大接口，它想表达的只有一件事。

触发方式：`Tickable` 每轮周期被调，`Waker` 只在有人 `Notify()` 之后被调。

**`Waker` 单轮处理多少完全由模块自己定，框架不给上限。** 代价要清楚：这等于把「这一轮
占多久」的决定权交给模块，占久了拖的是所有人（包括周期 tick 与 ctx 取消的响应延迟）——
主循环单线程，没有别的 worker 替别人吸收（对比 skynet：它敢让批量正比于积压，是因为有
多 worker + 服务间让出）。

**框架不做任何兜底**（没有上限、没有告警，见上节）。所以自定上界是 `Waker` 契约的一部分，
不是可以随便填的数 —— 写 `Wake` 时得自己掂量：`smoke/inbox` 自定 8 条就是这个原因。

`Build` 阶段用类型断言把实现能力的模块筛成**冻结的子集**（`tickableModules` /
`wakeableModules`），主循环只遍历这几个容器，不再每轮扫全表。

容器装的是 **`[]Module` 而不是能力接口**：它的语义就是「模块表的一个子集」，能力只是筛选
条件。于是日志（`namesOf`）与将来自省都能直接取名字，代价只是热路径上一句
`m.(Tickable).Tick(now, tickCount)` —— 不带 `ok`，因为容器只由 Build 里那段筛选产出，必然成立。
（那次断言实测 146ns/秒量级，不构成选型理由；选 `[]Module` 是为了语义与简洁，
不是为了快。）

那两段筛选**在 `Build` 里就地写开、不抽成包级泛型助手**：Go 的泛型函数不能是闭包，
抽出去只能放包级，反而把「它只服务于 Build」这件事藏起来了。加第三个能力时照抄一段即可。

「谁会被 tick」与「要不要开 ticker」（`len(tickableModules) > 0`）于是在同一处一次定死 ——
不会出现「`modulesTickAll` 按每轮现算、ticker 按 Build 快照」那种半冻结。代价是运行期新增的
能力模块不会被自动拾取，而那本来就是「Build 之后模块集合冻结」这条约定的应有之义。

热路径那两句不带 `ok` 的断言（`m.(Tickable)` / `m.(Waker)`）是这套设计**唯一的代价**：
容器的正确性由 Build 保证，类型上不再表达。改动 Build 里那段筛选时要意识到这一点。

实现它们是**可选能力**：不实现 `Update` 的模块完全不受影响，也不用写空方法。
Build 里顺手把筛出来的模块名打进启动日志 —— 那是 duck typing 唯一失效模式
（方法名写错导致静默不收集）的防线，模块没出现在那行里就是没被收到。

顺序 = 拓扑序 = Init 顺序。tick 取正向（与 Init 同向，数据流向下游）。

**不要改成显式注册**（如包级 `RegisterTick` 或宏）。类型断言让"实现了就一定被收集"由
编译器保证，没有中间态；显式注册一旦漏写就是静默失效。

框架**不在循环里 recover**：`Init` / `Start` 也不 recover。

## 快速读法

- 查装配与校验看 `Build`：工厂在此才被调用，重名/缺依赖/成环都在这里报，
  能力列表也在这里收集。
- 查生命周期看 `Init` / `Start`。
- 查主循环、唤醒、能力广播看 [`loop.go`](loop.go)。
- 查"为什么顺序是这样"看 [`app.go`](app.go) 的 `depsSort`，以及 `app_test.go` 里的顺序断言。

## 工作规则

- **`App` 是具体结构体，不是接口**。生命周期只有这一种实现，模块集合的差异由各 svr
  注册的 `Module` 决定。接口只用在有真实多实现的地方 —— 本包里的那一处是 `Module`。
  不要为 `App` 抽接口，也不要加 `I` 前缀。
- **绝不可以在持有 `App.mu` 的情况下调用模块方法**：模块回调里会调
  `App().GetModule(...)`，那会重新加锁并自锁。加锁只用于读写 `state` / `modulesArr` / `modulesMap`。
- **只有 `lifecycleState` 一个生命周期枚举，没有逐模块状态**。模块状态是由 `lifecycleState`
  加上"Init 全部跑完才开始 Start"这条不变量推出来的。**若将来改成边 Init 边 Start 的
  交错生命周期**，这条前提就不再成立 —— 到时候若要加回回滚，必须改回按模块记账。
- **`Start` 返回错误的模块，框架对它不做任何事**。`Module.Start` 的注释约定该模块返回错误前
  必须自清理，框架再补一刀就是二次释放。`app_test.go` 的 `TestStartFailure` 钉住了这条。
- **停机是有意删掉的，不要顺手加回来。** 这里曾经有 `Stop` / `Shutdown` / 停服预算看门狗 /
  启动失败回滚 / `Ctrl` 控制事件通道 / `sigctrl` 信号包，整套已移除。要恢复其中任何一样，
  先回答：**它是框架职责还是模块职责？** 框架侧能做对的只有"顺序"，而顺序在启动阶段
  已经由拓扑排序给足了。收尾涉及具体资源（连接、文件、存档），那属于模块和装配层。
- **模块只能通过 `App.Context()` 感知进程正在退出**，且它是 `Run` 返回时才 cancel 的 ——
  比模块的 goroutine 活得短。不要指望它承载"还剩多少时间"，框架不给预算。
- 顺序语义（"无依赖按注册顺序初始化"）靠测试固定，改 `depsSort` 时必须同步
  `app_test.go` 的断言。
- `Module` 接口的签名与注释由调用方约定，改动前先确认所有 `*svr` 模块实现。
