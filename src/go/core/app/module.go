package app

import (
	"time"
)

// Module 是一个可被 App 装配、调度并驱动生命周期的功能单元，给 APP 赋予功能扩充
// 模块通过工厂函数注册，App 在 Build 阶段把自身递进工厂，模块自行保存
type Module interface {
	Name() string
	App() *App
	Init() error
	Start() error
	DependsOn() []string // DependsOn 返回该模块依赖的模块名，Build 阶段据此做拓扑排序
}

// Tickable 由希望被主循环周期驱动的模块实现
//
// now 是一轮 tick 的时间基准；tickCount 是**周期 tick 跑过的次数**（从 1 开始，
// 不是循环轮次 —— 两次 tick 之间穿插的事件不会让它跳）。同一轮内所有模块拿到同一份。
//
// tickCount 只从这个参数给到：App 不提供查询它的方法。自起 goroutine 的模块不在主循环
// 的节奏上（随时可能被别的轮次推走），读了只会误导，那种模块要计数请自己记。
type Tickable interface {
	Tick(now time.Time, tickCount uint64)
}

// Waker 由需要被事件唤醒的模块实现。
//
// 与 Tickable 的区别在触发方式：Tickable 每轮周期被调，Waker 只在有人
// App.Notify() 之后被调。模块自持事件队列（v1 用一个带缓冲的 chan 就够），
// 投递方 Post 进队列后叫一次 Notify。
//
// 契约：
//   - Wake 跑在 Run 的 goroutine 上，与其他模块的 Wake、以及 Tick 串行，
//     所以模块内部状态不用加锁 —— 这是它相对于"自起 goroutine 处理事件"的全部价值；
//   - **单轮处理多少完全由模块自己定，框架不给上限**。但要清楚这等于把「这一轮占
//     多久」的决定权交给模块：占久了拖的是**所有人**，包括周期 tick 与 ctx 取消的
//     响应延迟 —— 主循环是单线程的，没有别的 worker 能替别人吸收（这点与 skynet
//     不同）。**框架不做任何兜底** —— 没有上限、也没有告警。所以自定上界是这个契约的
//     一部分，不是可以随便填的数；
//   - **返回 true 表示还有积压没排空**，App 会立刻再调度一轮。这个 bool 决定的是
//     "事件会不会被处理"，比单轮做多少更要紧。永远返回 true 会让主循环再也回不到
//     阻塞态，CPU 打满。
type Waker interface {
	Wake() bool
}
