# CLAUDE.md

本文件是 `src/core/app/` 的局部索引。改进程骨架、模块接口或生命周期顺序前，先读本文件。

## 上级入口

- [../../../CLAUDE.md](../../../CLAUDE.md)
- [../CLAUDE.md](../CLAUDE.md)

## 目录定位

- Go package：`app`，import 路径 `project/src/core/app`。
- 进程级骨架：模块注册 → 依赖拓扑排序 → 生命周期编排（含失败回滚）→ **主循环**。
- 只做编排：不监听信号、不做 RPC 装配，只依赖标准库。`app` 包**不 import `os/signal`**；
  信号由各 `src/server/*svr` 的 `main.go` 用 `src/core/sigctrl` 翻译成 `Ctrl` 后经
  `WithCtrl` 注入。`Run` 拿到 ctx 与这个控制源，两者谁先到都走同一条退出路径。

## 主要文件

- [`app.go`](app.go) — `App` 结构体、`New`、`Option`、`Build`、`Run`，以及 `depsSort` 依赖排序
- [`module.go`](module.go) — `Module` 接口与两个可选能力接口 `Updater` / `Reloader`
- [`lifecycle.go`](lifecycle.go) — `lifecycleState` 与 Init/Start/Stop/Shutdown 及回滚
- [`loop.go`](loop.go) — 主循环 `runLoop`、控制事件 `Ctrl` / `handleCtrl`、
  `Post` / `Notify`、能力广播（`modulesUpdateAll` / `modulesReloadAll` / `modulesWakeAll`）
- [`errors.go`](errors.go) — 哨兵错误与错误串组装助手

## 主循环

- `Run` 里 `<-ctx.Done()` 已换成 `a.runLoop(ctx)`，循环体只做四件事：等 ctx、
  等控制事件、等唤醒、等周期更新。平时零 CPU 阻塞在 select 上。
- **循环跑在 `Run` 自己的 goroutine 上，不是另起一条**。于是循环返回之后 Run 才
  开始 Stop，`Update` / `Reload` 与 `Stop` / `Shutdown` 在时间上不可能重叠，
  `App.mu` 的用法一个字不用改。**不要为了"让 Run 早点返回"把循环挪进 goroutine** ——
  那会让 `Update` 与 `Shutdown` 并发，整套锁约定立刻不够用。
- 四类输入：
  - `ctx.Done()` 与 `CtrlStop` 语义相同，先到先得；
  - `WithCtrl(ch)` 注入的外部控制源（sigctrl 把信号翻译成的通道）。框架**只读不关**，
    读到关闭就把该 case 置 nil 摘掉 —— 不摘会读到 `Ctrl(0)` 零值空转烧 CPU，
    这也是 `Ctrl` 从 1 开始编号的原因；
  - `wakeCh`：`Notify()` 写、主循环读的**信号**通道，容量 1、不承载数据。
    收到就调 `modulesWakeAll` 跑一遍 Waker 模块。**必须与 `ctrlCh` 分开** ——
    `ctrlCh` 承载停服/热更这类丢了会出事的消息，`wakeCh` 是可以随意合并、丢弃的
    信号，两者后果量级不同（`Post` 满时是丢的）；
  - `WithUpdateInterval(d)` 的周期更新，`d <= 0` 关闭。没有模块实现 `Updater` 时
    框架自动不开 ticker。
- **一轮真干过活就要自己补 token**：`modulesWakeAll` 返回 true（还有模块报积压）时
  循环自己 `Notify()` 一次。select 已经把进来那个 token 消费掉了，不补的话剩下的
  积压要等到下一次有人投递才动 —— 表现为偶发延迟。用 `Notify()` 而不是 `continue`，
  是为了让控制事件和周期更新在这期间仍有机会被选中。
- `Post(c)` 是唯一可从任意 goroutine 投递控制事件的口，非阻塞、可能丢弃。
  `Update` / `Reload` / `Wake` 内部要停服只能用 `Post(CtrlStop)`，**禁止直接调
  `Stop()` / `Shutdown()` / `Run()`** —— 那会从模块回调节点递归引爆停服流程。
- `Update` / `Reload` / `Wake` 必须短、不阻塞。需要阻塞的活留在模块自有 goroutine 里
  （`smoke` 包的 `beat` 是那个形状）。
- 已知副作用：ctx 取消那一刻若 select 恰好选中了别的就绪分支，会多跑一轮。
  它发生在 `Stop` 之前所以安全，但**测试不能断言"cancel 之后零 update"**。
- `updateCount` 是**周期更新跑过的次数**，从 1 开始。自增点在 `case now := <-updateCh` 里，
  **不是循环顶部** —— 它数的不是"while 转了几圈"。这样两次 `Update` 之间穿插多少事件
  都不影响 updateCount 的连续性，模块按 `updateCount % N` 分频才有意义，`updateCount × 更新间隔` 也才近似
  等于运行时长。（这个区别由 `TestUpdateCountNotBumpedByEvents` 钉住：数轮次的话那两次
  更新之间隔着一次唤醒，会得到 1、3 而不是 1、2。）
- **updateCount 只作为 `Update(now, updateCount)` 的参数交付，App 上没有查询方法**。自起 goroutine
  的模块不在主循环的节奏上（随时会被别的轮次推走），读了只会误导；那种模块要计数请
  自己记。**参数是唯一入口** —— 拿不到参数就拿不到 updateCount。
- 它仍**不是精确的时钟**：单轮超时时 `time.Ticker` 会丢 tick，updateCount 会落后于时间。
  要精确的时间差用 `Update` 收到的 `now`。

## 没有运行时健康告警

框架**不做任何运行时告警**。两个后果要清楚：

- 主循环**卡在某个回调里时没有任何东西能发现** —— 它自己发现不了（select 还没回去），
  也没有外部 goroutine 盯着。唯一的兜底是 sigctrl 的二次信号强杀（人工按第二次 Ctrl+C）。
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
type Updater  interface { Update(now time.Time, updateCount uint64) }
type Reloader interface { Reload() error }
type Waker    interface { Wake() bool }
```

三个接口都**不与 `Module` 耦合**：能力接口只描述能力本身，模块自己把「是 Module」和
「能被主循环驱动」两个身份组合起来。这是 Go 的惯例 —— 接口越小抽象越强，而
`interface { Module; Update() }` 是个 9 方法的大接口，它想表达的只有一件事。

触发方式：`Updater` 每轮周期被调，`Waker` 只在有人 `Notify()` 之后被调，
`Reloader` 只在收到 `CtrlReload` 时被调。

**`Waker` 单轮处理多少完全由模块自己定，框架不给上限。** 代价要清楚：这等于把「这一轮
占多久」的决定权交给模块，占久了拖的是所有人（包括控制事件与停服信号的响应延迟）——
主循环单线程，没有别的 worker 替别人吸收（对比 skynet：它敢让批量正比于积压，是因为有
多 worker + 服务间让出）。

**框架不做任何兜底**（没有上限、没有告警，见上节）。所以自定上界是 `Waker` 契约的一部分，
不是可以随便填的数 —— 写 `Wake` 时得自己掂量：`smoke/inbox` 自定 8 条就是这个原因。

`Build` 阶段用类型断言把实现能力的模块筛成**冻结的子集**（`modulesUpdater` /
`modulesReloader` / `modulesWaker`），主循环只遍历这几个容器，不再每轮扫全表。

容器装的是 **`[]Module` 而不是能力接口**：它的语义就是「模块表的一个子集」，能力只是筛选
条件。于是日志（`namesOf`）与将来自省都能直接取名字，代价只是热路径上一句
`m.(Updater).Update(now, updateCount)` —— 不带 `ok`，因为容器只由 Build 里那段筛选产出，必然成立。
（那次断言实测 146ns/秒量级，不构成选型理由；选 `[]Module` 是为了语义与简洁，
不是为了快。）

那三段筛选**在 `Build` 里就地写开、不抽成包级泛型助手**：Go 的泛型函数不能是闭包，
抽出去只能放包级，反而把「它只服务于 Build」这件事藏起来了。加第四个能力时照抄一段即可。

「谁会被更新」与「要不要开 ticker」（`len(modulesUpdater) > 0`）于是在同一处一次定死 ——
不会出现「`modulesUpdateAll` 按每轮现算、ticker 按 Build 快照」那种半冻结。代价是运行期新增的
能力模块不会被自动拾取，而那本来就是「Build 之后模块集合冻结」这条约定的应有之义。

热路径那三句不带 `ok` 的断言（`m.(Updater)` / `m.(Reloader)` / `m.(Waker)`）是这套设计**唯一的代价**：
容器的正确性由 Build 保证，类型上不再表达。改动 Build 里那段筛选时要意识到这一点。

实现它们是**可选能力**：不实现 `Update` 的模块完全不受影响，也不用写空方法。
Build 里顺手把筛出来的模块名打进启动日志 —— 那是 duck typing 唯一失效模式
（方法名写错导致静默不收集）的防线，模块没出现在那行里就是没被收到。

顺序 = 拓扑序 = Init 顺序。更新取正向（与 Init 同向，数据流向下游），
热更也取正向（配置提供者先换新，消费者再用新的）；只有 Stop/Shutdown 是逆序。

**不要改成显式注册**（如包级 `RegisterTick` 或宏）。类型断言让"实现了就一定被收集"由
编译器保证，没有中间态；显式注册一旦漏写就是静默失效。

框架**不在循环里 recover**：`Init` / `Start` 也不 recover，循环里破例只会掩盖状态不一致。

## 快速读法

- 查装配与校验看 `Build`：工厂在此才被调用，重名/缺依赖/成环都在这里报，
  能力列表也在这里收集。
- 查生命周期看 `Init` / `Start` / `Stop` / `Shutdown`。
- 查主循环、控制事件、能力广播看 [`loop.go`](loop.go)。
- 查停服预算与看门狗看 [`lifecycle.go`](lifecycle.go) 的 `shutdownOne`。
- 查启动失败怎么收拾看 `rollbackInit`（Init 失败）与 `rollbackStart`（Start 失败）。
- 查"为什么顺序是这样"看 [`app.go`](app.go) 的 `depsSort`，以及 `app_test.go` 里的顺序断言。

## 工作规则

- **`App` 是具体结构体，不是接口**。生命周期只有这一种实现，模块集合的差异由各 svr
  注册的 `Module` 决定。接口只用在有真实多实现的地方 —— 本包里的那一处是 `Module`。
  不要为 `App` 抽接口，也不要加 `I` 前缀。
- **绝不可以在持有 `App.mu` 的情况下调用模块方法**：模块回调里会调
  `App().GetModule(...)`，那会重新加锁并自锁。加锁只用于读写 `state` / `modulesArr` / `modulesMap`。
- **只有 `lifecycleState` 一个生命周期枚举，没有逐模块状态**。模块状态是由 `lifecycleState` 加上
  "Init 全部跑完才开始 Start"这条不变量推出来的：已启动的必然是 `modulesArr[0, i)` 前缀，
  只 Init 未启动的必然是 `(i, n)` 后缀。**若将来改成边 Init 边 Start 的交错生命周期，
  这个前提就不成立，回滚必须改回按模块记账** —— `lifecycle.go` 的 `rollbackStart`
  注释里也写明了这一点。
- **Start 返回错误的模块，框架不得对它调用 Stop/Shutdown**。`Module.Start` 的注释约定
  该模块返回错误前必须自清理，再补一刀会二次释放。`app_test.go` 的
  `TestStartFailureRollback` 钉住了这条。
- **关机分两趟**：所有模块先逆序收到 `Stop` 信号，再逆序 `Shutdown` 等待。正常路径
  （`Stop()` 然后 `Shutdown()`）与失败回滚都是这个形状，总耗时取决于最慢的模块而不是
  各模块之和。不要改成逐模块 `Stop+Shutdown`。
- **看门狗不破坏"两趟关机"**：`Shutdown` 仍严格逆序，`shutdownOne` 只给单次等待加上界；
  超预算的模块被记名放弃，`Shutdown()` 返回 `ErrShutdownTimeout`。它**不杀 goroutine** ——
  「超时」只是「不再等」，被放弃模块仍在跑，随进程退出被带走。预算在停服阶段开始时算一次、
  所有模块共享，所以逆序位置靠后的模块会替最前面卡住的买单（这是换「总退出时间有上界」
  的代价）。`WithShutdownTimeout(d <= 0)` 表示不设上限，退回死等。
- **交给模块的预算 ctx 必须从 `Background` 派生，绝不能从 `a.ctx` 派生**。`a.ctx` 在一切
  Shutdown 之前就已 cancel（见下一条），基于它派生出来的 ctx 一开门就是 done 的，
  ctx 感知的模块会立刻放弃清理 —— 兜底反而变成了默认行为，比不传 ctx 还糟。
  模块因此同时有两条信息：`App().Context()`（进程正在退出）与参数 ctx（还剩多少预算）。
  由 `TestShutdownReceivesLiveCtx` 钉住。
- **`Shutdown` 必须能处理"只 Init 过、从未 Start"的状态**，因为 Init 失败与 Start 失败
  都会走到这里。
- **一切 `Shutdown` 调用之前，进程 ctx 必须已经 cancel**。正常路径靠 `Stop()` 末尾，
  **回滚路径（`rollbackInit` / `rollbackStart`）必须自己先调 `a.cancel()`** ——
  这里曾经漏过，后果是模块的 `Shutdown()` 等一个永远不会 done 的 ctx，直接卡死。
  三条路径分别由 `TestStopCancelsContextBeforeShutdown` /
  `TestInitRollbackCancelsContext` / `TestStartRollbackCancelsContext` 钉住。
  **新增任何调用 `Shutdown` 的路径，都要先确认 cancel 已经发生。**
- 顺序语义（"无依赖按注册顺序初始化"、"Stop/Shutdown 严格逆序"）靠测试固定，
  改 `depsSort` 或回滚顺序时必须同步 `app_test.go` 的断言。
- `Module` 接口的签名与注释由调用方约定，改动前先确认所有 `*svr` 模块实现。
