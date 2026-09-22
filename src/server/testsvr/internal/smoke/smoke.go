// Package smoke 提供 testsvr 用的假模块：它们不做业务，只用最小的代价把
// app 的生命周期、依赖排序、主循环调度与并发退出链路在一个真实进程里跑一遍。
//
// 这里是三种模块模板，按活的触发方式选：
//
//	beat 系（ticker / heartbeat / worker）
//	    自己起 goroutine、自持 Ticker；Stop 发退出信号、Shutdown 等待结束，
//	    三件事分开。适合**会阻塞或耗时不可控**的活：网络 accept/recv、DB、
//	    文件、等锁、sleep、重试。
//
//	pulse（实现 app.Updater）
//	    零 goroutine，活儿交给主循环按固定节拍（默认 100ms）广播，顺序由拓扑序保证。
//	    适合**短、非阻塞、周期性**的活：超时扫描、统计聚合、批量 flush 的触发。
//	    需要独立于全局节拍的频率时不要用它 —— 要么留在 beat 里，要么在
//	    Update 内自己 n%k 分频。
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
// 一次 Update / Wake 卡住会同时拖住其他模块与停服响应 —— 这是协作式调度的固有代价，
// 所以后两种必须短。主循环有看门狗能发现卡住（默认 30s 一条 Error），但那只是事后
// 告警，不是保护。
package smoke

import (
	"context"
	"sync"
	"time"

	"project/src/core/app"
)

// beat 是三个假模块共用的骨架：定时打点 + 可被叫停 + 可被等待。
//
// 模块不自己定义 goroutine 的收发仪式，只能通过 extra 往日志行追加字段；
// 退出路径统一走 done 与 Context() 两条 select 分支。
type beat struct {
	app      *app.App
	name     string
	step     time.Duration
	extra    func() []any  // 可选：追加到每轮日志行的字段
	done     chan struct{} // 构造函数里创建，Stop 关闭
	exited   chan struct{} // goroutine 退出前关闭，供测试断言
	stopOnce sync.Once
	wg       sync.WaitGroup
	mu       sync.Mutex
	n        int
}

func newBeat(a *app.App, name string, step time.Duration, extra func() []any) beat {
	return beat{
		app:    a,
		name:   name,
		step:   step,
		extra:  extra,
		done:   make(chan struct{}),
		exited: make(chan struct{}),
	}
}

func (b *beat) Name() string        { return b.name }
func (b *beat) App() *app.App       { return b.app }
func (b *beat) DependsOn() []string { return nil }
func (b *beat) Init() error         { return nil }

// Start 起一个后台 goroutine 定时打点。
//
// 退出有两条路径：done 是主路径（Stop 发出的退出信号），Context() 只做兜底——
// 框架在 Stop 阶段末尾、所有模块都收到 Stop 之后才 cancel 进程 ctx，
// 模块不得依赖它完成自身的资源清理。
func (b *beat) Start() error {
	// WaitGroup.Go 内部自带 Done，这里只需保证 close(exited) 在函数返回前发生：
	// 于是 Shutdown（wg.Wait）返回时 exited 必然已关闭，测试可以非阻塞断言，不用 sleep。
	b.wg.Go(func() {
		defer close(b.exited)

		t := time.NewTicker(b.step)
		defer t.Stop()
		for {
			select {
			case <-b.done:
				return
			case <-b.app.Context().Done():
				return
			case <-t.C:
				b.tick()
			}
		}
	})
	return nil
}

// Stop 发出退出信号，不等待。
//
// stopOnce 让重复调用安全：close 一个已关闭的 channel 会 panic，
// 而这是要被抄 9 遍的模板，值得挡一道。
func (b *beat) Stop() { b.stopOnce.Do(func() { close(b.done) }) }

// Shutdown 等待 goroutine 真正退出。
//
// 用 wg 而不是 exited：exited 只代表那一个 goroutine 跑完了函数体，wg 才代表全部。
// 单 goroutine 时两者等价，但这是要抄 9 遍的模板，用 wg 更耐抄。
//
// ctx 给的是停服预算的剩余时间。预算耗尽时不再等 —— 模块自己的 goroutine 会随
// 进程退出被带走（框架不会替我们杀它，它只是不再等）。
func (b *beat) Shutdown(ctx context.Context) {
	stopped := make(chan struct{})
	go func() {
		defer close(stopped)
		b.wg.Wait()
	}()

	select {
	case <-stopped:
	case <-ctx.Done():
		b.app.Logger().Warn("module shutdown out of budget", "module", b.name)
	}
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
