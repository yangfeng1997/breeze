//go:build linux

package sigctrl

import (
	"syscall"
	"testing"

	"project/src/core/app"
)

// TestLinuxSignalMapping 钉住 Linux 上的信号映射表。
func TestLinuxSignalMapping(t *testing.T) {
	cases := []struct {
		sig  syscall.Signal
		want app.Ctrl
	}{
		{syscall.SIGINT, app.CtrlStop},
		{syscall.SIGTERM, app.CtrlStop},
		{syscall.SIGHUP, app.CtrlReload},
		{syscall.SIGUSR1, app.CtrlReload},
	}

	for _, c := range cases {
		got, ok := mapSignal(c.sig)
		if !ok {
			t.Errorf("mapSignal(%v) 未被映射, want %v", c.sig, c.want)
			continue
		}
		if got != c.want {
			t.Errorf("mapSignal(%v) = %v, want %v", c.sig, got, c.want)
		}
	}

	// SIGUSR2 刻意留空 —— 留给将来（例如重启式更新）。
	if _, ok := mapSignal(syscall.SIGUSR2); ok {
		t.Error("SIGUSR2 不该被映射")
	}
}

// TestReloadSignalDoesNotArmForceExit 钉住：热更信号不参与「第二次就强杀」的计数。
// 连发两次热更必须产生两个 CtrlReload，而不是第二次把人踢下线。
func TestReloadSignalDoesNotArmForceExit(t *testing.T) {
	h, exits := newTestHandler()

	h.onSignal(syscall.SIGHUP)
	h.onSignal(syscall.SIGHUP)

	for i := 0; i < 2; i++ {
		select {
		case c := <-h.out:
			if c != app.CtrlReload {
				t.Fatalf("第 %d 次热更信号产生了 %v, want CtrlReload", i+1, c)
			}
		default:
			t.Fatalf("第 %d 次热更信号没有产生控制事件", i+1)
		}
	}

	if len(*exits) != 0 {
		t.Errorf("热更信号触发了退出: %v", *exits)
	}
}

// TestStopAfterReloadStillArmsForceExit 钉住：热更信号不会消耗掉那一次停服额度。
func TestStopAfterReloadStillArmsForceExit(t *testing.T) {
	h, exits := newTestHandler()

	h.onSignal(syscall.SIGHUP) // 热更先来
	h.onSignal(syscall.SIGTERM)
	if len(*exits) != 0 {
		t.Fatalf("第一次停服信号就退出了: %v", *exits)
	}

	h.onSignal(syscall.SIGTERM)
	if len(*exits) != 1 {
		t.Errorf("第二次停服信号没有强制退出: %v", *exits)
	}
}
