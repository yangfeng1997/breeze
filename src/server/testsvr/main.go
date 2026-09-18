// testsvr 是 app 进程骨架的第一个消费者，本身不做业务：它把生命周期、
// 依赖排序与并发退出链路在一个真实进程里跑一遍，并充当 src/server 下
// 其余 svr 的装配模板——main 只负责装配与信号，模块实现在 internal/ 里。
package main

import (
	"context"
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	"project/src/core/app"
	"project/src/server/testsvr/internal/smoke"
)

func main() {
	// 信号由 main 提供，框架本身不监听信号。
	// Windows 上收不到 SIGTERM，Ctrl+C 走的是 os.Interrupt；留 SIGTERM 是为了
	// 将来跑 Linux 不用改代码。
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	a := app.New("testsvr")

	// 注册顺序是设计过的，不是随手排的。depsSort 是 Kahn 算法，每步取
	// 注册序号最小的就绪节点，于是上面这个注册顺序会产出：
	//
	//	heartbeat → ticker → worker
	//
	// worker 注册在最前（序号 0）却排到最后——依赖把它压住了（有依赖被强制重排）；
	// 而互不依赖的 heartbeat 与 ticker 保持彼此的注册先后（无依赖保持注册序）。
	a.RegisterModule(smoke.NewWorker)
	a.RegisterModule(smoke.NewHeartbeat)
	a.RegisterModule(smoke.NewTicker)

	// Run 阻塞直到 ctx 结束，然后自己走完 Stop → Shutdown。
	// 启动阶段失败会直接返回错误，此时框架已回滚完毕。
	if err := a.Run(ctx); err != nil {
		slog.Error("testsvr 启动失败", "err", err)
		os.Exit(1)
	}
}
