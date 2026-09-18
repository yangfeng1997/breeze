package app

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

func (a *App) Shutdown() {
	a.mu.Lock()
	if a.state == lifecycleShutdown {
		a.mu.Unlock()
		return
	}
	if a.state < lifecycleInited {
		a.state = lifecycleShutdown
		a.mu.Unlock()
		return
	}
	if a.state == lifecycleStarted {
		a.logger.Warn("shutdown before stop", "app", a.name)
	}
	a.state = lifecycleShutdown
	modulesArr := a.modulesArr
	a.mu.Unlock()

	a.shutdownRange(0, len(modulesArr))

	a.cancel()
}

// rollbackInit 释放 [0, n) 已 Init 模块的资源，第 n 个模块的 Init 已自行清理
func (a *App) rollbackInit(n int) {
	a.cancel() // 必须先于任何 Shutdown，否则等它的 goroutine 会一直卡住
	a.shutdownRange(0, n)
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
	a.shutdownRange(fail+1, n)
	a.stopRange(0, fail)
	a.shutdownRange(0, fail)
	a.setTerminal()
}

// stopRange 逆序对 [lo, hi) 区间模块发出退出信号，不等待
func (a *App) stopRange(lo, hi int) {
	for i := hi - 1; i >= lo; i-- {
		a.modulesArr[i].Stop()
		a.logger.Info("module stopped", "app", a.name, "module", a.modulesArr[i].Name())
	}
}

// shutdownRange 逆序等待 [lo, hi) 区间模块退出并释放资源
func (a *App) shutdownRange(lo, hi int) {
	for i := hi - 1; i >= lo; i-- {
		a.modulesArr[i].Shutdown()
		a.logger.Info("module shutdown", "app", a.name, "module", a.modulesArr[i].Name())
	}
}

// setTerminal 把 App 置为终态：启动失败即进程结束，不再允许重试
func (a *App) setTerminal() {
	a.mu.Lock()
	a.state = lifecycleShutdown
	a.mu.Unlock()
}
