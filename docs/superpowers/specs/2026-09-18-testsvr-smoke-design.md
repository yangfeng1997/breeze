# testsvr 冒烟服务设计

日期：2026-09-18

## 背景

`src/core/app` 是进程骨架：模块注册 → 依赖拓扑排序 → 生命周期编排（含失败回滚）。
它有 16 个测试钉住契约，但**全仓库至今没有任何 `main` 包**——`src/server/` 下 10 个
svr 目录全是空的，框架从未在真实进程里跑起来过。

`src/server/testsvr` 是第一个服务进程。它的产出不只是"能跑"，更是后面 9 个 svr 的
抄写模板：进程怎么起、信号怎么接、模块怎么切、goroutine 怎么收，都以它为准。

## 目标

1. 把 `app` 的生命周期在一个真实进程里完整跑通：
   `Build → Init → Start → 等信号 → Stop → Shutdown`。
2. 验证三条现有单测覆盖不到的东西：
   - 真实进程里 Ctrl+C 能触发优雅退出；
   - 模块能通过 `App.GetModule` 拿到兄弟模块（该方法目前除测试外零调用方）；
   - 模块的 goroutine 在 `Shutdown` 返回时确已退出。
3. 定下模板：`main.go` 只做装配，模块放 `internal/` 子包，一个模块一个文件。

## 非目标

- **不接配置**：`run/` 目录、`scripts/config.py`、`scripts/build.py`、`startall.sh`
  目前都不存在（只在 `scrpits/setup_windows_env.ps1` 的说明里被提到）。等那套工具出现
  再谈 testsvr 如何参与。
- **不对外提供服务**：不开监听端口、不接 RPC、不碰 protobuf。
- **不做日志配置**：直接用 `app.New` 默认的 `slog.Default()`，`main` 里不写日志代码。
- **不做故障注入**：启动失败的回滚路径已由 `app` 包的 `TestInitFailureRollback` 与
  `TestStartFailureRollback` 钉死，冒烟服务不重复。

## 目录结构

```
src/server/testsvr/
  main.go                     // 装配与信号，约 25 行
  internal/smoke/
    smoke.go                  // beat 骨架（三个模块共用）
    ticker.go                 // ticker 模块
    worker.go                 // worker 模块
    heartbeat.go              // heartbeat 模块
    smoke_test.go             // 两个用例
```

用 `internal/` 是故意的：这三个模块只给本进程用，外部包 import 会直接编译报错。

## main.go

```go
func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	a := app.New("testsvr")
	a.Register(smoke.NewWorker)     // 注册序 0 —— 依赖 ticker
	a.Register(smoke.NewHeartbeat)  // 注册序 1 —— 无依赖
	a.Register(smoke.NewTicker)     // 注册序 2 —— 无依赖

	if err := a.Run(ctx); err != nil {
		slog.Error("testsvr 启动失败", "err", err)
		os.Exit(1)
	}
}
```

要点：

- 信号由 `main` 提供，框架本身不监听信号——这是 `app` 包 CLAUDE.md 里写明的分工。
- 工厂返回值直接就是 `func(*app.App) app.Module`，`Register` 直接收，不需要包装。
- `Run` 返回 `nil` 表示正常退出（收到信号），`main` 自然返回即退出码 0。

## 注册顺序与拓扑序

`modulesArr` 按**注册序**建立，`depsSort` 再排出**执行序**。上面的注册顺序会产出：

| 模块 | 注册序 | 拓扑序 | 依赖 |
|---|---|---|---|
| worker | 0 | 2 | ticker |
| heartbeat | 1 | 0 | 无 |
| ticker | 2 | 1 | 无 |

于是日志里能看到：

- Init 顺序 `heartbeat → ticker → worker`——worker 注册在最前（序号 0）却排到最后，
  被依赖压住了，这是"有依赖会强制重排"；
- heartbeat 与 ticker 互不依赖，**保持彼此的注册先后**，这是"无依赖保持注册序"；
- 退出时严格逆序：`worker → ticker → heartbeat`。

这两条语义是方案选型时特意要覆盖的（原方案只演示前一条）。

**这个注册顺序是试出来的，不是推出来的。** `depsSort` 每步只在"当前已就绪"的节点里
挑注册序号最小的那个，并不是把几条链做稳定归并。本设计第一版按后者推导，预测 Init
顺序为 `ticker → heartbeat → worker`，实跑得到的是 `ticker → worker → heartbeat`：
ticker 出队后 worker 立刻变成就绪，而它的序号 0 比 heartbeat 的 2 小，于是插到了
heartbeat 前面——"worker 排到最后"根本没发生。改成上表这个注册顺序才两条都演示到。

## beat 骨架

三个模块共用 `beat`，它实现"后台定时打点 + 可被叫停 + 可被等待"这个形状。

```go
type beat struct {
	app      *app.App
	name     string
	step     time.Duration
	extra    func() []any    // 可选：追加到每轮日志行的字段
	done     chan struct{}   // 构造函数里创建，Stop 关闭
	exited   chan struct{}   // goroutine 退出前关闭，供测试断言
	stopOnce sync.Once
	wg       sync.WaitGroup
	mu       sync.Mutex
	n        int
}
```

`Start` 起一个 goroutine：

```go
func (b *beat) Start() error {
	b.wg.Add(1)
	go func() {
		defer b.wg.Done()
		defer close(b.exited)
		t := time.NewTicker(b.step)
		defer t.Stop()
		for {
			select {
			case <-b.done:                  // 主路径：Stop 发出的退出信号
				return
			case <-b.app.Context().Done():  // 兜底：框架在 Stop 末尾 cancel
				return
			case <-t.C:
				b.tick()
			}
		}
	}()
	return nil
}

func (b *beat) Stop()     { b.stopOnce.Do(func() { close(b.done) }) }
func (b *beat) Shutdown() { b.wg.Wait() }
```

打点本身也放在 `beat` 上，模块只能往里加字段，不能换掉这个动作：

```go
func (b *beat) tick() {
	b.mu.Lock()
	b.n++
	n := b.n
	b.mu.Unlock()

	attrs := []any{"module", b.name, "n", n}
	if b.extra != nil {
		attrs = append(attrs, b.extra()...)
	}
	b.app.Logger().Info("tick", attrs...)
}
```

要点：

- **`extra` 这个钩子的存在是必需的，不是设计洁癖。** Go 没有虚方法：`beat.Start`
  里的 `b.tick()` 是静态派发，`b` 的静态类型就是 `*beat`。worker 就算定义了自己的
  `tick()`，那个方法也**永远不会被调到**——它只是把 `beat.tick` 遮住，而 goroutine
  里跑的还是 `beat.tick`。要让模块改变打点行为，只能靠函数字段，不能靠"重写方法"。
- **`<-b.done` 是主路径，`App().Context()` 只做兜底。** `app.Context()` 的文档写明
  "模块不得依赖它完成自身的资源清理"——它是在 Stop 阶段末尾、所有模块都收到 Stop 之后
  才被 cancel 的。
- **`stopOnce` 让 `Stop` 真正幂等。** 框架的正常路径只会调一次，但这是要被抄 9 遍的
  模板，`close` 一个已关闭的 channel 会 panic，值得挡一道。
- **`exited` 的 defer 顺序有讲究**：`close(b.exited)` 注册在 `wg.Done()` 之后，
  LIFO 使其先执行。于是 `Shutdown`（`wg.Wait()`）返回时 `exited` 必然已关闭，
  测试可以非阻塞地断言，不需要 sleep。
- `done` 在构造函数里创建而非 `Start` 里，这样 `Stop` 早于 `Start` 调用也不会 panic。

`beat` 提供的默认实现：`Name` / `App` 取值，`DependsOn` 返回 `nil`，`Init` 返回 `nil`。

构造函数签名：

```go
func newBeat(a *app.App, name string, step time.Duration, extra func() []any) beat
```

## 模块的构造：两层

每个模块都有**两个**构造函数，这是为了测试能用上真实装配路径：

```go
// ticker.go —— 内部构造，周期可指定
func newTicker(a *app.App, step time.Duration) *ticker {
	return &ticker{beat: newBeat(a, "ticker", step, nil)}
}

// 导出工厂，把生产周期写死，签名正好是 app.Register 要的
func NewTicker(a *app.App) app.Module { return newTicker(a, 500*time.Millisecond) }
```

测试调 `newTicker(pa, 5*time.Millisecond)`——**走的还是生产那条构造路径，只是周期不同**。
测试若自己搓 `&ticker{beat{...}}` 字面量，就会和生产装配漂移：worker 的 `extra` 钩子
漏掉也不会有人发现，测试照样绿。

## 三个模块

类型全部不导出，只导出工厂函数；`main` 只需要工厂，拿不到也不该拿到具体类型。

### ticker（`ticker.go`）

- 生产周期 `500ms`，`name = "ticker"`，`extra` 传 `nil`
- 额外提供 `count() int`：带锁读 `n`

### heartbeat（`heartbeat.go`）

- 生产周期 `700ms`，`name = "heartbeat"`，`extra` 传 `nil`
- 与 ticker 周期错开，日志里好分辨

### worker（`worker.go`）

- 生产周期 `500ms`，`name = "worker"`
- `DependsOn() → []string{"ticker"}`
- 构造时挂上 `extra` 钩子，把 ticker 的计数并进每轮日志：

```go
func newWorker(a *app.App, step time.Duration) *worker {
	w := &worker{}
	w.beat = newBeat(a, "worker", step, func() []any {
		return []any{"ticker", w.ticker.count()}
	})
	return w
}
```

闭包捕获 `w` 本身，`w.ticker` 要等 `Init` 才赋值——但钩子只在 `Start` 之后
被 tick 调用，那时 `Init` 早已跑完；`Init` 失败则模块永不 `Start`，钩子不会被调到。

- `Init` 里按名取兄弟模块并做类型断言：

```go
func (w *worker) Init() error {
	m := w.app.GetModule("ticker")
	if m == nil {
		return fmt.Errorf("找不到依赖的 ticker 模块")
	}
	t, ok := m.(*ticker)
	if !ok {
		return fmt.Errorf("ticker 模块类型意外: %T", m)
	}
	w.ticker = t
	return nil
}
```

- 每轮日志因此形如 `msg=tick module=worker n=7 ticker=6`——ticker 那一列就是
  worker 从兄弟模块读到的实时计数

worker 这条最有价值：**它证明 worker 拿到的是同一个、已经 Init 过的实例**，
而不只是"顺序碰巧对了"。模块之间不互相 import，只通过 `App` 拿——这是真实模块
之间唯一的协作方式。

因为 `Init` 出错会让框架回滚且永不 `Start`，`Start` 之后 `w.ticker` 必然非 nil，
后续不需要再加防御性判空。

## 退出时序

Ctrl+C → `signal.NotifyContext` 让 ctx done → `Run` 走 `Stop` 再 `Shutdown`：

1. `Stop` 逆序调 `worker.Stop / heartbeat.Stop / ticker.Stop`，各自 `close(done)`；
2. 所有 `Stop` 走完，框架 `cancel()` 进程 ctx（兜底，防止有 goroutine 还 select 在它上面）；
3. `Shutdown` 逆序 `wg.Wait()`，等各自的 goroutine 真正退出。

两趟分离是 `app` 的既定设计：总耗时取决于最慢的模块，而不是各模块耗时之和。

## 错误处理

- **启动失败**（Build / Init / Start 任一步出错）：`Run` 直接返回该错误，框架已回滚完毕。
  `main` 打一行 `slog.Error` 后 `os.Exit(1)`。
- **运行期**：模块的 goroutine 不做错误处理——它只打点，没有会失败的操作。
- **退出期**：`Module.Stop` / `Module.Shutdown` 没有返回值，不存在错误传播。

## 测试（`internal/smoke/smoke_test.go`）

两个用例，都用**真实 `App`** 驱动，不造测试替身，也不为测试给生产代码开导出参数。

**测试调用内部的快周期构造函数**，不调把 500ms 写死的导出工厂（等不起）：

```go
a := app.New("smoke-test")
tk := newTicker(a, 5*time.Millisecond)
wk := newWorker(a, 5*time.Millisecond)
a.Register(func(*app.App) app.Module { return tk })
a.Register(func(*app.App) app.Module { return wk })
```

等待一律用**带 deadline 的轮询**，不用固定 `sleep`——固定睡眠在慢机器上会假失败：

```go
deadline := time.Now().Add(time.Second)
for tk.count() == 0 {
	if time.Now().After(deadline) {
		t.Fatal("ticker 一直没打点")
	}
	time.Sleep(time.Millisecond)
}
```

### `TestWorkerReadsTicker`

`New → Register(worker, ticker) → Build → Init → Start`，轮询等到 `tk.count() > 0`
后再睡几轮，断言 `wk.count() > 0`。钉住"依赖声明 → 拓扑序 → `GetModule` 拿到兄弟
实例"这条链路：worker 的 `Init` 若没能拿到 ticker，它会返回错误，`Build/Init` 直接
失败，用例根本走不到断言。

### `TestShutdownStopsGoroutines`

`Build → Init → Start`，按上面的方式轮询确认两个 goroutine 都真的在跑，再调
`Stop` + `Shutdown`，断言 `Shutdown` 返回时各模块的 `exited` 通道已关闭（非阻塞
`select`）。**先确认"在跑"再断言"停了"**，否则 goroutine 没起来也能让用例通过。

**这条是模板里最容易抄错的**：`Shutdown` 只 `close(done)` 而不 `wg.Wait()` 就是
goroutine 泄漏——进程退出可能看不出问题，但真实模块会丢数据、丢连接。

## 验收标准

1. `go build ./...`、`go vet ./...`、`gofmt -l .` 全部干净。
2. `go test ./...` 全绿，`app` 包原有 16 个用例不受影响。
3. 启动后日志中 Init 顺序为 `heartbeat → ticker → worker`，且能看到三者的打点日志
   交错输出，其中 worker 那行带 `ticker=N` 且 N 在增长。
4. 运行中按 Ctrl+C，进程退出码为 0，且退出日志的顺序为
   `worker → ticker → heartbeat`（Stop 与 Shutdown 各一遍逆序）。
5. 粘住退出后进程不残留（无 goroutine 泄漏导致的挂起）。

## 已知限制

- `syscall.SIGTERM` 在 Windows 上收不到（Ctrl+C 走的是 `os.Interrupt`）。保留它是为了
  将来跑 Linux 时不用改代码，不是现在就能验证的路径。
- 没有退出超时兜底：`Module.Shutdown` 没有 ctx 参数，模块卡住时没有任何东西能取消它。
  `app` 包的注释已记录这一点，等真实模块出现后再补。
- testsvr 不参与 `startall.sh` 那套启动链路（该脚本尚不存在）。
