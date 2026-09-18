// Package smoke 提供 testsvr 用的假模块：它们不做业务，只用最小的代价把
// app 的生命周期、依赖排序与并发退出链路在一个真实进程里跑一遍。
//
// 这里是模板：后面真实的 svr 模块照这个形状写——Start 起 goroutine、
// Stop 发退出信号、Shutdown 等待结束，三件事分开。
package smoke

import (
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
func (b *beat) Shutdown() { b.wg.Wait() }

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
