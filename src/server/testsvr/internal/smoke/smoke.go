// Package smoke 提供 testsvr 用的假模块：它们不做业务，只用最小的代价把
// app 的生命周期、依赖排序与主循环调度在一个真实进程里跑一遍。
//
// 这里是三种模块模板，按活的触发方式选：
//
//	beat 系（ticker / heartbeat / worker）
//	    自己起 goroutine、自持 Ticker。适合**会阻塞或耗时不可控**的活：
//	    网络 accept/recv、DB、文件、等锁、sleep、重试。
//
//	pulse（实现 app.Tickable）
//	    零 goroutine，活儿交给主循环按固定节拍（默认 30 帧）广播，顺序由拓扑序保证。
//	    适合**短、非阻塞、周期性**的活：超时扫描、统计聚合、批量 flush 的触发。
//	    需要独立于全局节拍的频率时不要用它 —— 要么留在 beat 里，要么在
//	    Tick 内自己 n%k 分频。
//
//	inbox（实现 app.Waker）
//	    零 goroutine，只在有人 Post 之后才被主循环调到 —— **没事件时完全不占主循环**。
//	    适合**事件驱动、但需要在主循环上串行处理**的活（想让自身状态免锁就得用它）。
//	    投递方 Post 进模块自己的队列后叫一次 app.Notify()。
//
// 判据两条，依次问：
//
//  1. 这一次的活会不会阻塞或耗时不可控？会 → beat。
//  2. 是周期来还是事件来？周期 → pulse；事件 → inbox。
//
// 一次 Tick / Wake 卡住会同时拖住其他模块与 ctx 取消的响应 —— 这是协作式调度的
// 固有代价，所以后两种必须短。框架对此**不做任何兜底**（没有上限，也没有告警），
// 自定上界是 Waker 契约的一部分。
package smoke

import (
	"sync"
	"time"

	"project/src/core/app"
)

// beat 是三个假模块共用的骨架：定时打点。
//
// 退出只有一条路径 —— App.Context() 在 Run 的主循环退出后 cancel，goroutine 随之返回。
// 模块不自己定义收发仪式，只能通过 extra 往日志行追加字段。
type beat struct {
	app    *app.App
	name   string
	step   time.Duration
	extra  func() []any  // 可选：追加到每轮日志行的字段
	exited chan struct{} // goroutine 退出前关闭，供测试断言
	mu     sync.Mutex
	n      int
}

func newBeat(a *app.App, name string, step time.Duration, extra func() []any) beat {
	return beat{
		app:    a,
		name:   name,
		step:   step,
		extra:  extra,
		exited: make(chan struct{}),
	}
}

func (b *beat) Name() string        { return b.name }
func (b *beat) App() *app.App       { return b.app }
func (b *beat) DependsOn() []string { return nil }
func (b *beat) Init() error         { return nil }

// Start 起一个后台 goroutine 定时打点。
//
// 退出路径是 App.Context()：框架在 Run 的主循环退出后才 cancel 它，所以这个
// goroutine 的生命周期比主循环长一点 —— 这是刻意的，那段尾巴用来做自己的收尾。
// 不依赖它做收尾的模块可以无视这条。
func (b *beat) Start() error {
	go func() {
		defer close(b.exited)

		t := time.NewTicker(b.step)
		defer t.Stop()
		for {
			select {
			case <-b.app.Context().Done():
				return
			case <-t.C:
				b.tick()
			}
		}
	}()
	return nil
}

// count 返回已打点次数，供兄弟模块读取。
func (b *beat) count() int {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.n
}

func (b *beat) tick() {
	b.mu.Lock()
	b.n++
	n := b.n
	b.mu.Unlock()

	attrs := []any{"module", b.name, "n", n}
	if b.extra != nil {
		attrs = append(attrs, b.extra()...)
	}
	b.app.Logger().Info("tick", attrs...)
}
