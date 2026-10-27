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
	tickableModules   []Module
	wakeableModules   []Module
	wakeCh            chan struct{}
	tickCount         uint64
	tickInterval      time.Duration
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

// WithTickInterval 设定主循环周期 tick 的间隔，默认 defaultTickInterval。
//
// d <= 0 表示关闭周期 tick：循环退化成纯事件驱动，只在有人 Notify 或 ctx 结束时醒来。
// 没有实现 Tickable 的模块时框架也会自动这么做（那时开 ticker 纯属空转）。
// 必须在 Build 之前（即 New 时）传入。
func WithTickInterval(d time.Duration) Option {
	return func(a *App) {
		a.tickInterval = d
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

// New 创建一个待装配的 App
func New(name string, opts ...Option) *App {
	// 默认值先落定，再由 opts 覆盖 —— 于是 Option 只需表达"改成了什么"
	a := &App{
		name:         name,
		logger:       slog.Default(),
		tickInterval: defaultTickInterval,
		wakeCh:       make(chan struct{}, 1),
	}
	for _, opt := range opts {
		opt(a)
	}
	a.ctx, a.cancel = context.WithCancel(context.Background())
	return a
}

// Name 返回进程名，如 "gatesvr"
func (a *App) Name() string { return a.name }

// Logger 返回进程日志器。模块应复用同一实例，而不是各自新建
func (a *App) Logger() *slog.Logger { return a.logger }

// Context 返回进程级 ctx，在 Run 的主循环退出之后被 cancel。
// 它只表示"进程正在退出"，模块不得依赖它完成自身的资源清理
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
	var tickables []Module
	for _, m := range sorted {
		if _, ok := m.(Tickable); ok {
			tickables = append(tickables, m)
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
	a.tickableModules, a.wakeableModules = tickables, wakers
	a.state = lifecycleBuilt
	a.mu.Unlock()

	// 把筛出来的模块名打进日志：这是 duck typing 唯一失效模式（方法名拼错导致
	// 静默不收集）的防线 —— 模块没出现在这行里，就是没被收到。
	a.logger.Info("app built", "app", a.name, "modules", len(sorted),
		"modulesTickable", namesOf(tickables),
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

// Run 走完启动序列，然后阻塞在 ctx 上，ctx 结束即返回。
//
// 返回非 nil 只表示启动阶段失败 —— 框架**不做任何收尾**：模块集合已 Start、
// 进程 ctx 已被 cancel，谁来释放资源是调用方的事。
func (a *App) Run(ctx context.Context) error {
	defer a.cancel() // 无论从哪条路返回，进程都算正在退出

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
	return nil
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
