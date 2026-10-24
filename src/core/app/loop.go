// 本文件是 App 的主循环，以及主循环驱动的能力列表。

package app

import (
	"context"
	"time"
)

// defaultTickInterval 是主循环周期 tick 的默认间隔：30 帧。
//
// 用 time.Second/30（33.333ms）而不是 33*time.Millisecond —— 后者是 NFShmXFrame 的
// 整数除法写法，30 帧只覆盖 990ms，实际帧率 30.3。
//
// **代价**：这个循环是协作式的，全部 Tickable 模块在同一轮里串行跑，一轮超时
// `time.Ticker` 就丢 tick（tickCount 会落后于时间，见 runLoop 的注释）。33ms 的每轮
// 预算比粗粒度周期活时代的 100ms 紧三倍，所以 Tick 必须短且不阻塞 ——
// 占久了丢的是所有人的帧，不只是自己的。
const defaultTickInterval = time.Second / 30

// runLoop 是 App 的主循环：平时零 CPU 阻塞在 select 上，被唤醒或周期驱动。
//
// 它跑在 Run 自己的 goroutine 上，不是另起一条 —— 这是刻意的：循环返回之后 Run
// 直接 cancel 并返回，没有任何后续阶段要并行，于是 Tick / Wake 与调用方的收尾
// 逻辑在时间上不可能重叠，「纯串行编排器」这个性质由这条 goroutine 边界保住。
//
// 注意 for 条件与 select 里的 ctx.Done() 是并存的：ctx 取消那一刻若 select 恰好
// 选中了别的就绪分支，会多跑一轮。它发生在 Run 返回之前，所以是安全的 ——
// 但也因此**不能断言「cancel 之后一次都不再跑」**。
func (a *App) runLoop(ctx context.Context) {
	// 没有 Tickable 模块时保持 nil：nil channel 在 select 里永久阻塞，
	// 于是不必为"开一个永远没人用的 ticker"付空转代价。
	var tickCh <-chan time.Time
	if a.tickInterval > 0 && len(a.tickableModules) > 0 {
		t := time.NewTicker(a.tickInterval)
		defer t.Stop()
		tickCh = t.C
	}

	for ctx.Err() == nil {
		select {
		case <-ctx.Done():
			return

		case <-a.wakeCh:
			if a.modulesWakeAll() {
				// 还有积压 → 自己补一个 token 立刻再转一圈。不能指望生产者的
				// 下一次 Notify：它可能早就发过了，那个 token 已经被这一轮消费掉。
				//
				// 走 select 而不是 continue，是为了让周期 tick 在这期间仍有机会被
				// 选中 —— 否则一个持续积压的模块会把别的分支饿死。
				a.Notify()
			}

		case now := <-tickCh:
			// tickCount 只在这里自增：它数的是**周期 tick 跑过多少次**，不是 while 转了多少圈。
			// 于是两次 Tick 之间无论穿插多少事件，tickCount 都是连着的 ——
			// 模块按 tickCount % N 分频才有意义，tickCount × tick 间隔 也才近似等于运行时长。
			a.tickCount++
			a.modulesTickAll(now, a.tickCount)
		}
	}
}

// modulesWakeAll 把一次唤醒交给全部 Waker 模块。单轮处理多少由模块自己定，框架不设上限 ——
// 权衡见 Waker 的契约（占久了拖的是所有人）。
// 顺序 = 拓扑序 = Init 顺序，与 modulesTickAll 一致。
//
// 返回 true 表示还有模块报积压，调用方据此再 Notify 一轮。
func (a *App) modulesWakeAll() bool {
	var more bool
	for _, m := range a.wakeableModules {
		// 不带 ok 的断言：容器只由 Build 里那段筛选产出，元素必然实现 Waker。
		// 即使已经知道 more，也必须把剩下的模块都问一遍 —— 每个都有自己的队列。
		if m.(Waker).Wake() {
			more = true
		}
	}
	return more
}

// modulesTickAll 把一次 tick 广播给 Build 阶段冻结下来的全部 Tickable 模块。
//
// 顺序 = 收集顺序 = 拓扑序 = Init 顺序：tick 是数据流向下游的推力，与 Init 同向；
// 反过来只会让"依赖者的 tick 跑在被依赖者之前"。
func (a *App) modulesTickAll(now time.Time, tickCount uint64) {
	for _, m := range a.tickableModules {
		// 不带 ok 的断言：容器只由 Build 里那段筛选产出，元素必然实现 Tickable。
		m.(Tickable).Tick(now, tickCount)
	}
}
