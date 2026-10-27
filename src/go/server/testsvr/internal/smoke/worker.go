package smoke

import (
	"fmt"
	"time"

	"project/core/app"
)

// worker 依赖 ticker，用来验证"依赖声明 → 拓扑序 → 按名取到兄弟模块"整条链路。
type worker struct {
	beat
	ticker *ticker
}

// newWorker 内部构造，周期可指定，供测试用快周期驱动。
//
// extra 闭包捕获 w 本身，w.ticker 要等 Init 才赋值——但钩子只在 Start 之后
// 被 tick 调用，那时 Init 早已跑完；Init 失败则模块永不 Start，钩子不会被调到。
func newWorker(a *app.App, step time.Duration) *worker {
	w := &worker{}
	w.beat = newBeat(a, "worker", step, func() []any {
		return []any{"ticker", w.ticker.count()}
	})
	return w
}

// NewWorker 是登记到 app 的工厂。
func NewWorker(a *app.App) app.Module { return newWorker(a, 500*time.Millisecond) }

// DependsOn 声明对 ticker 的依赖，depsSort 据此把它排到 ticker 之后。
func (w *worker) DependsOn() []string { return []string{"ticker"} }

// Init 按名取兄弟模块。
//
// 模块之间不互相 import，只通过 App 拿——这是真实模块之间唯一的协作方式。
// 若取不到或类型不符就返回错误，框架就此终止启动，永不会 Start 本模块。
func (w *worker) Init() error {
	m := w.app.GetModule("ticker")
	if m == nil {
		return fmt.Errorf("找不到依赖的 ticker 模块")
	}
	t, ok := m.(*ticker)
	if !ok {
		return fmt.Errorf("ticker 模块类型意外: %T", m)
	}
	w.ticker = t
	return nil
}
