package sigctrl

import (
	"os"
	"syscall"
	"testing"

	"project/src/core/app"
)

// newTestHandler 造一个不真发信号、退出函数被换成记录的 handler。
func newTestHandler() (*handler, *[]int) {
	exits := &[]int{}
	h := &handler{
		out:  make(chan app.Ctrl, ctrlBuffer),
		exit: func(code int) { *exits = append(*exits, code) },
	}
	return h, exits
}

// TestUnmappedSignalIgnored 钉住：不认识的信号既不产生控制事件，也不触发退出。
func TestUnmappedSignalIgnored(t *testing.T) {
	h, exits := newTestHandler()

	// 0 号在任何平台的映射表里都不存在（SIGINT=2 / SIGTERM=15 / SIGHUP=1 / SIGUSR1=10）。
	h.onSignal(syscall.Signal(0))

	select {
	case c := <-h.out:
		t.Errorf("未映射的信号产生了控制事件 %v", c)
	default:
	}
	if len(*exits) != 0 {
		t.Errorf("未映射的信号触发了退出: %v", *exits)
	}
}

// TestSecondStopForcesExit 钉住二次强杀：第一次停服信号正常投递且不退出，
// 第二次直接强制退出。
//
// 这一层必须在主循环之外 —— 循环若卡在某个 Update / Reload 回调里，
// 它自己永远读不到第二个信号。
func TestSecondStopForcesExit(t *testing.T) {
	h, exits := newTestHandler()

	h.onSignal(os.Interrupt)

	select {
	case c := <-h.out:
		if c != app.CtrlStop {
			t.Fatalf("第一次停服信号产生了 %v, want CtrlStop", c)
		}
	default:
		t.Fatal("第一次停服信号没有产生控制事件")
	}
	if len(*exits) != 0 {
		t.Errorf("第一次停服信号就触发了退出: %v", *exits)
	}

	h.onSignal(os.Interrupt)
	if len(*exits) != 1 || (*exits)[0] != forceExitCode {
		t.Errorf("第二次停服信号的退出码 = %v, want [%d]", *exits, forceExitCode)
	}
}

// TestNotifyStopIsIdempotent 钉住 stop() 可以重复调用而不 panic。
// main 里通常写 defer stop()，但显式调用 + defer 并存是常见写法。
func TestNotifyStopIsIdempotent(t *testing.T) {
	_, stop := Notify()
	stop()
	stop()
}
