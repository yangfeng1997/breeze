// Package sigctrl 把操作系统信号翻译成 app.Ctrl，供 app 的主循环消费。
//
// 独立成包是刻意的：app 只做编排，不 import os/signal。各 svr 的 main 只做两件事：
// 用 sigctrl.Notify() 拿到通道，塞给 app.WithCtrl。
package sigctrl

import (
	"os"
	"os/signal"
	"sync"

	"project/src/core/app"
)

const (
	// ctrlBuffer 是控制事件通道的缓冲深度。停服/热更是边沿事件，
	// 缓冲只为吸收"信号连发时主循环还没醒"的抖动。
	ctrlBuffer = 8

	// rawBuffer 是原始 os.Signal 通道的缓冲深度。
	rawBuffer = 16
)

// forceExitCode 是二次停服信号触发强制退出时的退出码。
//
// 用 1 而不是 0：让编排器（systemd / k8s / startall.sh）能区分"优雅退出"与"被强杀"。
const forceExitCode = 1

// handler 把信号映射成控制事件，并负责「第二次停服信号强制退出」。
//
// 强杀逻辑放在这里、不放在主循环里：循环若正卡在某个 Update / Reload 回调里，
// 它自己永远读不到第二个信号；放在信号 goroutine 里就一定生效。
type handler struct {
	out  chan app.Ctrl
	exit func(int)

	mu       sync.Mutex
	stopSeen bool
}

// Option 用于定制 Notify 的行为。
type Option func(*handler)

// WithExit 替换退出函数，仅供测试用（默认为 os.Exit）。
func WithExit(f func(int)) Option {
	return func(h *handler) {
		if f != nil {
			h.exit = f
		}
	}
}

// Notify 开始监听进程信号，返回控制事件通道与停止函数。
//
// 返回的通道由本包持有、永不关闭：app 只读不关，关闭会让读端读到非法零值。
// main 必须在退出前 defer stop()；stop 之后这些信号恢复默认处理。
func Notify(opts ...Option) (<-chan app.Ctrl, func()) {
	h := &handler{
		out:  make(chan app.Ctrl, ctrlBuffer),
		exit: os.Exit,
	}
	for _, o := range opts {
		o(h)
	}

	raw := make(chan os.Signal, rawBuffer)
	signal.Notify(raw, signals()...)

	go func() {
		for sig := range raw {
			h.onSignal(sig)
		}
	}()

	var once sync.Once
	return h.out, func() {
		once.Do(func() {
			// signal.Stop 的语义保证：它返回之后 raw 不会再收到投递，
			// 所以紧接着的 close 不会撞上并发发送。
			signal.Stop(raw)
			close(raw)
		})
	}
}

// onSignal 处理一个原始信号。未映射的信号直接忽略。
func (h *handler) onSignal(sig os.Signal) {
	c, ok := mapSignal(sig)
	if !ok {
		return
	}

	if c == app.CtrlStop {
		h.mu.Lock()
		already := h.stopSeen
		h.stopSeen = true
		h.mu.Unlock()

		if already {
			// 第二次停服信号：不再等。此刻主循环多半卡在某个回调里，
			// 或者停服预算已经耗尽 —— 无论如何，这是最后一道兜底。
			os.Stderr.WriteString("sigctrl: received a second stop signal, forcing exit\n")
			h.exit(forceExitCode)
			return
		}
	}

	select {
	case h.out <- c:
	default:
		// 缓冲满了：主循环已经被卡住。不阻塞自己 —— 交给第二次信号收场。
	}
}
