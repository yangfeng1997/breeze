package app

import (
	"context"
	"log/slog"
	"slices"
	"sync"
	"testing"
	"time"
)

// loopTestInterval 是主循环测试用的 tick 间隔。
//
// 取得足够大（20ms），保证一轮 modulesTickAll（微秒级）绝不可能跨过两次 tick。于是
// 「模块在第 n 次 Tick 里取消 ctx」能确定性地在第 n 轮末尾结束循环 ——
// 测试既不需要 sleep，也不会因为 select 在多个就绪 case 间随机选择而 flaky。
const loopTestInterval = 20 * time.Millisecond

// loopApp 造一个日志丢弃的 App，避免测试输出被启动日志污染。
func loopApp(opts ...Option) *App {
	return New("test", append([]Option{WithLogger(slog.New(slog.DiscardHandler))}, opts...)...)
}

// tickableModule 是实现了 Tickable 的假模块：记录每次 Tick 收到的 now，
// 并可在第 n 次 tick 后回调 —— 测试用它确定性地取消 ctx 结束主循环。
type tickableModule struct {
	*fakeModule
	mu    sync.Mutex
	nows  []time.Time
	after func(n int)
}

func (m *tickableModule) Tick(now time.Time, _ uint64) {
	m.mu.Lock()
	m.nows = append(m.nows, now)
	n := len(m.nows)
	m.mu.Unlock()

	m.rec.add(m.name, "Tick")
	if m.after != nil {
		m.after(n)
	}
}

func (m *tickableModule) count() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.nows)
}

// 编译期确认两个测试模块真的满足各自的能力接口。
//
// 没有这几行的话，接口签名一变，模块就**静默**不再满足 —— Build 的类型断言筛不到它，
// 容器变空，测试表现为"挂住"而不是"编译失败"（这个坑踩过一次）。
var (
	_ Tickable = (*tickableModule)(nil)
	_ Waker    = (*countingWaker)(nil)
	_ Tickable = (*tickRecording)(nil)
)

// registerTickable 注册一个带 Tickable 能力的模块。
func registerTickable(a *App, rec *recorder, name string, deps []string, after func(int)) *tickableModule {
	m := &tickableModule{
		fakeModule: &fakeModule{name: name, app: a, deps: deps, rec: rec},
		after:      after,
	}
	a.RegisterModule(func(*App) Module { return m })
	return m
}

// cancelWaker 是"只负责结束循环"的 Waker：被唤醒一次就取消 ctx。
//
// 没有周期 tick 可依赖时（WithTickInterval(0)、或压根没有 Tickable 模块），
// wakeCh 是唯一能主动推进循环的入口，用它驱动退出是确定性的。
func cancelWaker(a *App, rec *recorder, cancel context.CancelFunc) *countingWaker {
	w := &countingWaker{
		fakeModule: &fakeModule{name: "w", app: a, rec: rec},
		replies:    []bool{false},
	}
	w.after = func(int) { cancel() }
	a.RegisterModule(func(*App) Module { return w })
	return w
}

// TestRunLoopTicksInTopoOrder 钉住三件事：每轮广播全部 Tickable、顺序是拓扑序
// 而非注册序、同一轮内所有模块拿到同一个 now。
func TestRunLoopTicksInTopoOrder(t *testing.T) {
	rec := &recorder{}
	a := loopApp(WithTickInterval(loopTestInterval))

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// 注册序是 early → dep → other，但 early 依赖 dep，
	// 稳定拓扑排序必须把 dep 提到 early 前面：dep → early → other。
	var dep *tickableModule
	dep = registerTickable(a, rec, "dep", nil, func(n int) {
		// 第二轮末尾取消 ctx：此时本轮 dep 已跑完，early/other 还会跑完，
		// 所以总数恰好是 2 轮。
		if n == 2 {
			cancel()
		}
	})
	early := registerTickable(a, rec, "early", []string{"dep"}, nil)
	other := registerTickable(a, rec, "other", nil, nil)

	if err := a.Run(ctx); err != nil {
		t.Fatalf("Run() error = %v, want nil", err)
	}

	assertCalls(t, rec.snapshot(), []string{
		"dep.Init", "early.Init", "other.Init",
		"dep.Start", "early.Start", "other.Start",
		"dep.Tick", "early.Tick", "other.Tick",
		"dep.Tick", "early.Tick", "other.Tick",
	})

	// 同一轮内所有模块必须拿到同一个时间基准，否则模块做"距上次过了多久"的
	// 判断时基准不一致。
	if !dep.nows[0].Equal(early.nows[0]) || !early.nows[0].Equal(other.nows[0]) {
		t.Errorf("第一轮内各模块的 now 不一致: %v / %v / %v",
			dep.nows[0], early.nows[0], other.nows[0])
	}
	if dep.nows[0].Equal(dep.nows[1]) {
		t.Error("两轮之间的 now 相同，时间基准没有推进")
	}
}

// TestRunLoopIntervalDisabled 钉住 WithTickInterval(0) 的效果：
// 周期 tick 关闭，但唤醒事件照常处理。
func TestRunLoopIntervalDisabled(t *testing.T) {
	rec := &recorder{}
	a := loopApp(WithTickInterval(0))

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// worker 实现了 Tickable —— 若 ticker 照开，它每轮都会被调到；这里断言一次都没有。
	registerTickable(a, rec, "worker", nil, nil)
	cancelWaker(a, rec, cancel)
	a.Notify() // 循环起来后得先给个 token，否则 Wake 一次都不会被调

	if err := a.Run(ctx); err != nil {
		t.Fatalf("Run() error = %v, want nil", err)
	}

	// 全程没有任何 Tick，但唤醒照常被处理。
	assertCalls(t, rec.snapshot(), []string{
		"worker.Init", "w.Init",
		"worker.Start", "w.Start",
	})
}

// TestRunLoopNoTickablesNoTicker 覆盖"没有 Tickable 模块时不开 ticker"这条分支：
// tickCh 会是 nil channel，循环仍必须能被 ctx 正常结束。
func TestRunLoopNoTickablesNoTicker(t *testing.T) {
	rec := &recorder{}
	a := loopApp() // 用默认间隔，但没有模块实现 Tickable

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	cancelWaker(a, rec, cancel)
	a.Notify()

	if err := a.Run(ctx); err != nil {
		t.Fatalf("Run() error = %v, want nil", err)
	}
	assertCalls(t, rec.snapshot(), []string{"w.Init", "w.Start"})
}

// TestExternalCtxCancelStillExits 钉住高频 tick 不会饿死 ctx.Done。
// 用"在 tick 回调里取消 ctx"驱动，完全确定性：不需要 sleep，也不用把 Run 丢进 goroutine。
func TestExternalCtxCancelStillExits(t *testing.T) {
	rec := &recorder{}
	a := loopApp(WithTickInterval(time.Millisecond))

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	busy := registerTickable(a, rec, "busy", nil, func(n int) {
		if n == 3 {
			cancel()
		}
	})

	if err := a.Run(ctx); err != nil {
		t.Fatalf("Run() error = %v, want nil", err)
	}

	// 不要求恰好 3 次：ctx 取消那一刻若 select 选中了 tick 分支，会多跑一轮
	// （见 runLoop 的注释）。这里只要求"跑到过第 3 次，且循环确实退出了"。
	if got := busy.count(); got < 3 {
		t.Errorf("tick 次数 = %d, want >= 3", got)
	}
}

// TestNoTickAfterRunReturns 钉住"循环先退、调用方收尾后行"：
// Run 返回之后不可能再有 tick 在跑 —— 循环跑在 Run 自己的 goroutine 上。
func TestNoTickAfterRunReturns(t *testing.T) {
	rec := &recorder{}
	a := loopApp(WithTickInterval(time.Millisecond))

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	busy := registerTickable(a, rec, "busy", nil, func(n int) {
		if n == 3 {
			cancel()
		}
	})

	if err := a.Run(ctx); err != nil {
		t.Fatalf("Run() error = %v, want nil", err)
	}

	// 断言"不再有活动"天然要等一会儿：1ms 的间隔下，若循环还活着这里会涨很多。
	before := busy.count()
	time.Sleep(20 * time.Millisecond)
	if after := busy.count(); after != before {
		t.Errorf("Run 返回后 tick 计数仍在增长: %d → %d", before, after)
	}
}

// ---- 事件驱动（Waker）----

// countingWaker 按序返回预设的 bool，并记录被调了几次。
// 单轮处理多少由模块自己定，所以这里不关心"给了多少预算"。
type countingWaker struct {
	*fakeModule
	mu      sync.Mutex
	calls   int
	replies []bool
	after   func(n int)
}

func (m *countingWaker) Wake() bool {
	m.mu.Lock()
	m.calls++
	n := m.calls
	var ret bool
	if n-1 < len(m.replies) {
		ret = m.replies[n-1]
	}
	m.mu.Unlock()

	if m.after != nil {
		m.after(n)
	}
	return ret
}

func (m *countingWaker) got() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.calls
}

// TestWakeRenotifyOnBacklog 钉住：模块返回 true（还有积压）时框架会自己再调度一轮，
// 不依赖生产者再 Notify —— select 已经把进来那个 token 消费掉了。
func TestWakeRenotifyOnBacklog(t *testing.T) {
	rec := &recorder{}
	a := loopApp(WithTickInterval(0))

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// 第一轮报"还有" → 框架补 token → 第二轮报"没有" → 取消 ctx 收场。
	m := &countingWaker{
		fakeModule: &fakeModule{name: "w", app: a, rec: rec},
		replies:    []bool{true, false},
	}
	m.after = func(n int) {
		if n == 2 {
			cancel()
		}
	}
	a.RegisterModule(func(*App) Module { return m })

	// 循环起来后得先给它一个 token，否则没人叫，Wake 一次都不会被调。
	a.Notify()

	if err := a.Run(ctx); err != nil {
		t.Fatalf("Run() error = %v, want nil", err)
	}

	if got := m.got(); got != 2 {
		t.Errorf("Wake 被调 %d 次, want 2（第一次报积压后框架应立刻再调度一轮）", got)
	}
}

// tickRecording 记录每次 Tick 收到的 tickCount。
type tickRecording struct {
	*fakeModule
	mu     sync.Mutex
	counts []uint64
	after  func(n int)
}

func (m *tickRecording) Tick(_ time.Time, tickCount uint64) {
	m.mu.Lock()
	m.counts = append(m.counts, tickCount)
	n := len(m.counts)
	m.mu.Unlock()

	if m.after != nil {
		m.after(n)
	}
}

func (m *tickRecording) got() []uint64 {
	m.mu.Lock()
	defer m.mu.Unlock()
	return slices.Clone(m.counts)
}

// TestTickCountNotBumpedByEvents 钉住 tickCount 的语义与交付方式。
//
// 语义：数的是**周期 tick 跑过多少次**，从 1 开始 —— 不是"while 转了几圈"，
// 两次 tick 之间穿插事件不该让它跳。这条不成立的话，模块按 `tickCount % N` 做的分频
// 会随事件多寡而变，形同虚设。
//
// 交付：**只作为 Tick 的参数**，App 上没有查询方法 —— 自起 goroutine 的模块
// 拿不到参数，也就拿不到 tickCount，这正是这个设计的用意。
func TestTickCountNotBumpedByEvents(t *testing.T) {
	rec := &recorder{}
	a := loopApp(WithTickInterval(loopTestInterval))

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	m := &tickRecording{fakeModule: &fakeModule{name: "u", app: a, rec: rec}}
	m.after = func(n int) {
		switch n {
		case 1:
			a.Notify() // 让下一轮走"唤醒"分支，而不是 tick 分支
		case 2:
			cancel()
		}
	}
	a.RegisterModule(func(*App) Module { return m })

	// 一个被唤醒后什么都不做的 Waker —— 它的存在让第 2 轮真的走进 wakeCh 分支。
	w := &countingWaker{
		fakeModule: &fakeModule{name: "w", app: a, rec: rec},
		replies:    []bool{false},
	}
	a.RegisterModule(func(*App) Module { return w })

	if err := a.Run(ctx); err != nil {
		t.Fatalf("Run() error = %v, want nil", err)
	}

	// 若 tickCount 数的是"轮次"，两次 Tick 之间隔着那次唤醒，这里会是 [1 3]。
	if got := m.got(); !slices.Equal(got, []uint64{1, 2}) {
		t.Errorf("Tick 收到的 tickCount = %v, want [1 2]（tickCount 应当只数周期 tick）", got)
	}
}
