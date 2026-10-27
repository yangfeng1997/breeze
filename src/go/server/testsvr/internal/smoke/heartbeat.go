package smoke

import (
	"time"

	"project/core/app"
)

// heartbeat 无依赖，周期与 ticker 错开，便于在日志里分辨两者。
type heartbeat struct {
	beat
}

// newHeartbeat 内部构造，周期可指定，供测试用快周期驱动。
func newHeartbeat(a *app.App, step time.Duration) *heartbeat {
	return &heartbeat{beat: newBeat(a, "heartbeat", step, nil)}
}

// NewHeartbeat 是登记到 app 的工厂。
func NewHeartbeat(a *app.App) app.Module { return newHeartbeat(a, 700*time.Millisecond) }
