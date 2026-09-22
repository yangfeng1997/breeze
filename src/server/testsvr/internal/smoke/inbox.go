package smoke

import (
	"context"
	"sync"

	"project/src/core/app"
)

const (
	// inboxQueueSize 是事件队列容量，决定入站背压阈值。满了 Post 会阻塞，
	// 把压力回压给投递方 —— 这是入口背压，与"消费端有预算"是两件事。
	inboxQueueSize = 256

	// inboxBudgetCap 是本模块给自己定的单轮上限。框架不给上限（见 app.Waker 的契约），
	// 所以这个值就是全部的节奏控制 —— 取小了留给别人和停服信号响应时间，
	// 取大了吞吐高但拖长这一轮。
	inboxBudgetCap = 8
)

// inbox 演示第三种模块模板：事件驱动，在主循环上处理。
//
// 与另外两种对照：
//
//	beat   自持 goroutine + Ticker，活的会阻塞时用它
//	pulse  实现 app.Updater，被周期广播，短的非阻塞周期活用它
//	inbox  实现 app.Waker，只在有人 Post 之后才被调 —— 没事件时完全不占主循环
type inbox struct {
	app *app.App
	ch  chan int // 事件队列
	mu  sync.Mutex
	n   int // 已处理条数，供测试读
}

func newInbox(a *app.App) *inbox {
	return &inbox{app: a, ch: make(chan int, inboxQueueSize)}
}

// NewInbox 是登记到 app 的工厂。
func NewInbox(a *app.App) app.Module { return newInbox(a) }

// 编译期确认能力接口没写错 —— 理由同 pulse.go 里那行。
var _ app.Waker = (*inbox)(nil)

func (m *inbox) Name() string                 { return "inbox" }
func (m *inbox) App() *app.App                { return m.app }
func (m *inbox) DependsOn() []string          { return nil }
func (m *inbox) Init() error                  { return nil }
func (m *inbox) Start() error                 { return nil }
func (m *inbox) Stop()                        {}
func (m *inbox) Shutdown(ctx context.Context) {}

// Post 可在任意 goroutine 调用：入队后叫醒主循环。
// 队列满时会阻塞在这里 —— 背压回压给投递方，通常是网络读协程。
func (m *inbox) Post(ev int) {
	m.ch <- ev
	m.app.Notify()
}

// Wake 跑在主循环 goroutine 上，每轮至多处理 inboxBudgetCap 条。
//
// 上界是本模块自己定的：它知道自己处理一条要多久，框架不知道。代价是这也等于
// 由我决定这一轮占多久 —— 取大了拖的是所有人，所以这个值是契约的一部分。
//
// 返回"还有没有"，用 len(ch) 精确判断，而不是猜"是不是吃满了 cap" —— 后者在
// "恰好处理 cap 条但队列刚好空了"时会假阳性，让主循环白转一圈。
func (m *inbox) Wake() bool {
	var n int
drain:
	for n < inboxBudgetCap {
		select {
		case <-m.ch:
			n++
		default:
			break drain // 排空了
		}
	}

	m.mu.Lock()
	m.n += n
	m.mu.Unlock()

	return len(m.ch) > 0
}

// count 返回已处理条数，供测试读。
func (m *inbox) count() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.n
}
