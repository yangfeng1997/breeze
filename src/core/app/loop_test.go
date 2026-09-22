package app

import (
	"context"
	"errors"
	"log/slog"
	"slices"
	"sync"
	"testing"
	"time"
)

// loopTestInterval 是主循环测试用的更新间隔。
//
// 取得足够大（20ms），保证一轮 modulesUpdateAll（微秒级）绝不可能跨过两次 tick。于是
// 「模块在第 n 次 Update 里 Post(CtrlStop)」能确定性地在第 n 轮末尾结束循环 ——
// 测试既不需要 sleep，也不会因为 select 在多个就绪 case 间随机选择而 flaky。
const loopTestInterval = 20 * time.Millisecond

// loopApp 造一个日志丢弃的 App，避免测试输出被启动日志污染。
func loopApp(opts ...Option) *App {
	return New("test", append([]Option{WithLogger(slog.New(slog.DiscardHandler))}, opts...)...)
}

// updatableModule 是实现了 Updater 的假模块：记录每次 Update 收到的 now，
// 并可在第 n 次更新后回调 —— 测试用它确定性地点 Post(CtrlStop) 结束主循环。
type updatableModule struct {
	*fakeModule
	mu    sync.Mutex
	nows  []time.Time
	after func(n int)
}

func (m *updatableModule) Update(now time.Time, _ uint64) {
	m.mu.Lock()
	m.nows = append(m.nows, now)
	n := len(m.nows)
	m.mu.Unlock()

	m.rec.add(m.name, "Update")
	if m.after != nil {
		m.after(n)
	}
}

func (m *updatableModule) count() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.nows)
}

// 编译期确认三个测试模块真的满足各自的能力接口。
//
// 没有这几行的话，接口签名一变，模块就**静默**不再满足 —— Build 的类型断言筛不到它，
// 容器变空，测试表现为"挂住"而不是"编译失败"（这个坑踩过一次）。
var (
	_ Updater  = (*updatableModule)(nil)
	_ Reloader = (*reloadableModule)(nil)
	_ Waker    = (*countingWaker)(nil)
	_ Updater  = (*updateRecording)(nil)
)

// reloadableModule 是实现了 Reloader 的假模块。
type reloadableModule struct {
	*fakeModule
	reloadErr error
	after     func(n int)
	mu        sync.Mutex
	n         int
}

func (m *reloadableModule) Reload() error {
	m.mu.Lock()
	m.n++
	n := m.n
	m.mu.Unlock()

	m.rec.add(m.name, "Reload")
	if m.after != nil {
		m.after(n)
	}
	return m.reloadErr
}

// postingModule 在 Start 里投递控制事件，用于确定性驱动主循环。
type postingModule struct {
	*fakeModule
	post []Ctrl
}

func (m *postingModule) Start() error {
	if err := m.fakeModule.Start(); err != nil {
		return err
	}
	for _, c := range m.post {
		m.app.Post(c)
	}
	return nil
}

// registerUpdatable 注册一个带 Updater 能力的模块。
func registerUpdatable(a *App, rec *recorder, name string, deps []string, after func(int)) *updatableModule {
	m := &updatableModule{
		fakeModule: &fakeModule{name: name, app: a, deps: deps, rec: rec},
		after:      after,
	}
	a.RegisterModule(func(*App) Module { return m })
	return m
}

// registerReloadable 注册一个带 Reloader 能力的模块。
func registerReloadable(a *App, rec *recorder, name string, deps []string, reloadErr error, after func(int)) *reloadableModule {
	m := &reloadableModule{
		fakeModule: &fakeModule{name: name, app: a, deps: deps, rec: rec},
		reloadErr:  reloadErr,
		after:      after,
	}
	a.RegisterModule(func(*App) Module { return m })
	return m
}

// registerPosting 注册一个在 Start 里投递控制事件的模块。
func registerPosting(a *App, rec *recorder, name string, deps []string, post ...Ctrl) *postingModule {
	m := &postingModule{
		fakeModule: &fakeModule{name: name, app: a, deps: deps, rec: rec},
		post:       post,
	}
	a.RegisterModule(func(*App) Module { return m })
	return m
}

// TestRunLoopUpdatesInTopoOrder 钉住三件事：每轮广播全部 Updater、顺序是拓扑序
// 而非注册序、同一轮内所有模块拿到同一个 now。
func TestRunLoopUpdatesInTopoOrder(t *testing.T) {
	rec := &recorder{}
	a := loopApp(WithUpdateInterval(loopTestInterval))

	// 注册序是 early → dep → other，但 early 依赖 dep，
	// 稳定拓扑排序必须把 dep 提到 early 前面：dep → early → other。
	var dep *updatableModule
	dep = registerUpdatable(a, rec, "dep", nil, func(n int) {
		// 第二轮末尾投递停服：此时本轮 dep 已跑完，early/other 还会跑完，
		// 所以总数恰好是 2 轮。
		if n == 2 {
			a.Post(CtrlStop)
		}
	})
	early := registerUpdatable(a, rec, "early", []string{"dep"}, nil)
	other := registerUpdatable(a, rec, "other", nil, nil)

	if err := a.Run(context.Background()); err != nil {
		t.Fatalf("Run() error = %v, want nil", err)
	}

	assertCalls(t, rec.snapshot(), []string{
		"dep.Init", "early.Init", "other.Init",
		"dep.Start", "early.Start", "other.Start",
		"dep.Update", "early.Update", "other.Update",
		"dep.Update", "early.Update", "other.Update",
		"other.Stop", "early.Stop", "dep.Stop",
		"other.Shutdown", "early.Shutdown", "dep.Shutdown",
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

// TestRunLoopIntervalDisabled 钉住 WithUpdateInterval(0) 的效果：
// 周期更新关闭，但控制事件照常处理。
func TestRunLoopIntervalDisabled(t *testing.T) {
	rec := &recorder{}
	a := loopApp(WithUpdateInterval(0))

	registerUpdatable(a, rec, "worker", nil, nil)
	registerReloadable(a, rec, "cfg", nil, nil, func(n int) {
		a.Post(CtrlStop) // 热更完就停，结束循环
	})
	registerPosting(a, rec, "trigger", nil, CtrlReload)

	if err := a.Run(context.Background()); err != nil {
		t.Fatalf("Run() error = %v, want nil", err)
	}

	// 三个模块互不依赖，保持注册序 worker → cfg → trigger；
	// 关键是：全程没有任何 Update，但 CtrlReload 照常被处理。
	assertCalls(t, rec.snapshot(), []string{
		"worker.Init", "cfg.Init", "trigger.Init",
		"worker.Start", "cfg.Start", "trigger.Start",
		"cfg.Reload",
		"trigger.Stop", "cfg.Stop", "worker.Stop",
		"trigger.Shutdown", "cfg.Shutdown", "worker.Shutdown",
	})
}

// TestRunLoopNoUpdatersNoTicker 覆盖"没有 Updater 模块时不开 ticker"这条分支：
// updateCh 会是 nil channel，循环仍必须能被 ctx 正常结束。
func TestRunLoopNoUpdatersNoTicker(t *testing.T) {
	rec := &recorder{}
	a := loopApp() // 用默认间隔，但没有模块实现 Updater

	registerPosting(a, rec, "a", nil, CtrlStop)

	if err := a.Run(context.Background()); err != nil {
		t.Fatalf("Run() error = %v, want nil", err)
	}
	assertCalls(t, rec.snapshot(), []string{
		"a.Init", "a.Start", "a.Stop", "a.Shutdown",
	})
}

// TestPostCtrlStopExitsRun 钉住 Post(CtrlStop) 与 ctx 取消等价：
// 走同一条 Stop → Shutdown 路径，Run 返回 nil。
func TestPostCtrlStopExitsRun(t *testing.T) {
	rec := &recorder{}
	a := loopApp(WithUpdateInterval(loopTestInterval))

	registerUpdatable(a, rec, "a", nil, nil)
	registerPosting(a, rec, "stopper", nil, CtrlStop)

	// ctx 永不取消：Run 若没被 Post 驱动退出，这个测试会直接挂住。
	if err := a.Run(context.Background()); err != nil {
		t.Fatalf("Run() error = %v, want nil", err)
	}

	assertCalls(t, rec.snapshot(), []string{
		"a.Init", "stopper.Init",
		"a.Start", "stopper.Start",
		"stopper.Stop", "a.Stop",
		"stopper.Shutdown", "a.Shutdown",
	})
}

// TestCtrlReloadCallsReloadersInOrder 钉住热更顺序是正向拓扑序
// （与 Stop 的逆序相反）：配置提供者先换新，消费者再用新的。
func TestCtrlReloadCallsReloadersInOrder(t *testing.T) {
	rec := &recorder{}
	a := loopApp(WithUpdateInterval(0))

	// 注册序 consumer → base → spare，consumer 依赖 base：
	// 拓扑序 base → consumer → spare。
	registerReloadable(a, rec, "consumer", []string{"base"}, nil, nil)
	registerReloadable(a, rec, "base", nil, nil, nil)
	registerReloadable(a, rec, "spare", nil, nil, func(n int) {
		a.Post(CtrlStop)
	})
	registerPosting(a, rec, "trigger", nil, CtrlReload)

	if err := a.Run(context.Background()); err != nil {
		t.Fatalf("Run() error = %v, want nil", err)
	}

	assertCalls(t, rec.snapshot(), []string{
		"base.Init", "consumer.Init", "spare.Init", "trigger.Init",
		"base.Start", "consumer.Start", "spare.Start", "trigger.Start",
		"base.Reload", "consumer.Reload", "spare.Reload",
		"trigger.Stop", "spare.Stop", "consumer.Stop", "base.Stop",
		"trigger.Shutdown", "spare.Shutdown", "consumer.Shutdown", "base.Shutdown",
	})
}

// TestReloadErrorDoesNotStopLoop 钉住热更失败的语义：
// 只记日志、继续调用其余模块、不停服也不回滚。
func TestReloadErrorDoesNotStopLoop(t *testing.T) {
	rec := &recorder{}
	a := loopApp(WithUpdateInterval(0))

	boom := errors.New("bad config")
	registerReloadable(a, rec, "broken", nil, boom, nil)
	registerReloadable(a, rec, "healthy", nil, nil, func(n int) {
		a.Post(CtrlStop)
	})
	registerPosting(a, rec, "trigger", nil, CtrlReload)

	if err := a.Run(context.Background()); err != nil {
		t.Fatalf("Run() error = %v, want nil", err)
	}

	// broken 失败后 healthy 仍必须被调到。
	assertCalls(t, rec.snapshot(), []string{
		"broken.Init", "healthy.Init", "trigger.Init",
		"broken.Start", "healthy.Start", "trigger.Start",
		"broken.Reload", "healthy.Reload",
		"trigger.Stop", "healthy.Stop", "broken.Stop",
		"trigger.Shutdown", "healthy.Shutdown", "broken.Shutdown",
	})
}

// TestUnknownCtrlIgnored 钉住未知控制事件既不致死循环、也不被当成停服。
func TestUnknownCtrlIgnored(t *testing.T) {
	rec := &recorder{}
	a := loopApp(WithUpdateInterval(loopTestInterval))

	registerUpdatable(a, rec, "a", nil, nil)
	registerPosting(a, rec, "trigger", nil, Ctrl(99), CtrlStop)

	if err := a.Run(context.Background()); err != nil {
		t.Fatalf("Run() error = %v, want nil", err)
	}
	assertCalls(t, rec.snapshot(), []string{
		"a.Init", "trigger.Init",
		"a.Start", "trigger.Start",
		"trigger.Stop", "a.Stop",
		"trigger.Shutdown", "a.Shutdown",
	})
}

// stuckModule 的 Shutdown 永远不返回，用来验证停服预算看门狗。
type stuckModule struct {
	*fakeModule
	release chan struct{} // 测试收尾时放行，避免 goroutine 泄漏到别的用例
}

func (m *stuckModule) Shutdown(ctx context.Context) {
	m.rec.add(m.name, "Shutdown")
	<-m.release
}

// TestShutdownReceivesLiveCtx 钉住一个很容易写错的地方：交给模块的预算 ctx
// 必须从 Background 派生，**不能**从 a.ctx 派生 —— 后者在一切 Shutdown 之前
// 就已 cancel，基于它派生出来的 ctx 一开门就是 done 的，ctx 感知的模块会立刻
// 放弃清理，兜底反而变成了默认行为。
func TestShutdownReceivesLiveCtx(t *testing.T) {
	rec := &recorder{}
	a := loopApp(WithShutdownTimeout(time.Second))

	m := &fakeModule{name: "a", app: a, rec: rec}
	a.RegisterModule(func(*App) Module { return m })

	if err := a.Build(); err != nil {
		t.Fatalf("Build() error = %v, want nil", err)
	}
	if err := a.Init(); err != nil {
		t.Fatalf("Init() error = %v, want nil", err)
	}
	if err := a.Start(); err != nil {
		t.Fatalf("Start() error = %v, want nil", err)
	}

	a.Stop()
	if err := a.Shutdown(); err != nil {
		t.Fatalf("Shutdown() error = %v, want nil", err)
	}

	if m.shutdownCtxErr != nil {
		t.Errorf("Shutdown 收到的 ctx 已经 done（%v）—— 它必须从 Background 派生", m.shutdownCtxErr)
	}
	if !m.shutdownCtxHasDeadline {
		t.Error("Shutdown 收到的 ctx 没有 deadline —— 模块拿不到剩余预算")
	}
	// 与此同时，进程 ctx 必须已经 cancel（框架硬不变量）
	if !m.ctxCanceledAtShutdown {
		t.Error("Shutdown 被调用时进程 ctx 还没 cancel")
	}
}

// TestShutdownTimeoutAbandonsStuckModule 钉住看门狗：卡住的模块不该拖死整个进程。
func TestShutdownTimeoutAbandonsStuckModule(t *testing.T) {
	rec := &recorder{}
	a := loopApp(WithShutdownTimeout(50 * time.Millisecond))

	release := make(chan struct{})
	t.Cleanup(func() { close(release) })

	// stuck 排在拓扑序最前 → 逆序停服时最后一个才轮到它，
	// 于是 healthy 有时间在预算内跑完。
	stuck := &stuckModule{
		fakeModule: &fakeModule{name: "stuck", app: a, rec: rec},
		release:    release,
	}
	a.RegisterModule(func(*App) Module { return stuck })
	healthy := &fakeModule{name: "healthy", app: a, rec: rec}
	a.RegisterModule(func(*App) Module { return healthy })

	if err := a.Build(); err != nil {
		t.Fatalf("Build() error = %v, want nil", err)
	}
	if err := a.Init(); err != nil {
		t.Fatalf("Init() error = %v, want nil", err)
	}
	if err := a.Start(); err != nil {
		t.Fatalf("Start() error = %v, want nil", err)
	}

	started := time.Now()
	a.Stop()
	err := a.Shutdown()
	elapsed := time.Since(started)

	if !errors.Is(err, ErrShutdownTimeout) {
		t.Errorf("Shutdown() error = %v, want wraps ErrShutdownTimeout", err)
	}
	if elapsed > 2*time.Second {
		t.Errorf("停服耗时 %v：看门狗没生效", elapsed)
	}
	assertCalls(t, rec.snapshot(), []string{
		"stuck.Init", "healthy.Init",
		"stuck.Start", "healthy.Start",
		"healthy.Stop", "stuck.Stop",
		"healthy.Shutdown", "stuck.Shutdown",
	})
}

// TestExternalCtxCancelStillExits 钉住高频 update 不会饿死 ctx.Done。
// 用"在 update 回调里取消 ctx"驱动，完全确定性：不需要 sleep，也不用把 Run 丢进 goroutine。
func TestExternalCtxCancelStillExits(t *testing.T) {
	rec := &recorder{}
	a := loopApp(WithUpdateInterval(time.Millisecond))

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	busy := registerUpdatable(a, rec, "busy", nil, func(n int) {
		if n == 3 {
			cancel()
		}
	})

	if err := a.Run(ctx); err != nil {
		t.Fatalf("Run() error = %v, want nil", err)
	}

	// 不要求恰好 3 次：ctx 取消那一刻若 select 选中了 update 分支，会多跑一轮
	// （见 runLoop 的注释）。这里只要求"跑到过第 3 次，且循环确实退出了"。
	if got := busy.count(); got < 3 {
		t.Errorf("update 次数 = %d, want >= 3", got)
	}
}

// TestNoUpdateAfterStop 钉住"循环先退、停服后行"：
// Run 返回之后不可能再有 update 在跑 —— 两件事在同一个 goroutine 上串行。
func TestNoUpdateAfterStop(t *testing.T) {
	rec := &recorder{}
	a := loopApp(WithUpdateInterval(time.Millisecond))

	busy := registerUpdatable(a, rec, "busy", nil, nil)
	registerPosting(a, rec, "stopper", nil, CtrlStop)

	if err := a.Run(context.Background()); err != nil {
		t.Fatalf("Run() error = %v, want nil", err)
	}

	// 断言"不再有活动"天然要等一会儿：1ms 的间隔下，若循环还活着这里会涨很多。
	before := busy.count()
	time.Sleep(20 * time.Millisecond)
	if after := busy.count(); after != before {
		t.Errorf("Run 返回后 update 计数仍在增长: %d → %d", before, after)
	}
}

// ---- 事件驱动（Waker）与两个看门狗 ----

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
	a := loopApp(WithUpdateInterval(0))

	// 第一轮报"还有" → 框架补 token → 第二轮报"没有" → 停。
	m := &countingWaker{
		fakeModule: &fakeModule{name: "w", app: a, rec: rec},
		replies:    []bool{true, false},
	}
	m.after = func(n int) {
		if n == 2 {
			a.Post(CtrlStop)
		}
	}
	a.RegisterModule(func(*App) Module { return m })

	// 循环起来后得先给它一个 token，否则没人叫，Wake 一次都不会被调。
	a.Notify()

	if err := a.Run(context.Background()); err != nil {
		t.Fatalf("Run() error = %v, want nil", err)
	}

	if got := m.got(); got != 2 {
		t.Errorf("Wake 被调 %d 次, want 2（第一次报积压后框架应立刻再调度一轮）", got)
	}
}

// updateRecording 记录每次 Update 收到的 updateCount。
type updateRecording struct {
	*fakeModule
	mu     sync.Mutex
	counts []uint64
	after  func(n int)
}

func (m *updateRecording) Update(_ time.Time, updateCount uint64) {
	m.mu.Lock()
	m.counts = append(m.counts, updateCount)
	n := len(m.counts)
	m.mu.Unlock()

	if m.after != nil {
		m.after(n)
	}
}

func (m *updateRecording) got() []uint64 {
	m.mu.Lock()
	defer m.mu.Unlock()
	return slices.Clone(m.counts)
}

// TestUpdateCountNotBumpedByEvents 钉住 updateCount 的语义与交付方式。
//
// 语义：数的是**周期更新跑过多少次**，从 1 开始 —— 不是"while 转了几圈"，
// 两次更新之间穿插事件不该让它跳。这条不成立的话，模块按 `updateCount % N` 做的分频
// 会随事件多寡而变，形同虚设。
//
// 交付：**只作为 Update 的参数**，App 上没有查询方法 —— 自起 goroutine 的模块
// 拿不到参数，也就拿不到 updateCount，这正是这个设计的用意。
func TestUpdateCountNotBumpedByEvents(t *testing.T) {
	rec := &recorder{}
	a := loopApp(WithUpdateInterval(loopTestInterval))

	m := &updateRecording{fakeModule: &fakeModule{name: "u", app: a, rec: rec}}
	m.after = func(n int) {
		switch n {
		case 1:
			a.Notify() // 让下一轮走"唤醒"分支，而不是更新分支
		case 2:
			a.Post(CtrlStop)
		}
	}
	a.RegisterModule(func(*App) Module { return m })

	// 一个被唤醒后什么都不做的 Waker —— 它的存在让第 2 轮真的走进 wakeCh 分支。
	w := &countingWaker{
		fakeModule: &fakeModule{name: "w", app: a, rec: rec},
		replies:    []bool{false},
	}
	a.RegisterModule(func(*App) Module { return w })

	if err := a.Run(context.Background()); err != nil {
		t.Fatalf("Run() error = %v, want nil", err)
	}

	// 若 updateCount 数的是"轮次"，两次 Update 之间隔着那次唤醒，这里会是 [1 3]。
	if got := m.got(); !slices.Equal(got, []uint64{1, 2}) {
		t.Errorf("Update 收到的 updateCount = %v, want [1 2]（updateCount 应当只数周期更新）", got)
	}
}

// TestNoWakeModulesIsFree 钉住没有 Waker 模块时主循环照常：wakeCh 只是个没人写的
// 通道，不该影响停服与周期更新。
func TestNoWakeModulesIsFree(t *testing.T) {
	rec := &recorder{}
	a := loopApp(WithUpdateInterval(0))
	registerPosting(a, rec, "stopper", nil, CtrlStop)

	if err := a.Run(context.Background()); err != nil {
		t.Fatalf("Run() error = %v, want nil", err)
	}
	assertCalls(t, rec.snapshot(), []string{
		"stopper.Init", "stopper.Start", "stopper.Stop", "stopper.Shutdown",
	})
}

// TestClosedCtrlSourceDoesNotSpin 钉住"来源通道被关闭"这条分支：
// 关闭后该 case 必须被摘掉（否则读到 Ctrl(0) 零值会疯狂空转），
// 循环仍要能被内部控制事件正常结束。
func TestClosedCtrlSourceDoesNotSpin(t *testing.T) {
	rec := &recorder{}

	closed := make(chan Ctrl)
	close(closed)

	a := loopApp(WithUpdateInterval(loopTestInterval), WithCtrl(closed))

	registerUpdatable(a, rec, "a", nil, nil)
	registerPosting(a, rec, "stopper", nil, CtrlStop)

	done := make(chan error, 1)
	go func() { done <- a.Run(context.Background()) }()

	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("Run() error = %v, want nil", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("外部控制源关闭后循环空转，Run 没有返回")
	}
}
