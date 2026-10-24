// testsvr 是 app 进程骨架的第一个消费者，本身不做业务：它把生命周期、
// 依赖排序与主循环调度在一个真实进程里跑一遍，并充当 src/server 下
// 其余 svr 的装配模板——main 只负责装配，模块实现在 internal/ 里。
package main

import (
	"context"
	"log/slog"
	"os"

	"project/src/core/app"
	"project/src/server/testsvr/internal/smoke"
)

func main() {
	// 不传 WithTickInterval：30 帧是框架的默认值，这里正好顺带把它跑一遍。
	a := app.New("testsvr")

	// 注册顺序是设计过的，不是随手排的。depsSort 是 Kahn 算法，每步取
	// 注册序号最小的就绪节点，于是上面这个注册顺序会产出：
	//
	//	heartbeat → ticker → worker → pulse → inbox
	//
	// worker 注册在最前（序号 0）却排在第三——依赖把它压住了（有依赖被强制重排）；
	// 而互不依赖的其余几个保持彼此的注册先后（无依赖保持注册序）。
	a.RegisterModule(smoke.NewWorker)
	a.RegisterModule(smoke.NewHeartbeat)
	a.RegisterModule(smoke.NewTicker)
	a.RegisterModule(smoke.NewPulse)
	a.RegisterModule(smoke.NewInbox)

	// Run 阻塞在 ctx 上，ctx 结束即返回。框架不做任何收尾 —— 进程怎么退、
	// 模块占的资源谁释放，都是这里的责任。
	//
	// ctx 传 Background 是当前状态的如实表达：本进程已经没有信号源，
	// 也没有别的取消源，只能被外部结束进程。
	if err := a.Run(context.Background()); err != nil {
		slog.Error("testsvr 退出异常", "err", err)
		os.Exit(1)
	}
}
