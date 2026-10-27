// App 的构造选项：只放 App 自己运转要用的东西。
// 业务配置、命令行参数不进这里 —— 由 main 通过模块工厂的闭包传给具体模块。

#pragma once

#include <chrono>

namespace breeze
{

// 默认 30 帧。整数除法：1e9/30 ns = 33.333ms；写成 milliseconds(33) 只有 990ms/30 帧，实为 30.3 帧。
// **代价**：全部 ITickable 在同一轮里串行跑，一轮超时就丢 tick（tickCount 落后于时间），
// 所以 Tick 必须短且不阻塞 —— 占久了丢的是所有人的帧。
inline constexpr std::chrono::nanoseconds kDefaultTickInterval{1'000'000'000 / 30};

struct Options
{
    // <= 0 关闭周期 tick，循环退化为纯事件驱动。没有 ITickable 模块时也自动关闭。
    std::chrono::nanoseconds tickInterval = kDefaultTickInterval;
};

} // namespace breeze
