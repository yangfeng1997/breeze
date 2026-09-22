# app 主循环设计

日期：2026-09-19

## 背景

`src/core/app` 此前是**纯串行编排器**：`Build → Init → Start → <-ctx.Done() → Stop → Shutdown`。
`app.go` 里那行裸阻塞是进程唯一的存活机制，框架一行 `go` 语句都没有，时间是**去中心化**的
—— 每个模块（`smoke.beat`）各起 goroutine、各持一个 `time.Ticker`。

三个具体问题：

1. **没有统一周期入口**。周期逻辑只能各自起 goroutine，无法保证顺序、无法统一观测、
   无法串行化。
2. **信号不可扩展**。`testsvr/main.go` 用 `signal.NotifyContext` 把所有信号压成一个
   ctx 取消，**不区分类型**；想加热更就得改框架或每个 main 各写一套。
3. **停服救不回来**。`Module.Shutdown()` 无 ctx、无超时兜底；一次卡住进程永久挂住，
   第二次 Ctrl+C 只是对已 cancel 的 ctx 再 cancel 一次，毫无作用。

## 目标

把裸阻塞换成**事件驱动 + 周期更新**的主循环，把信号与停服统一收进循环；周期更新与热更
做成可选能力接口；停服加上预算兜底。

**关键窗口**：`src/server/` 下另外 9 个 svr 目录全是空的，模块实现只有 `smoke` 的 4 个。
任何接口改动现在只波及少数文件，等模板被抄 9 遍就是 12+ 个。

## 非目标

- 不接配置系统，因此**配置热更只留接口与事件路由**，不实现重载。
- 不做代码热更：Go 的 `plugin` 包不支持 Windows 且加载后无法卸载。现实路径是重启式更新
  （`CtrlRestart` 已占位：优雅退出，由外部编排器拉起新进程）。
- 不引入队列 / 调度器：当前没有可调度的工作项，周期更新只做广播。
- 不改 `init/start` 的错序拒绝语义与回滚形状。

## 设计

### 主循环

新文件 `src/core/app/loop.go`，`Run` 里 `<-ctx.Done()` 换成 `a.runLoop(ctx)`：

```go
func (a *App) runLoop(ctx context.Context) {
	var updateCh <-chan time.Time
	if a.updateInterval > 0 && len(a.modulesUpdater) > 0 {
		t := time.NewTicker(a.updateInterval)
		defer t.Stop()
		updateCh = t.C // 没有 Updater 时保持 nil，该 case 永久阻塞，零空转
	}

	src := a.ctrlSource // 可能为 nil
	for ctx.Err() == nil {
		select {
		case <-ctx.Done():
			return

		case c := <-a.ctrlCh:
			if a.handleCtrl(c) {
				return
			}

		case c, ok := <-src:
			if !ok {
				src = nil // 来源关闭：摘掉这条 case，否则读到 Ctrl(0) 零值空转
				continue
			}
			if a.handleCtrl(c) {
				return
			}

		case <-a.wakeCh:
			if a.modulesWakeAll() {
				a.Notify() // 还有积压，自己补一个 token 立刻再转一圈
			}

		case now := <-updateCh:
			a.updateCount++ // updateCount 只在这里自增：它数的是"周期更新跑过多少次"
			a.modulesUpdateAll(now, a.updateCount)
		}
	}
}
```

**循环跑在 `Run` 自己的 goroutine 上，不另起一条** —— 这是本设计最要紧的一条。循环返回
之后 `Run` 才调 `Stop`，于是 `Update` / `Reload` 与 `Stop` / `Shutdown` 在时间上不可能重叠，
框架不需要为此加任何锁，「纯串行编排器」这个性质保住了。一旦把循环挪进 goroutine，
`OnTick` 式的回调就可能与 `Shutdown` 并发，整套锁约定立刻不够用。

**已知副作用**：`for ctx.Err() == nil` 与 select 里的 `ctx.Done()` 并存，ctx 取消那一刻
若 select 恰好选中了 update 分支，会多跑一轮。它发生在 `Stop` 之前所以安全，但**测试不能
断言「cancel 之后零 update」**。

### 更新计数（`updateCount`）

**它数的是「周期更新跑过多少次」，从 1 开始 —— 不是「while 转了几圈」。** 自增点因此
放在 `case now := <-updateCh` 里，而不是循环顶部。

这个区别是有后果的：若数轮次，两次 `Update` 之间穿插一次唤醒就会让 updateCount 从 1 跳到 3，
于是模块按 `updateCount % N` 做的分频会随事件多寡而变，形同虚设。
（`TestUpdateCountNotBumpedByEvents` 钉住这条 —— 数轮次的实现会让它得到 `[1 3]`。）

**它只作为 `Update(now, updateCount)` 的参数交付，App 上没有查询方法。** 这是刻意的：
自起 goroutine 的模块不在主循环的节奏上（它随时会被别的轮次推走），读一个共享的 updateCount
只会误导；那种模块要计数请自己记。**参数是唯一入口** —— 拿不到参数就拿不到 updateCount，
于是"只在主循环中用"由类型系统保证，而不是靠约定。

**它近似是时钟，但不精确。** `updateCount × 更新间隔 ≈ 运行时长` 成立的前提是每一轮都跟得上；
单轮超时时 `time.Ticker` 会丢 tick，updateCount 就落后于时间。要精确的时间差请用 `Update`
收到的 `now`。

（NFShmXFrame 的帧号与时间更贴合，是因为它额外做了**补足休眠**（`sleepTime =
m_nFrameTime - cost`）让帧长恒定 —— 那部分与事件驱动冲突，见下节。）

### 要不要固定帧（NFShmXFrame 的 `m_bFixedFrame`）

结论：**不采用**。"固定帧"其实是四样东西捆在一起，只有一样真冲突：

| 组成部分 | 状态 |
|---|---|
| 固定节拍（每 X ms 触发） | **已有** —— `time.Ticker` 就是干这个的，Go 文档明确说它会调间隔或丢 tick 来补偿慢接收者 |
| 更新计数 / 逻辑时钟 | **已加**（见上）。`updateCount × 更新间隔 ≈ 运行时长`，但丢 tick 时会落后 |
| 单轮耗时告警（NFShmXFrame 的 `cost > 40ms` 警告） | **没有** —— 曾有 `WithStuckAfter`，已移除（见下） |
| **帧长必须花满**（`sleepTime = m_nFrameTime - cost`） | ← **真冲突** |

最后一条才是固定帧的本质：它要的是"这一帧无论如何占满 33ms"。代价是循环必须定期醒来、
空闲时也在跑 —— 而 `wakeCh` / `Notify()` 的全部意义就是"没事别转"。**两者不可兼得。**

**为什么不值得兼得**：固定帧的收益（把逻辑时间与真实时间解耦成可预测的节拍）只在
**有消费者**时才兑现 —— 帧同步（lockstep）、确定性回放、战斗录像。那是 `battlesvr`
那类服务的需求，而现在 `src/server/` 下只有 testsvr 有代码。等真要做帧同步时再设计，
那时需求是具体的；现在做，多半是照搬一个用不上的机制。

**将来若真要**，可以三者兼得（帧边界固定 + 帧内事件低延迟 + 空闲零 CPU）：

```go
for ctx.Err() == nil {
	deadline := time.Now().Add(a.updateCount)

	// 帧内：有事件立刻处理，没事件就睡到帧末
	for {
		remain := time.Until(deadline)
		if remain <= 0 {
			break
		}
		t := time.NewTimer(remain)
		select {
		case <-ctx.Done():
			t.Stop()
			return
		case c := <-a.ctrlCh:
			t.Stop()
			a.handleCtrl(c)
		case <-a.wakeCh:
			t.Stop()
			a.modulesWakeAll()
		case <-t.C: // 到点，进帧末
		}
	}

	a.modulesUpdateAll(time.Now(), 0) // 帧末跑周期更新（updateCount 这里先略）
}
```

代价是多一层循环 + 每轮新建 `Timer`。值不值得，取决于 battlesvr 到底要不要确定性模拟。

### 可选能力接口

```go
type Updater  interface { Update(now time.Time, updateCount uint64) }
type Reloader interface { Reload() error }
type Waker    interface { Wake() bool }
```

三个接口都**不与 `Module` 耦合**：能力接口只描述能力本身，模块自己把「是 Module」和
「能被主循环驱动」两个身份组合起来。Go 的惯例是接口越小抽象越强，而
`interface { Module; Update() }` 是个 9 方法的大接口，它想表达的只有一件事。

触发方式：`Updater` 每轮周期被调，`Waker` 只在有人 `Notify()` 之后被调，
`Reloader` 只在收到 `CtrlReload` 时被调。

`Build` 阶段用类型断言把实现能力的模块筛成**冻结的子集**（`modulesUpdater` /
`modulesReloader` / `modulesWaker`），主循环只遍历这几个容器。那几段筛选在 `Build` 里就地写开、
不抽成包级泛型助手 —— Go 的泛型函数不能是闭包，抽出去只能放包级，反而把
「它只服务于 Build」这件事藏起来；代价是加第四个能力时要照抄一段。

容器装的是 **`[]Module` 而不是能力接口**：语义上它就是「模块表的一个子集」，能力只是
筛选条件。于是启动日志（`namesOf`）与将来自省都能直接取名字；代价是热路径上的
`m.(Updater).Update(now, updateCount)` 不带 `ok` —— 容器的正确性由 Build 里那段筛选保证，类型上不再表达。
这是本次设计里唯一一处「靠不变量而非类型」的地方，改 Build 时要记得。

「谁会被更新」与「要不要开 ticker」（`len(modulesUpdater) > 0`）于是同一处一次定死，
不会出现「`modulesUpdateAll` 每轮现算、ticker 按 Build 快照」那种半冻结。代价是运行期新增的
能力模块不会被自动拾取 —— 那是「Build 之后模块集合冻结」这条约定的应有之义。

实现 `Update` / `Reload` 仍是可选能力：不实现的模块完全不受影响，也不用写空方法。

（选型时评估过"不缓存、每轮遍历 `modulesArr` 现断言"——实测 32 模块一轮 41.7ns vs 容器版
27.1ns，差 14.6ns/轮，10Hz 下即 146ns/秒，都是零分配。所以容器**不是为了性能**，
是为了让"谁能被更新"成为一份冻结的、可检视的东西。同理，容器存 `[]Module` 而非能力接口
带来的那句断言，也不是性能问题。）

**命名**：Go 官方风格规定单方法接口用「方法名 + `-er`」（`io.Reader` / `http.Handler`）。
`Tick` 走这条路会撞标准库的 `time.Ticker`，`Tickable` 是被迫的退路；`Update` → `Updater`
完全合规。裸动词（而非 `OnXxx`）则与现有 `Module` 的 `Init` / `Start` / `Stop` / `Shutdown`
保持一致 —— 那个接口里没有一个 `On` 前缀。

**为什么不显式注册**：NFShmXFrame 的 `REGISTER_MODULE_TICK` 宏靠人记得写，忘了就静默失效
（实测 92 个模块里 15 个 tick，另有一批实现了 `Tick()` 却从未被调用的死代码）。
类型断言让「实现了就一定被收集」由编译器保证，没有中间态。

**防线**：duck typing 唯一的失效模式是方法名写错。启动日志里的
`modulesUpdater=[...] modulesReloader=[...]` 就是这道防线 —— 模块没出现在里面，就是没被收到。

### 更新周期

全局一个 `WithUpdateInterval(d)`，**框架默认 100ms（10Hz）**；`testsvr` 显式设成
`time.Second/30`（30Hz）。注意是 `time.Second/30` 而不是 `33*time.Millisecond` ——
后者是 NFShmXFrame 的整数除法写法（`1000/30`），30 帧只覆盖 990ms，实际帧率是 30.3。
`d <= 0` 关闭周期更新。
**不支持每模块周期**：单个 `time.Ticker` 撑不起它，要换成「每模块 deadline + 每轮取最早的
去 Reset」，循环从 5 行变 40 行，且调用顺序不再等于拓扑序。真实需求模块自己一行就够：
`if n%10 != 0 { return }`。扩展点已留好（把 `updateCh` 换成 `Timer` + 每轮重算即可）。

### 控制事件

```go
type Ctrl uint8
const (
	CtrlStop    Ctrl = iota + 1 // 停服
	CtrlReload                  // 热更
	CtrlRestart                 // 重启式更新（代码更新的现实路径）
)

func (a *App) Post(c Ctrl) bool
func WithCtrl(ch <-chan Ctrl) Option
```

`Ctrl` 从 1 开始编号是刻意的：零值非法，于是万一有来源把已关闭的通道接进来，读到的零值
不会被误当成停服。

`Post` 非阻塞、缓冲满时丢弃。它同时是**测试确定性的关键**：用例让模块在 `Update` 里自己
`Post(CtrlStop)`，整个用例同步跑完，不需要起 goroutine 跑 `Run` 再轮询（那会让测试里无锁的
`recorder` 变成跨 goroutine 写入），零 sleep、零真实信号。

`Post` 也是模块内部请求停服的**唯一**正当手段：`Update` / `Reload` / `Wake` 里禁止直接调
`Stop()` / `Shutdown()` / `Run()`，那会从模块回调节点递归引爆停服流程。

### 事件驱动（Waker）

第三类能力接口，让模块把事件投到主循环上串行处理，而不是各起 goroutine：

```go
type Waker interface {
	// 单轮处理多少完全由模块自己定，框架不给上限。
	// 返回 true 表示还有积压，App 会立刻再调度一轮。
	Wake() bool
}
```

模块自持队列（v1 用一个带缓冲的 `chan` 就够），投递方入队后叫一次 `App.Notify()`。

**`wakeCh` 必须与 `ctrlCh` 分开**：`ctrlCh` 承载停服/热更这类丢了会出事的消息，
`wakeCh` 是容量 1、不承载数据、可随意合并丢弃的信号。`Post` 在缓冲满时是**丢**的 ——
丢一个 wake 无害，丢一个 `CtrlStop` 是事故。

**一轮真干过活就必须自己补 token**：select 已经把进来那个 token 消费掉了，
`modulesWakeAll` 返回 true 时循环自己 `Notify()` 一次，否则剩下的积压要等下一次有人投递才动
——表现为偶发延迟，最难查的一类 bug。用 `Notify()` 而不是 `continue`，是为了让控制事件
与周期更新在这期间仍有机会被选中，繁忙模块不至于把它们饿死。

**为什么返回值是 `bool` 而不是"处理了几条"**：框架拿到条数没法可靠判断是否干完，只能靠
"是不是吃满了上限"猜 —— 恰好处理满但队列刚好空了时会假阳性，白转一圈。`bool` 是模块
**明确回答**"还有没有"。

**单轮处理多少，框架不给上限。** 要清楚代价：这等于把「这一轮占多久」的决定权交给模块，
占久了拖的是**所有人**，包括控制事件与停服信号的响应延迟 —— 主循环单线程，没有别的
worker 替别人吸收（对比 skynet：它敢让批量正比于积压，是因为有多 worker + 服务间让出）。

**框架这时只剩一个事后告警**：卡住看门狗（`WithStuckAfter`，默认 30s）。它不阻止任何事，
只让你知道出事了。所以自定上界是 `Waker` 契约的一部分，不是可以随便填的数 ——
`smoke/inbox` 取 8（而非 256）就是这个原因。

（历史：曾经有过 `Wake(limit int) bool`，由 `WithWakeBudget` 给所有模块一个统一的硬上限。
去掉了 —— 上界该由知道"自己一条要处理多久"的模块来定，框架不知道。）

<details>
<summary>为什么不抄 skynet 的动态批量</summary>

skynet 一轮处理 `n = skynet_mq_length(q) >> worker_weight` 条（`skynet_server.c:352`），
weight=0 时就是**不限量**。它敢这么做，是因为有两个 breeze 没有的杠杆：

1. **多 worker 线程** —— A 占住一个，其他 worker 继续服务别人
2. **服务间让出** —— 一轮做完只要全局队列还有别人，`skynet_globalmq_push(q)` 把自己
   放回队尾（`skynet_server.c:373`）

单线程下这两个都不存在，抄过来直接退化成无上界。

而且"动态"本身不用抄：`min(limit, 积压)` 是 drain 循环的自然行为 —— 队列空了就
`default` 提前返回，等不到 limit。

**值得抄的是它的可观测性**（见下节）：死循环监控。
</details>

### 被移除的两条告警

框架**现在不做任何运行时健康告警**。两个后果：

- 主循环**卡在某个回调里时没有任何东西能发现** —— 它自己发现不了（select 还没回去），
  也没有外部 goroutine 盯着。唯一兜底是 sigctrl 的二次信号强杀（人工按第二次 Ctrl+C）。
  这也是"二次强杀要放在循环外"的第二个理由（第一个是循环卡住时收不到信号）。
- **积压完全静默** —— 消费追不上生产时队列会一路涨到 OOM，日志里毫无痕迹。

历史上这里有过两条：

| | 曾用阈值 | 判据 | 为什么是二手信号 |
|---|---|---|---|
| 卡住告警 | `WithStuckAfter`，30s | `runWork` 在每次回调前后写 `workStart`（atomic），另起 goroutine `watchLoop` 比对当前时间 | 从"回调没返回"推，不是"慢"本身 |
| 过载告警 | `WithOverloadAfter`，10s | 某个模块是不是一直从 `Wake` 返回 `true` | 从"Wake 报了还有"推，不是队列长度本身 |

要恢复可观测性，应该从**模块侧**做 —— 它才知道自己队列多长、哪一步慢 —— 而不是让框架
在回调边界上反推。

### 信号层

新包 `src/core/sigctrl`，`app` 包保持不 import `os/signal` 的分工。各 svr 的 main 只做两件事：

```go
ctrl, stop := sigctrl.Notify()
defer stop()
a := app.New("testsvr", app.WithCtrl(ctrl), ...)
```

- **Linux**：`SIGINT`/`SIGTERM` → 停服，`SIGHUP`/`SIGUSR1` → 热更。
- **非 Linux**：只有 `os.Interrupt`/`SIGTERM` → 停服；热更没有信号可用
  （`SIGUSR1` / `SIGHUP` 在 Windows 的 `syscall` 常量表里根本不存在），留待调试端口。

**二次信号强杀放在信号 goroutine 里，不放在主循环里**：循环若正卡在某个 `Reload` / 慢
`Update` 里，它自己永远读不到第二个信号；放在循环外就一定生效。第二次停服信号 → 打印一行
→ `exit(1)`（用 1 而非 0，让编排器能区分优雅退出与强杀）。

**main 里不要再叠加 `signal.NotifyContext`**：两套信号处理会抢第一个信号，且 `NotifyContext`
在首次信号后就撤掉自己，二次强杀那条兜底就没人管了。

### 停服预算

`Module.Shutdown()` → `Shutdown(ctx context.Context)`，App 侧再加全局看门狗
（`WithShutdownTimeout`，默认 10s）。两件都做：看门狗保证框架不挂住，给 ctx 则让模块有机会
自己有序收尾（刷盘、回包）。

**核心陷阱**：传给 `Shutdown` 的 ctx 必须从 `Background` 派生，**绝不能从 `a.ctx` 派生**。
`a.ctx` 在一切 Shutdown 之前就已 cancel（框架硬不变量），基于它派生出来的 ctx 一开门就是
done 的，ctx 感知的模块会立刻放弃清理 —— 兜底反而变成了默认行为，比不传还糟。模块因此
同时有两条信息：`App().Context()`（进程正在退出）与参数 ctx（还剩多少预算）。

预算在停服阶段开始时算一次、所有模块共享，总退出时间因此有上界；代价是逆序位置靠后的模块
会替最前面卡住的买单。**看门狗不杀 goroutine**，超时只是「不再等」—— 被放弃模块仍在跑，
随进程退出被带走，这一条要写进注释，否则「超时」会被误读成「安全回收」。

**这不破坏「关机分两趟」**：`Stop` 仍是全部先发信号，`Shutdown` 仍严格逆序，看门狗只给单次
等待加上界，没有改成逐模块 `Stop+Shutdown`。

## 三种模块模板

三者不是替代关系，是按"活的形状"分的三种工具：

| 场景 | 用哪个 |
|---|---|
| 会阻塞或耗时不可控（网络、DB、文件、等锁） | 模块自有 goroutine（`beat`） |
| 短、非阻塞、**周期性**（超时扫描、统计聚合、批量 flush 触发） | `app.Updater` |
| 短、非阻塞、**事件驱动**，但要在主循环上串行处理 | `app.Waker` |
| 需要独立于全局节拍的频率 | `beat`（或 `Update` 内自己 `n%k`） |

判据两条，依次问：

1. **这次的活会不会阻塞或耗时不可控？** 会 → `beat`。
2. **是周期来还是事件来？** 周期 → `Updater`；事件 → `Waker`。

后两种跑在主循环上，好处是模块内部状态**天然免锁**（与 `Stop`/`Shutdown` 也串行）；
代价是一次卡住会同时拖住其他模块与停服响应 —— 这是协作式调度的固有代价。
`Waker` 相对 `Updater` 的额外好处是**没事件时完全不占主循环**（`Updater` 每 100ms
都会被调一次，哪怕队列是空的）。

`testsvr` 用两个演示模块把这三种模板都跑了一遍：`pulse`（`Updater`，零 goroutine）
和 `inbox`（`Waker`，零 goroutine，`Stop`/`Shutdown` 空实现）。注册为第 4、第 5 个模块，
拓扑序为 `heartbeat → ticker → worker → pulse → inbox`，
**前三者的顺序与首版钉住的完全一致**。

## 与 GameServer 调度器的关系

GameServer 的 `wakeCh` 与 breeze 的 `wakeCh` 是同一个设计（`chan struct{}`、容量 1、
不承载数据，只表示"可能有活"），但**其余部分不同**，因为 breeze 没有队列：

| | GameServer | breeze |
|---|---|---|
| 队列归谁 | App 持有（`Processor` 从自己的 `WakeableQueue` 取） | 模块自持（v1 一个 `chan`） |
| 唤醒后 | 扫全部 `Processor.Proc(limit)` | 扫全部 `Waker.Wake()` |
| 双路径（繁忙非阻塞检查控制事件 / 空闲阻塞） | **需要** —— 繁忙时 `continue` 不阻塞，控制信号会被饿死 | **不需要** —— 循环永远阻塞，且 Go 的 select 在多 case 就绪时随机选，不存在饿死 |
| 两层 recover | 有（一个 panic 不能打死整轮，因为一轮要跑几百个任务） | **没有** —— 与 `Init`/`Start` 一致 |

**将来如果引入真正的队列**（比如网络层投递的不是单个事件而是任务批），要重新考虑的是
recover：一条脏任务打死整个主循环的后果，比现在一个回调 panic 严重得多。

## 验收标准

1. `go build ./...`、`go vet ./...`、`gofmt -l .` 全部干净；`GOOS=linux go build ./...` 通过。
2. `go test ./...` 全绿，**`app` 包原有 16 个用例的断言不改一行**（只有 `Shutdown` 的签名
   从 `Shutdown()` 变成 `Shutdown(ctx)`）。
3. 启动日志里出现
   `app built ... modulesUpdater=[pulse] modulesReloader=[] modulesWaker=[inbox]`，
   且 Init 顺序为 `heartbeat → ticker → worker → pulse → inbox`。
4. 运行中按 Ctrl+C，进程退出码 0，`module stopped` 与 `module shutdown` 各一遍逆序
   （完整逆序 `pulse → worker → ticker → heartbeat`）。
5. 连按两次 Ctrl+C，进程立即退出（退出码 1）。
6. Linux 上 `kill -HUP <pid>` 触发一次热更调用。

## 已知限制

- **二次信号强杀与 `SIGHUP` 热更无法在 Windows 上验证**。Windows 没有 `SIGHUP`；
  也无法给另一进程发 Ctrl+C（`kill` 在 Git Bash 下发的不是控制台事件），
  详见 `plans/2026-09-18-testsvr-smoke.md` 里 `GenerateConsoleCtrlEvent` 的失败记录。
  这两条只能人工在真实终端或 Linux 上确认。
- **主循环卡住时没有任何自动恢复**，只有告警。看门狗只报日志，不停服也不杀 goroutine ——
  这是刻意的（合法慢回调不该被误杀），代价是卡死时必须人工介入。`Shutdown` 的预算
  看门狗管的是停服阶段，管不了运行期。
- **`Wake` 里 panic 会打死整个主循环**（框架不在循环里 recover，与 `Init`/`Start` 一致）。
  这是当前设计与 GameServer 的主要差别，见「与 GameServer 调度器的关系」。
- **停服超时只是「不再等」**，不杀 goroutine；被放弃的模块会活到进程退出。
  治本手段是让模块实现尊重传入的 ctx。
- **热更没有回滚**。部分生效的配置回滚不了，停服也解决不了它；框架只记日志、继续调用
  其余模块。这是刻意的：活着的进程必须继续服务。
- **能力容器在 Build 阶段冻结**，只被主循环 goroutine 读，故不加锁。一旦支持运行期增删
  模块，必须补锁或改成不可变快照，并同步决定容器是否跟着变（现在两者是同一处定死的）。
