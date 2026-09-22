package smoke

import (
	"context"
	"sync"
	"time"

	"project/src/core/app"
)

// pulse 演示第二种模块模板：中心化周期更新驱动，零 goroutine。
//
// 与 beat 系的模块对比着看：
//
//	beat  自己起 goroutine、自持 Ticker，Stop 发信号、Shutdown 等待。
//	      适合会阻塞或耗时不可控的活（网络、DB、等锁）。
//	pulse 没有 goroutine、没有 Ticker、没有 done/exited/wg，Stop 与 Shutdown
//	      都是空实现。活短且非阻塞时交给 app 主循环广播（app.Updater），
//	      调用顺序由拓扑序保证，还省掉一次跨 goroutine 同步。
//
// 选哪个的判据见 smoke.go 的包注释。
type pulse struct {
	app *app.App
	mu  sync.Mutex
	n   int
}

// newPulse 内部构造。
func newPulse(a *app.App) *pulse { return &pulse{app: a} }

// NewPulse 是登记到 app 的工厂。
func NewPulse(a *app.App) app.Module { return newPulse(a) }

// 编译期确认能力接口没写错。没有这行的话，方法签名一变模块就**静默**不再被
// Build 筛到 —— 表现为"这个模块的 Update 再也不被调"，而日志里只有一行
// modulesUpdater=[] 可查。加上它，签名对不上当场编译失败。
var _ app.Updater = (*pulse)(nil)

func (p *pulse) Name() string        { return "pulse" }
func (p *pulse) App() *app.App       { return p.app }
func (p *pulse) DependsOn() []string { return nil }
func (p *pulse) Init() error         { return nil }
func (p *pulse) Start() error        { return nil }

// Stop 与 Shutdown 是空实现：没有自有 goroutine 要叫停，也没有要等的。
// ctx 参数照签，但用不上。
func (p *pulse) Stop()                        {}
func (p *pulse) Shutdown(ctx context.Context) {}

// Update 由主循环广播，跑在 Run 的 goroutine 上 —— 与其余模块的 Update
// 以及 Stop/Shutdown 串行，所以这里改自身状态天然不需要跨 goroutine 同步。
// 加锁只是因为测试 goroutine 会并发读计数。
func (p *pulse) Update(now time.Time, updateCount uint64) {
	p.mu.Lock()
	p.n++
	n := p.n
	p.mu.Unlock()

	// updateCount 是主循环的轮次，只从这里拿到 —— 它没有 App 上的查询方法。
	p.app.Logger().Info("update", "module", p.Name(), "n", n, "updateCount", updateCount)
}

// count 返回已更新次数，供测试读取。
func (p *pulse) count() int {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.n
}
