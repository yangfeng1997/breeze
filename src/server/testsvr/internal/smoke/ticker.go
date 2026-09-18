package smoke

import (
	"time"

	"project/src/core/app"
)

// ticker 无依赖，只做定时打点，并把自己的计数留给兄弟模块读。
type ticker struct {
	beat
}

// newTicker 内部构造，周期可指定，供测试用快周期驱动。
func newTicker(a *app.App, step time.Duration) *ticker {
	return &ticker{beat: newBeat(a, "ticker", step, nil)}
}

// NewTicker 是登记到 app 的工厂，签名正好是 app.Register 要的形状。
func NewTicker(a *app.App) app.Module { return newTicker(a, 500*time.Millisecond) }
