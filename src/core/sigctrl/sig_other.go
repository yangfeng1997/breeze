//go:build !linux

package sigctrl

import (
	"os"
	"syscall"

	"project/src/core/app"
)

// signals 列出本平台要监听的信号。
//
// Windows 上只有 Ctrl+C / Ctrl+Break 会被投递成 os.Interrupt。关闭控制台窗口、
// 注销、系统关机走的是 CTRL_CLOSE / CTRL_LOGOFF / CTRL_SHUTDOWN 事件，Go 运行时
// 把它们映射成 SIGTERM（并挂住进程一小段时间给清理留机会）—— 所以 SIGTERM 必须
// 留着，那正是「关窗口要优雅退出」的那条路径。
func signals() []os.Signal {
	return []os.Signal{os.Interrupt, syscall.SIGTERM}
}

// mapSignal 把平台信号翻译成控制事件。
//
// 热更在非 Linux 上没有信号可用（SIGUSR1 / SIGHUP 在 Windows 的 syscall 包常量表
// 里根本不存在），需要时由调试端口或控制台命令走同一条 Ctrl 通道。
func mapSignal(s os.Signal) (app.Ctrl, bool) {
	switch s {
	case os.Interrupt, syscall.SIGTERM:
		return app.CtrlStop, true
	}
	return 0, false
}
