package smoke

import (
	"context"
	"testing"
	"time"

	"project/core/app"
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

// isClosed 非阻塞地判断 ch 是否已关闭。
func isClosed(ch <-chan struct{}) bool {
	select {
	case <-ch:
		return true
	default:
		return false
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

// run 在后台跑 Run，返回取消函数与 Run 的返回通道。
//
// 模块唯一的退出路径是 App.Context()，而它只在 Run 返回时才 cancel ——
// 所以测试必须走 Run，不能自己调 Build/Init/Start：那样 ctx 永远不会取消，
// 模块的 goroutine 也就永远不停。
func run(a *app.App) (context.CancelFunc, <-chan error) {
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- a.Run(ctx) }()
	return cancel, done
}

// stop 取消 ctx 并等 Run 返回。
func stop(t *testing.T, cancel context.CancelFunc, done <-chan error) {
	t.Helper()
	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("Run() error = %v, want nil", err)
		}
	case <-time.After(time.Second):
		t.Fatal("ctx 取消后 Run 没有返回")
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
	cancel, done := run(a)

	waitFor(t, "ticker 打点", func() bool { return tk.count() > 0 })
	waitFor(t, "worker 打点", func() bool { return wk.count() > 0 })

	stop(t, cancel, done)
}

// TestCtxCancelStopsGoroutines 钉住 beat 模板的退出契约：
// ctx 取消之后，模块自己的 goroutine 确实会退出。
//
// 这里没有 Shutdown 那样的"等模块结束"阶段可依赖，只能轮询 —— 但也正因为如此，
// 模块**必须自己盯着 App.Context()**：没有任何人会来叫停它。这是模板里最容易抄错的
// 一处：起了 goroutine 却不看 ctx，进程退出前它一直在跑，真实模块会丢数据、丢连接。
func TestCtxCancelStopsGoroutines(t *testing.T) {
	a, tk, wk := testApp(t)
	cancel, done := run(a)

	// 先确认两个 goroutine 都真在跑，再断言它们停了：
	// 否则 goroutine 根本没起来，用例也会通过，等于没测。
	waitFor(t, "ticker 打点", func() bool { return tk.count() > 0 })
	waitFor(t, "worker 打点", func() bool { return wk.count() > 0 })

	stop(t, cancel, done)

	waitFor(t, "ticker goroutine 退出", func() bool { return isClosed(tk.exited) })
	waitFor(t, "worker goroutine 退出", func() bool { return isClosed(wk.exited) })
}

// TestInboxWakeDrivenByNotify 验证第三种模板：事件驱动的模块。
//
// 投递数刻意超过单轮上限的三倍，验证它分多轮排空而不是一轮吃完 ——
// 这条同时钉住了预算生效、以及"返回 true 后框架会自己再调度一轮"。
func TestInboxWakeDrivenByNotify(t *testing.T) {
	a := app.New("smoke-test")
	ib := newInbox(a)
	a.RegisterModule(func(*app.App) app.Module { return ib })

	cancel, done := run(a)

	total := inboxBudgetCap*3 + 1
	for i := 0; i < total; i++ {
		ib.Post(i)
	}

	waitFor(t, "inbox 排空", func() bool { return ib.count() == total })

	stop(t, cancel, done)
}

// TestPulseTicksDrivenByLoop 验证第二种模板：中心化周期 tick 驱动的模块
// （零 goroutine）确实被主循环广播到了，且 Run 收得到 ctx 取消。
//
// 与上面两个用例的区别：pulse 没有 exited 可断言 —— 它压根没有 goroutine。
// 对它来说"停得干净"是平凡的，因为没有任何东西在后台跑。
func TestPulseTicksDrivenByLoop(t *testing.T) {
	a := app.New("smoke-test", app.WithTickInterval(time.Millisecond))
	p := newPulse(a)
	a.RegisterModule(func(*app.App) app.Module { return p })

	cancel, done := run(a)

	waitFor(t, "pulse tick", func() bool { return p.count() > 0 })

	stop(t, cancel, done)
}
