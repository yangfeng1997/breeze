package app

// lifecycleState 标记 App 整体的推进位置，用于拒绝错序调用
type lifecycleState uint8

const (
	lifecycleNew     lifecycleState = iota // 已创建，未 Build
	lifecycleBuilt                         // Build 完成
	lifecycleInited                        // Init 完成
	lifecycleStarted                       // Start 完成 —— 终态
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
	default:
		return "unknown"
	}
}

// Init 按拓扑序依次初始化全部模块，遇错即返回。
//
// **不回滚**：出错模块之前的那些模块不会收到任何通知，框架也不释放它们 Init 期
// 占用的资源。启动阶段只有"往前走"这一种状态推进，不维护逐模块的已初始化账本。
func (a *App) Init() error {
	a.mu.Lock()
	if a.state != lifecycleBuilt {
		a.mu.Unlock()
		return a.stateErr("init", lifecycleBuilt)
	}
	a.state = lifecycleInited
	modulesArr := a.modulesArr
	a.mu.Unlock()

	for _, m := range modulesArr {
		if err := m.Init(); err != nil {
			return a.moduleErr("init", m.Name(), err)
		}
		a.logger.Info("module inited", "app", a.name, "module", m.Name())
	}
	return nil
}

// Start 按拓扑序依次启动全部模块，遇错即返回。
//
// 同样**不回滚**，而且比 Init 更需要小心：已经 Start 的模块可能已经起了 goroutine、
// 开了监听，**框架不会叫停它们**。返回错误的那个模块由 Module.Start 的约定负责
// 自行清理，其余模块随进程退出被带走。
func (a *App) Start() error {
	a.mu.Lock()
	if a.state != lifecycleInited {
		a.mu.Unlock()
		return a.stateErr("start", lifecycleInited)
	}
	a.state = lifecycleStarted
	modulesArr := a.modulesArr
	a.mu.Unlock()

	for _, m := range modulesArr {
		if err := m.Start(); err != nil {
			return a.moduleErr("start", m.Name(), err)
		}
		a.logger.Info("module started", "app", a.name, "module", m.Name())
	}
	return nil
}
