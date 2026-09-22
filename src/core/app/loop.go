// 本文件是 App 的主循环，以及主循环驱动的能力列表。

package app

import (
	"context"
	"strconv"
	"time"
)

// defaultUpdateInterval 是主循环周期更新的默认间隔。
//
// 100ms 对应 10Hz —— 这个循环是给「超时扫描、心跳、批量 flush」这类粗粒度周期活
// 用的协作调度器，不是帧循环。真需要每帧推进的活请用模块自有 goroutine。
const defaultUpdateInterval = 100 * time.Millisecond

// ctrlBuffer 是内部控制事件通道的缓冲深度。
// 停服/热更是边沿事件，缓冲只为吸收"信号连发时主循环还没醒"的抖动。
const ctrlBuffer = 8

// Ctrl 是投递给主循环的进程级控制事件。
//
// 从 1 开始编号是刻意的：Ctrl 的零值非法，于是万一有来源把已关闭的通道接进来，
// 读到的零值不会被误当成停服。
type Ctrl uint8

const (
	CtrlStop    Ctrl = iota + 1 // 停服：Run 走完 Stop → Shutdown 后返回
	CtrlReload                  // 热更：按拓扑序依次调 Reloader.Reload
	CtrlRestart                 // 重启式更新：优雅退出，由外部编排器拉起新进程
)

func (c Ctrl) String() string {
	switch c {
	case CtrlStop:
		return "stop"
	case CtrlReload:
		return "reload"
	case CtrlRestart:
		return "restart"
	default:
		return "unknown(" + strconv.Itoa(int(c)) + ")"
	}
}

// runLoop 是 App 的主循环：平时零 CPU 阻塞在 select 上，被事件唤醒。
//
// 它跑在 Run 自己的 goroutine 上，不是另起一条 —— 这是刻意的：循环返回之后 Run
// 才开始 Stop，于是 Update / Reload 与 Stop / Shutdown 在时间上不可能重叠，
// 框架不需要为此加任何锁，「纯串行编排器」这个性质保住了。
//
// 注意 for 条件与 select 里的 ctx.Done() 是并存的：ctx 取消那一刻若 select 恰好
// 选中了别的就绪分支，会多跑一轮。它发生在 Stop 之前，所以是安全的 ——
// 但也因此**不能断言「cancel 之后一次都不再跑」**。
func (a *App) runLoop(ctx context.Context) {
	// 没有 Updater 模块时保持 nil：nil channel 在 select 里永久阻塞，
	// 于是不必为"开一个永远没人用的 ticker"付空转代价。
	var updateCh <-chan time.Time
	if a.updateInterval > 0 && len(a.modulesUpdater) > 0 {
		t := time.NewTicker(a.updateInterval)
		defer t.Stop()
		updateCh = t.C
	}

	// src 可能为 nil（没注入控制源）：nil channel 在 select 里永久阻塞。
	src := a.ctrlSource

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
				// 来源被关闭：摘掉这条 case。不摘的话读端会拿到 Ctrl(0) 零值
				// 疯狂空转把 CPU 打满 —— 这也是 Ctrl 从 1 开始编号的原因。
				src = nil
				continue
			}
			if a.handleCtrl(c) {
				return
			}

		case <-a.wakeCh:
			if a.modulesWakeAll() {
				// 还有积压 → 自己补一个 token 立刻再转一圈。不能指望生产者的
				// 下一次 Notify：它可能早就发过了，那个 token 已经被这一轮消费掉。
				//
				// 走 select 而不是 continue，是为了让控制事件和周期更新在这期间
				// 仍有机会被选中 —— 否则一个持续积压的模块会把停服信号饿死。
				a.Notify()
			}

		case now := <-updateCh:
			// updateCount 只在这里自增：它数的是**周期更新跑过多少次**，不是 while 转了多少圈。
			// 于是两次 Update 之间无论穿插多少事件，updateCount 都是连着的 ——
			// 模块按 updateCount % N 分频才有意义，updateCount × 更新间隔 也才近似等于运行时长。
			a.updateCount++
			a.modulesUpdateAll(now, a.updateCount)
		}
	}
}

// modulesWakeAll 把一次唤醒交给全部 Waker 模块。单轮处理多少由模块自己定，框架不设上限 ——
// 权衡见 Waker 的契约（占久了拖的是所有人）。
// 顺序 = 拓扑序 = Init 顺序，与 modulesUpdateAll 一致。
//
// 返回 true 表示还有模块报积压，调用方据此再 Notify 一轮。
func (a *App) modulesWakeAll() bool {
	var more bool
	for _, m := range a.modulesWaker {
		// 不带 ok 的断言：容器只由 Build 里那段筛选产出，元素必然实现 Waker。
		// 即使已经知道 more，也必须把剩下的模块都问一遍 —— 每个都有自己的队列。
		if m.(Waker).Wake() {
			more = true
		}
	}
	return more
}

// handleCtrl 分发一个控制事件，返回 true 表示主循环应退出。
//
// 它跑在 Run 的 goroutine 上，与 Update / Wake / Stop / Shutdown 串行，不需要加锁。
func (a *App) handleCtrl(c Ctrl) (exit bool) {
	switch c {
	case CtrlStop:
		return true

	case CtrlReload:
		a.modulesReloadAll()
		return false

	case CtrlRestart:
		// 重启式更新是 Go 里代码更新的现实路径：优雅退出，由外部编排器
		// （systemd / k8s / startall.sh）拉起新进程。与 CtrlStop 走同一条退出路径。
		a.logger.Info("restart requested, exiting for the supervisor to relaunch", "app", a.name)
		return true

	default:
		// 不认识的事件不致死循环，也不当成停服 —— 只记一条日志。
		a.logger.Warn("unknown control event", "app", a.name, "ctrl", uint8(c))
		return false
	}
}

// modulesUpdateAll 把一次更新广播给 Build 阶段冻结下来的全部 Updater 模块。
//
// 顺序 = 收集顺序 = 拓扑序 = Init 顺序：更新是数据流向下游的推力，与 Init 同向；
// 反过来只会让"依赖者的更新跑在被依赖者之前"。
func (a *App) modulesUpdateAll(now time.Time, updateCount uint64) {
	for _, m := range a.modulesUpdater {
		// 不带 ok 的断言：容器只由 Build 里那段筛选产出，元素必然实现 Updater。
		m.(Updater).Update(now, updateCount)
	}
}

// modulesReloadAll 按拓扑序依次热更全部 Reloader。
//
// 顺序取正向拓扑序（与 Stop 的逆序相反）：热更是「重新配置」而不是「拆解」，
// 配置提供者应当先换新，消费者再用新的；逆序会让消费者读到半新半旧的配置。
//
// 任一模块失败只记日志、继续调用其余模块，不回滚也不停服 —— 活着的进程没法
// 回滚「部分生效的配置」，停服也解决不了它。
func (a *App) modulesReloadAll() {
	var failed int
	for _, m := range a.modulesReloader {
		// 不带 ok 的断言：容器只由 Build 里那段筛选产出，元素必然实现 Reloader。
		if err := m.(Reloader).Reload(); err != nil {
			failed++
			a.logger.Error("module reload failed", "app", a.name, "module", m.Name(), "err", err)
			continue
		}
		a.logger.Info("module reloaded", "app", a.name, "module", m.Name())
	}

	if failed > 0 {
		a.logger.Warn("reload finished with errors", "app", a.name,
			"ok", len(a.modulesReloader)-failed, "failed", failed)
		return
	}
	a.logger.Info("reload done", "app", a.name, "modules", len(a.modulesReloader))
}
