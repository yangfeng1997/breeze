package app

import (
	"context"
	"fmt"
	"log/slog"
	"strings"
	"sync"
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

// New 创建一个待装配的 App
func New(name string, opts ...Option) *App {
	a := &App{
		name:   name,
		logger: slog.Default(),
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

	a.mu.Lock()
	a.modulesArr, a.modulesMap, a.state = sorted, modulesMap, lifecycleBuilt
	a.mu.Unlock()

	a.logger.Info("app built", "app", a.name, "modules", len(sorted))
	return nil
}

// Run 走完完整的启动与退出序列，阻塞直到 ctx 结束
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

	<-ctx.Done()

	a.Stop()
	a.Shutdown()
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
