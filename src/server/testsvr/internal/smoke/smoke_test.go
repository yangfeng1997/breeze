package smoke

import (
	"testing"
	"time"

	"project/src/core/app"
)

// waitFor 轮询直到 cond 为真，超时即失败。
//
// 一律用带 deadline 的轮询而不是固定 sleep：固定睡眠在慢机器上会假失败。
func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(time.Second)
	for !cond() {
		if time.Now().After(deadline) {
			t.Fatalf("等待超时：%s", what)
		}
		time.Sleep(time.Millisecond)
	}
}

// testApp 造一个 App 并注册两个快周期模块。
//
// 走的是生产那条构造路径（newTicker / newWorker），只是周期不同——
// 若测试自己搓结构体字面量，就会和真实装配漂移，漏挂 extra 钩子也没人发现。
func testApp(t *testing.T) (*app.App, *ticker, *worker) {
	t.Helper()
	a := app.New("smoke-test")
	tk := newTicker(a, 5*time.Millisecond)
	wk := newWorker(a, 5*time.Millisecond)
	a.RegisterModule(func(*app.App) app.Module { return tk })
	a.RegisterModule(func(*app.App) app.Module { return wk })
	return a, tk, wk
}

func start(t *testing.T, a *app.App) {
	t.Helper()
	if err := a.Build(); err != nil {
		t.Fatalf("Build() error = %v, want nil", err)
	}
	if err := a.Init(); err != nil {
		t.Fatalf("Init() error = %v, want nil", err)
	}
	if err := a.Start(); err != nil {
		t.Fatalf("Start() error = %v, want nil", err)
	}
}

// assertClosed 非阻塞地断言 ch 已关闭。
func assertClosed(t *testing.T, name string, ch <-chan struct{}) {
	t.Helper()
	select {
	case <-ch:
	default:
		t.Fatalf("Shutdown 返回后 %s 的 goroutine 仍在运行", name)
	}
}

// TestWorkerReadsTicker 验证真实模块能通过 App 拿到兄弟模块，并读到它的实时状态。
//
// 它证明的是"依赖能被满足"：worker.Init 取到了 ticker，双方 goroutine 都跑起来了。
// 它**不**证明拓扑序——modulesMap 在 Build 阶段就已填好，GetModule 拿得到与 Init
// 先后无关。顺序由 app 包自己的 TestLifecycleOrder / TestDepsSort 钉住，
// 端到端所见则是 testsvr 启动日志里的 module inited 顺序。
func TestWorkerReadsTicker(t *testing.T) {
	a, tk, wk := testApp(t)
	start(t, a)
	defer func() { a.Stop(); a.Shutdown() }()

	waitFor(t, "ticker 打点", func() bool { return tk.count() > 0 })
	waitFor(t, "worker 打点", func() bool { return wk.count() > 0 })
}

// TestShutdownStopsGoroutines 钉住退出契约：Shutdown 返回时 goroutine 确已退出。
//
// Shutdown 只 close(done) 而不 wg.Wait() 就是 goroutine 泄漏——进程退出时可能
// 看不出问题，但真实模块会丢数据、丢连接。这是模板里最容易抄错的一处。
func TestShutdownStopsGoroutines(t *testing.T) {
	a, tk, wk := testApp(t)
	start(t, a)

	// 先确认两个 goroutine 都真在跑，再断言它们停了：
	// 否则 goroutine 根本没起来，用例也会通过，等于没测。
	waitFor(t, "ticker 打点", func() bool { return tk.count() > 0 })
	waitFor(t, "worker 打点", func() bool { return wk.count() > 0 })

	a.Stop()
	a.Shutdown()

	assertClosed(t, "ticker", tk.exited)
	assertClosed(t, "worker", wk.exited)
}
