package app

import (
	"context"
	"fmt"
	"log/slog"
	"strings"
	"sync"
	"time"
)

// 服务进程框架 App
type App struct {
	name              string
	logger            *slog.Logger
	ctx               context.Context
	cancel            context.CancelFunc
	mu                sync.RWMutex
	state             lifecycleState
	modulesFactoryArr []func(*App) Module
	modulesArr        []Module
	modulesMap        map[string]Module
	// 三个能力子集，Build 阶段筛出后冻结：主循环只遍历它们，不再每轮扫全表。
	// 装的是 Module 而非能力接口，所以热路径每次要断言一下 —— 容器只由 Build 里
	// 那段筛选产出，该断言必然成立；换来的是日志与将来自省都能直接取名字。
	modulesUpdater  []Module
	modulesReloader []Module
	modulesWaker    []Module

	// wakeCh 是唤醒信号：容量 1、不承载数据，只表示"可能有 Waker 模块要处理"。
	// 非阻塞发送，多个生产者同时叫会合并成一个 token；App 醒来后扫全部
	// Waker 模块，不会漏哪个。
	//
	// 必须与 ctrlCh 分开：ctrlCh 承载停服/热更这类丢了会出事的消息，
	// wakeCh 是可以随意合并、丢弃的信号 —— 两者的后果量级不同。
	// 单轮处理多少完全由模块自己定，这里没有预算字段 —— 见 Waker 的契约。
	wakeCh chan struct{}

	// updateCount 是**周期更新跑过的次数**，从 1 开始。只被主循环 goroutine 碰，故不用 atomic。
	//
	// 关键在于它数的不是"while 转了几圈"：两次更新之间无论穿插多少事件（唤醒、停服、
	// 热更），updateCount 都是连着的。这样模块按 `updateCount % N` 分频才有意义，
	// `updateCount × 更新间隔` 也才近似等于运行时长。
	//
	// 仍不是精确的时钟 —— 单轮超时时 ticker 会丢 tick，那种情况下 updateCount 会落后于时间。
	// 要精确的时间差请用 `Update` 收到的 `now`。
	//
	// **它没有查询方法，只作为 `Update` 的参数交付**。自起 goroutine 的模块不在主循环
	// 的节奏上（随时会被别的轮次推走），读了只会误导；那种模块要计数请自己记。
	// 参数是唯一入口 —— 拿不到参数就拿不到 updateCount。
	updateCount uint64

	// updateInterval 是主循环周期更新的间隔。New 时定死，Build 之后不再改，
	// 因此主循环读它不需要加锁。<= 0 表示关闭周期更新。
	updateInterval time.Duration

	// shutdownTimeout 是停服阶段的总预算。New 时定死。<= 0 表示不设上限（死等）。
	shutdownTimeout time.Duration

	// ctrlSource 是外部注入的控制事件源，可能为 nil（表示没有外部控制源）。
	// New 时定死，之后不再改。
	ctrlSource <-chan Ctrl

	// ctrlCh 是内部控制事件投递口：Post 写、主循环读。
	// 只有 App 持有写端且永不关闭 —— 来源方注入的通道由来源方负责关闭。
	ctrlCh chan Ctrl
}

// Option 用于定制 App
type Option func(*App)

// WithLogger 指定进程日志器，nil 表示忽略。默认使用 slog.Default()
func WithLogger(l *slog.Logger) Option {
	return func(a *App) {
		if l != nil {
			a.logger = l
		}
	}
}

// WithUpdateInterval 设定主循环周期更新的间隔，默认 defaultUpdateInterval。
//
// d <= 0 表示关闭周期更新：循环退化成纯事件驱动，只在控制事件与 ctx 结束时醒来。
// 没有实现 Updater 的模块时框架也会自动这么做（那时开 ticker 纯属空转）。
// 必须在 Build 之前（即 New 时）传入。
func WithUpdateInterval(d time.Duration) Option {
	return func(a *App) {
		a.updateInterval = d
	}
}

// WithShutdownTimeout 设定停服阶段的总预算，默认 defaultShutdownTimeout。
//
// 预算在整个停服阶段开始时算一次、所有模块共享：先卡住的模块吃掉预算，排在它后面
// （逆序拓扑上更靠后）的模块只剩下零头。这换来"总退出时间有上界"。
//
// d <= 0 表示不设上限 —— 退回到没有看门狗时的死等。必须在 Build 之前传入。
func WithShutdownTimeout(d time.Duration) Option {
	return func(a *App) {
		a.shutdownTimeout = d
	}
}

// Notify 由任意 goroutine 调用：某个模块的队列里可能有活了，叫醒主循环去跑
// Waker 模块。非阻塞、可合并 —— 多个生产者同时叫只会产生一个 token，
// 调用方不需要（也无法）判断自己是不是那个"有效"的通知。
func (a *App) Notify() {
	select {
	case a.wakeCh <- struct{}{}:
	default: // 已有 token，合并丢弃
	}
}

// WithCtrl 注入外部控制事件源，例如 sigctrl 把进程信号翻译成的通道。
//
// 框架只读不关：来源方负责在进程退出前停止投递并关闭通道；框架读到通道关闭
// 就把它从 select 里摘掉，不会空转。必须在 Build 之前（即 New 时）传入。
func WithCtrl(ch <-chan Ctrl) Option {
	return func(a *App) {
		a.ctrlSource = ch
	}
}

// New 创建一个待装配的 App
func New(name string, opts ...Option) *App {
	// 默认值先落定，再由 opts 覆盖 —— 于是 Option 只需表达"改成了什么"
	a := &App{
		name:            name,
		logger:          slog.Default(),
		updateInterval:  defaultUpdateInterval,
		shutdownTimeout: defaultShutdownTimeout,
		ctrlCh:          make(chan Ctrl, ctrlBuffer),
		wakeCh:          make(chan struct{}, 1),
	}
	for _, opt := range opts {
		opt(a)
	}
	a.ctx, a.cancel = context.WithCancel(context.Background())
	return a
}

// Post 由任意 goroutine 投递一个控制事件给主循环。
//
// 非阻塞：缓冲满时丢弃并返回 false。缓冲满意味着主循环已被某个回调卡住，
// 该由「第二次停服信号」或停服预算收场，而不是把调用方一起拖死 ——
// 这也正是 Update / Reload 内部可以安全 Post 的原因（含 Post(CtrlStop)）。
//
// 返回值只表示「有没有塞进缓冲」，不要拿它当作「停服请求已被受理」的证据。
func (a *App) Post(c Ctrl) bool {
	select {
	case a.ctrlCh <- c:
		return true
	default:
		return false
	}
}

// Name 返回进程名，如 "gatesvr"
func (a *App) Name() string { return a.name }

// Logger 返回进程日志器。模块应复用同一实例，而不是各自新建
func (a *App) Logger() *slog.Logger { return a.logger }

// Context 返回进程级 ctx。它在 Stop 阶段末尾、Shutdown 之前被 cancel
// 作为模块自身退出信号之外的一道兜底。模块不得依赖它完成自身的资源清理
func (a *App) Context() context.Context { return a.ctx }

// GetModule 按名取已装配的模块；未注册返回 nil
// 可在模块生命周期回调内部安全调用
func (a *App) GetModule(name string) Module {
	a.mu.RLock()
	defer a.mu.RUnlock()
	return a.modulesMap[name]
}

// RegisterModule 登记模块工厂，按调用顺序决定注册序
// 工厂在 Build 阶段才被调用，此时 App 已可用，工厂应把 App 存进模块并返回实例
// Build 之后再调用 RegisterModule 会 panic —— 那属于编程错误，无法通过返回值表达
func (a *App) RegisterModule(f func(*App) Module) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.state != lifecycleNew {
		panic("app: RegisterModule called after Build")
	}
	a.modulesFactoryArr = append(a.modulesFactoryArr, f)
}

// Build 调用全部工厂、校验模块表、按 DependsOn 做稳定拓扑排序
// 只能在 New 之后调用一次
func (a *App) Build() error {
	a.mu.Lock()
	if a.state != lifecycleNew {
		a.mu.Unlock()
		return a.stateErr("build", lifecycleNew)
	}
	if a.name == "" {
		a.mu.Unlock()
		return ErrEmptyName
	}
	modulesFactoryArr := a.modulesFactoryArr
	a.mu.Unlock()

	// 工厂在锁外调用：工厂里可能回调 App，持锁会自锁
	modulesArr := make([]Module, 0, len(modulesFactoryArr))
	modulesMap := make(map[string]Module, len(modulesFactoryArr))
	for i, f := range modulesFactoryArr {
		if f == nil {
			return errAt(a.name, i, ErrNilFactory)
		}
		m := f(a)
		if m == nil {
			return errAt(a.name, i, ErrNilModule)
		}
		name := m.Name()
		if name == "" {
			return errAt(a.name, i, ErrEmptyName)
		}
		if _, dup := modulesMap[name]; dup {
			return errAt(a.name, i, ErrDuplicateModule, name)
		}
		modulesArr = append(modulesArr, m)
		modulesMap[name] = m
	}

	sorted, err := depsSort(modulesArr)
	if err != nil {
		return wrapApp(a.name, err)
	}

	// 按拓扑序筛出实现了能力接口的模块，冻成两份子集供主循环遍历。
	// 两段刻意写开、不抽泛型助手：Go 的泛型函数不能是闭包，抽出去只能放包级，
	// 反而把「只服务于 Build」这件事藏起来了。
	var updaters []Module
	for _, m := range sorted {
		if _, ok := m.(Updater); ok {
			updaters = append(updaters, m)
		}
	}

	var reloaders []Module
	for _, m := range sorted {
		if _, ok := m.(Reloader); ok {
			reloaders = append(reloaders, m)
		}
	}

	var wakers []Module
	for _, m := range sorted {
		if _, ok := m.(Waker); ok {
			wakers = append(wakers, m)
		}
	}

	a.mu.Lock()
	a.modulesArr, a.modulesMap = sorted, modulesMap
	a.modulesUpdater, a.modulesReloader, a.modulesWaker = updaters, reloaders, wakers
	a.state = lifecycleBuilt
	a.mu.Unlock()

	// 把筛出来的模块名打进日志：这是 duck typing 唯一失效模式（方法名拼错导致
	// 静默不收集）的防线 —— 模块没出现在这行里，就是没被收到。
	a.logger.Info("app built", "app", a.name, "modules", len(sorted),
		"modulesUpdater", namesOf(updaters),
		"modulesReloader", namesOf(reloaders),
		"modulesWaker", namesOf(wakers))
	return nil
}

// namesOf 提取模块名，供启动日志与测试使用。
func namesOf(modules []Module) []string {
	out := make([]string, 0, len(modules))
	for _, m := range modules {
		out = append(out, m.Name())
	}
	return out
}

// Run 走完完整的启动与退出序列，阻塞直到 ctx 结束或收到停服控制事件。
// ctx 与 CtrlStop 等价：谁先到都走同一条 Stop → Shutdown 路径。
//
// 返回非 nil 只有两种情况：启动阶段失败（此时框架已回滚完毕），
// 或停服阶段有模块超出预算被放弃（ErrShutdownTimeout，见 WithShutdownTimeout）。
func (a *App) Run(ctx context.Context) error {
	if err := a.Build(); err != nil {
		return err
	}
	if err := a.Init(); err != nil {
		return err
	}
	if err := a.Start(); err != nil {
		return err
	}

	a.runLoop(ctx)

	a.Stop()
	return a.Shutdown()
}

// depsSort 按 DependsOn 模块依赖对 module 做稳定拓扑排序
func depsSort(modules []Module) ([]Module, error) {
	n := len(modules)
	out := make([]Module, 0, n)
	if n == 0 {
		return out, nil
	}

	// 名字 → 注册序号，用于解析依赖与报错
	idx := make(map[string]int, n)
	for i, m := range modules {
		idx[m.Name()] = i
	}

	// dependents[j] 是依赖 j 的模块序号，按注册序追加，天然有序
	dependents := make([][]int, n)
	indeg := make([]int, n)

	for i, m := range modules {
		// 同一个依赖名写多遍只算一次入度，否则该模块永远凑不齐 0 入度
		seen := make(map[string]bool)
		for _, dep := range m.DependsOn() {
			if dep == "" || seen[dep] {
				continue
			}
			seen[dep] = true

			j, ok := idx[dep]
			if !ok {
				return nil, fmt.Errorf("module %s: %w: %s", m.Name(), ErrUnknownDep, dep)
			}
			if j == i {
				return nil, fmt.Errorf("module %s: %w", m.Name(), ErrSelfDep)
			}
			dependents[j] = append(dependents[j], i)
			indeg[i]++
		}
	}

	done := make([]bool, n)
	for len(out) < n {
		next := -1
		for i := 0; i < n; i++ {
			if !done[i] && indeg[i] == 0 {
				next = i // 取注册序号最小的就绪节点
				break
			}
		}
		if next < 0 {
			break // 剩下的都有入度，必然成环
		}
		done[next] = true
		out = append(out, modules[next])
		for _, d := range dependents[next] {
			indeg[d]--
		}
	}

	if len(out) < n {
		// 报出所有排不进去的模块：既包含环上的，也包含挂在环后面的
		// 后者同样无法确定顺序，一起给出来更好定位
		var stuck []string
		for i, m := range modules {
			if !done[i] {
				stuck = append(stuck, m.Name())
			}
		}
		return nil, fmt.Errorf("%w among: %s", ErrCycle, strings.Join(stuck, ", "))
	}
	return out, nil
}
