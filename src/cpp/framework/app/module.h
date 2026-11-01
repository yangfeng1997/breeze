// Module 与两个可选能力接口 ITickable / IWaker。对照 src/go/core/app/module.go。

#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>
#include <vector>

#include "framework/app/errors.h"

namespace breeze
{

class App;

// 单调时钟：Tick 的 now 只用于算时间差，不是墙钟。
using Clock = std::chrono::steady_clock;

// 可被 App 装配、调度并驱动生命周期的功能单元。
//
// 由 App 持有（unique_ptr），模块只持有 App 的引用 —— App 活得比模块长。
// 工厂在 Build 阶段被调用，此时 App 已可用。
class Module
{
public:
    explicit Module(App& app) noexcept
        : app_(app)
    {
    }
    virtual ~Module(); // 见 module.cpp：钉住 vtable
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    // 非空且在模块存活期内有效（字面量或成员），App 会按它建索引。
    virtual std::string_view Name() const = 0;

    // Build 阶段据此做拓扑排序。返回的 view 必须在模块存活期内有效（字面量或成员）。
    virtual std::vector<std::string_view> DependsOn() const { return {}; }

    virtual Result<> Init() = 0;

    // 返回错误的模块**必须自清理**：它可能已起线程或开了监听，框架不替它收尾
    // （见 lifecycle.cpp 的 Start，无回滚）。
    virtual Result<> Start() = 0;

    App& app() const noexcept { return app_; }

private:
    App& app_;
};

// 希望被主循环周期驱动的模块。
//
// tickCount 是**周期 tick 跑过的次数**（从 1 开始），不是循环轮次 ——
// 两次 tick 之间穿插的事件不会让它跳。只经此参数交付，App 上没有查询方法。
// Tick 与其他模块的 Tick/Wake 在主循环线程上串行，必须短且不阻塞。
class ITickable
{
public:
    virtual void Tick(Clock::time_point now, std::uint64_t tickCount) = 0;

protected:
    ~ITickable() = default;
};

// 需要被 App::Notify() 唤醒的模块。
//
// - Wake 与其他 Tick/Wake 串行，模块内部状态不用加锁；
// - **单轮处理多少由模块自定，框架不给上限也不告警**：占久了拖的是所有人
//   （周期 tick、停止响应），主循环是单线程；
// - 返回 true = 还有积压，App 会补一个 token 再转一轮。永远返回 true 会让
//   主循环回不到阻塞态，CPU 打满。
class IWaker
{
public:
    virtual bool Wake() = 0;

protected:
    ~IWaker() = default;
};

} // namespace breeze
