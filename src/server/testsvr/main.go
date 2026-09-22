// testsvr 是 app 进程骨架的第一个消费者，本身不做业务：它把生命周期、
// 依赖排序与并发退出链路在一个真实进程里跑一遍，并充当 src/server 下
// 其余 svr 的装配模板——main 只负责装配与信号，模块实现在 internal/ 里。
package main

import (
	"context"
	"log/slog"
	"os"
	"time"

	"project/src/core/app"
	"project/src/core/sigctrl"
	"project/src/server/testsvr/internal/smoke"
)

func main() {
	// 信号由 srv 的 main 负责，框架不 import os/signal —— 分工没变，
	// 只是「翻译成什么」从 ctx 变成了 app.Ctrl。
	//
	// 不要再叠加 signal.NotifyContext：两套信号处理会抢第一个信号，而且
	// NotifyContext 在首次信号后就撤掉自己，二次强杀那条兜底就没人管了。
	ctrl, stop := sigctrl.Notify()
	defer stop()

	a := app.New("testsvr",
		app.WithCtrl(ctrl),
		// 30Hz。注意用 time.Second/30（33.333ms）而不是 33*time.Millisecond ——
		// 后者是 NFShmXFrame 的整数除法写法，30 帧只覆盖 990ms，实际帧率 30.3。
		app.WithUpdateInterval(time.Second/30),
		app.WithShutdownTimeout(10*time.Second),
	)

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

	// Run 阻塞直到 ctx 结束或收到停服控制事件，然后自己走完 Stop → Shutdown。
	// 启动阶段失败会直接返回错误，此时框架已回滚完毕；停服阶段有模块超出预算
	// 被放弃则返回 ErrShutdownTimeout。
	//
	// ctx 在这里只是「调用方自己的取消源」，与 CtrlStop 等价——main 没有别的来源。
	if err := a.Run(context.Background()); err != nil {
		slog.Error("testsvr 退出异常", "err", err)
		os.Exit(1)
	}
}
