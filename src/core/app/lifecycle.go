package app

import (
	"context"
	"fmt"
	"strings"
	"time"
)

// defaultShutdownTimeout 是停服阶段的总预算。
//
// 10s 取自「比 Windows 关控制台窗口的默认等待（约 5s）宽裕、比 k8s 的
// terminationGracePeriod（默认 30s）紧」这一段。
const defaultShutdownTimeout = 10 * time.Second

// lifecycleState 标记 App 整体的推进位置，用于拒绝错序调用
type lifecycleState uint8

const (
	lifecycleNew      lifecycleState = iota // 已创建，未 Build
	lifecycleBuilt                          // Build 完成
	lifecycleInited                         // Init 完成
	lifecycleStarted                        // Start 完成
	lifecycleStopped                        // Stop 已执行
	lifecycleShutdown                       // Shutdown 已执行，或回滚已完成 —— 终态
)

func (s lifecycleState) String() string {
	switch s {
	case lifecycleNew:
		return "new"
	case lifecycleBuilt:
		return "built"
	case lifecycleInited:
		return "inited"
	case lifecycleStarted:
		return "started"
	case lifecycleStopped:
		return "stopped"
	case lifecycleShutdown:
		return "shutdown"
	default:
		return "unknown"
	}
}

func (a *App) Init() error {
	a.mu.Lock()
	if a.state != lifecycleBuilt {
		a.mu.Unlock()
		return a.stateErr("init", lifecycleBuilt)
	}
	a.state = lifecycleInited
	modulesArr := a.modulesArr
	a.mu.Unlock()

	for i, m := range modulesArr {
		if err := m.Init(); err != nil {
			a.rollbackInit(i)
			return a.moduleErr("init", m.Name(), err)
		}
		a.logger.Info("module inited", "app", a.name, "module", m.Name())
	}
	return nil
}

func (a *App) Start() error {
	a.mu.Lock()
	if a.state != lifecycleInited {
		a.mu.Unlock()
		return a.stateErr("start", lifecycleInited)
	}
	a.state = lifecycleStarted
	modulesArr := a.modulesArr
	a.mu.Unlock()

	for i, m := range modulesArr {
		if err := m.Start(); err != nil {
			a.rollbackStart(i)
			return a.moduleErr("start", m.Name(), err)
		}
		a.logger.Info("module started", "app", a.name, "module", m.Name())
	}
	return nil
}

func (a *App) Stop() {
	a.mu.Lock()
	if a.state != lifecycleInited && a.state != lifecycleStarted {
		a.mu.Unlock()
		return
	}

	allStarted := a.state == lifecycleStarted
	a.state = lifecycleStopped
	modulesArr := a.modulesArr
	a.mu.Unlock()

	if allStarted {
		a.stopRange(0, len(modulesArr))
	}

	a.cancel()
}

// Shutdown 逆序等待所有模块退出并释放资源。
//
// 返回非 nil（ErrShutdownTimeout）表示有模块超出预算被放弃 —— 进程能退出，但那几个
// 模块没走完自己的清理。看门狗只是「不再等」，它**不会**杀 goroutine：被放弃模块的
// Shutdown 仍在跑，直到进程结束被一起带走。
func (a *App) Shutdown() error {
	a.mu.Lock()
	if a.state == lifecycleShutdown {
		a.mu.Unlock()
		return nil
	}
	if a.state < lifecycleInited {
		a.state = lifecycleShutdown
		a.mu.Unlock()
		return nil
	}
	if a.state == lifecycleStarted {
		a.logger.Warn("shutdown before stop", "app", a.name)
	}
	a.state = lifecycleShutdown
	modulesArr := a.modulesArr
	a.mu.Unlock()

	abandoned := a.shutdownRange(0, len(modulesArr), a.shutdownDeadline())

	a.cancel() // 顺序不变：先 cancel 再返回

	if len(abandoned) > 0 {
		return wrapApp(a.name, fmt.Errorf("%d abandoned: %s: %w",
			len(abandoned), strings.Join(abandoned, ", "), ErrShutdownTimeout))
	}
	return nil
}

// shutdownDeadline 返回本次停服阶段的统一截止时刻；未配置预算时返回零值。
func (a *App) shutdownDeadline() time.Time {
	if a.shutdownTimeout <= 0 {
		return time.Time{}
	}
	return time.Now().Add(a.shutdownTimeout)
}

// rollbackInit 释放 [0, n) 已 Init 模块的资源，第 n 个模块的 Init 已自行清理
func (a *App) rollbackInit(n int) {
	a.cancel() // 必须先于任何 Shutdown，否则等它的 goroutine 会一直卡住
	if abandoned := a.shutdownRange(0, n, a.shutdownDeadline()); len(abandoned) > 0 {
		// 原始启动错误才是要返回给调用方的那个，被放弃的模块只记日志
		a.logger.Warn("rollback abandoned modules", "app", a.name, "modules", abandoned)
	}
	a.setTerminal()
}

// rollbackStart 处理 Start 在第 fail 个模块失败后的回滚，全局顺序严格逆序
//
//	(fail, n) 只 Init 过   → 逆序只 Shutdown，释放 Init 期资源
//	fail                   → 跳过，接口约定该模块已自行清理
//	[0, fail) 已 Start     → 逆序 Stop + Shutdown
//
// 这里依赖一条不变量：Init 全部跑完才开始 Start。于是"已启动"必然是 [0, fail)
// 这个前缀，"已 Init 未启动"必然是 (fail, n) 这个后缀，无需逐模块记账
// 若将来改成边 Init 边 Start 的交错生命周期，这个前提就不成立，必须改回按模块记状态
func (a *App) rollbackStart(fail int) {
	n := len(a.modulesArr)
	a.cancel() // 必须先于任何 Shutdown，否则等它的 goroutine 会一直卡住
	// 预算在整段回滚开始时算一次，三段共享 —— 与正常停服路径同一语义。
	deadline := a.shutdownDeadline()
	abandoned := a.shutdownRange(fail+1, n, deadline)
	a.stopRange(0, fail)
	abandoned = append(abandoned, a.shutdownRange(0, fail, deadline)...)
	if len(abandoned) > 0 {
		a.logger.Warn("rollback abandoned modules", "app", a.name, "modules", abandoned)
	}
	a.setTerminal()
}

// stopRange 逆序对 [lo, hi) 区间模块发出退出信号，不等待
func (a *App) stopRange(lo, hi int) {
	for i := hi - 1; i >= lo; i-- {
		a.modulesArr[i].Stop()
		a.logger.Info("module stopped", "app", a.name, "module", a.modulesArr[i].Name())
	}
}

// shutdownRange 逆序等待 [lo, hi) 区间模块退出并释放资源，
// 返回超出预算被放弃的模块名。
//
// 这不破坏「关机分两趟」的约定：Stop 仍是全部先发信号，Shutdown 仍严格逆序，
// 看门狗只给单次等待加上界，没有改成逐模块 Stop+Shutdown。
func (a *App) shutdownRange(lo, hi int, deadline time.Time) []string {
	var abandoned []string
	for i := hi - 1; i >= lo; i-- {
		m := a.modulesArr[i]
		if a.shutdownOne(m, deadline) {
			a.logger.Info("module shutdown", "app", a.name, "module", m.Name())
			continue
		}
		abandoned = append(abandoned, m.Name())
		a.logger.Error("module shutdown abandoned", "app", a.name, "module", m.Name())
	}
	return abandoned
}

// shutdownOne 在预算内等一个模块退出；返回 false 表示超预算、该模块被放弃。
//
// 传给模块的 ctx 从 Background 派生，**绝不能从 a.ctx 派生**：a.ctx 在一切 Shutdown
// 之前就已被 cancel（框架硬不变量），基于它派生出来的 ctx 一开门就是 done 的，
// ctx 感知的模块会立刻放弃清理 —— 兜底反而变成了默认行为，比不传还糟。
func (a *App) shutdownOne(m Module, deadline time.Time) bool {
	ctx := context.Background()
	var timeoutC <-chan struct{}
	if !deadline.IsZero() {
		var cancel context.CancelFunc
		ctx, cancel = context.WithDeadline(ctx, deadline)
		defer cancel() // 超时后 cancel：让 ctx 感知的模块的 goroutine 也能退出
		timeoutC = ctx.Done()
	}

	done := make(chan struct{})
	go func() {
		defer close(done)
		m.Shutdown(ctx)
	}()

	select {
	case <-done:
		return true
	case <-timeoutC:
		// 未配置预算时 timeoutC 为 nil，永久阻塞 —— 即没有看门狗时的"死等"。
		return false
	}
}

// setTerminal 把 App 置为终态：启动失败即进程结束，不再允许重试
func (a *App) setTerminal() {
	a.mu.Lock()
	a.state = lifecycleShutdown
	a.mu.Unlock()
}
