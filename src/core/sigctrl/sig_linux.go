//go:build linux

package sigctrl

import (
	"os"
	"syscall"

	"project/src/core/app"
)

// signals 列出本平台要监听的信号。
func signals() []os.Signal {
	return []os.Signal{syscall.SIGINT, syscall.SIGTERM, syscall.SIGHUP, syscall.SIGUSR1}
}

// mapSignal 把平台信号翻译成控制事件：
//
//	SIGINT / SIGTERM → 停服（Ctrl+C、kill 的默认信号）
//	SIGHUP           → 热更（重读配置的传统约定）
//	SIGUSR1          → 热更（备用，与 SIGHUP 等价）
//
// SIGUSR2 刻意不映射，留给将来（例如重启式更新）。
func mapSignal(s os.Signal) (app.Ctrl, bool) {
	switch s {
	case syscall.SIGINT, syscall.SIGTERM:
		return app.CtrlStop, true
	case syscall.SIGHUP, syscall.SIGUSR1:
		return app.CtrlReload, true
	}
	return 0, false
}
