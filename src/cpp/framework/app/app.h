// 服务进程骨架 App：模块注册 → 拓扑排序 → 启动 → 主循环 → Stop → Shutdown。
// 启动与主循环对照 src/go/core/app；C++ 额外提供两段同步停机，见 Run。
// 不处理信号，启动失败不回滚，析构不隐式调用生命周期。

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "framework/app/app_options.h"
#include "framework/app/errors.h"
#include "framework/app/module.h"

namespace breeze
{

enum class Lifecycle : std::uint8_t
{
    New,
    Building,
    Built,
    Initializing,
    Inited,
    Starting,
    Started,
    Running,
    Stopping,
    Stopped,
    ShuttingDown,
    Shutdown,
    Failed, // 启动失败，不可继续推进或重试；见 lifecycle.cpp
};

std::string_view ToString(Lifecycle s) noexcept;

// 工厂是 move-only 闭包：std::function 会额外要求可拷贝性。
using ModuleFactory = std::move_only_function<std::unique_ptr<Module>(App&)>;

class App
{
public:
    explicit App(std::string name, Options opts = {});

    // **析构不是停机**：它只释放内存，不调任何模块的生命周期方法。
    // 但顺序要管 —— 按拓扑序**逆序**析构（依赖者先走），且此时 GetModule 已一律返回 nullptr。
    // 模块持有的 std::thread 必须是 jthread 并监听 StopToken()，否则析构里 join 不上
    // 会直接 std::terminate。这是 Go 版没有的约束（Go 随进程退出被带走）。
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    const std::string& Name() const noexcept { return name_; }

    // Stop 调模块前发出；启动失败则在 Run 返回时发出，析构也会兜底发出。
    // 只表示“进程正在退出”，不承载剩余时间；线程应在 Stop 中完成 join。
    std::stop_token StopToken() const noexcept { return stop_.get_token(); }

    // 登记模块工厂，调用顺序即注册序。Build 之后调用抛 std::logic_error（编程错误）。
    void RegisterModule(ModuleFactory f);

    Module* GetModule(std::string_view name) const;

    template <class T>
    T* GetModule(std::string_view name) const
    {
        return dynamic_cast<T*>(GetModule(name));
    }

    // 任意线程可调：叫醒主循环去跑 IWaker 模块。非阻塞、可合并 ——
    // 多个生产者同时叫只产生一个 token，调用方无法也不需要判断自己是不是"有效"的那次。
    void Notify() noexcept;

    std::expected<void, Error> Build();
    std::expected<void, Error> Init();
    std::expected<void, Error> Start();

    // 手动编排时在 Start 成功后调用，两段均幂等且按拓扑逆序；Shutdown 要求先 Stop。
    // 生命周期方法只能在同一线程串行调用，不得从模块回调重入；运行中退出用 Run 的外部 token。
    // 模块 Stop/Shutdown 不得抛异常。两段期间 GetModule 仍可用。
    std::expected<void, Error> Stop();
    std::expected<void, Error> Shutdown();

    // Build → Init → Start → 主循环 → Stop → Shutdown，全程在调用方线程执行。
    // 外部 token 结束后先退出主循环再同步收尾；启动失败不回滚，返回错误。
    // 收尾期间不再 Tick/Wake，不提供异步排空或超时预算。
    std::expected<void, Error> Run(std::stop_token st);

private:
    // Build 阶段筛出的能力子集：裸指针由 modules_ 拥有，Build 之后冻结。
    // 同时存 Module*（取名字）与能力指针（热路径直调，没有 Go 版那句不带 ok 的断言）。
    template <class Cap>
    struct Capable
    {
        Module* module;
        Cap* cap;
    };

    struct StringHash
    {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    };

    void RunLoop(std::stop_token st);
    bool WakeAll();
    void TickAll(Clock::time_point now, std::uint64_t tickCount);

    // cur 由调用方在持锁时读出再传入：错误串里不能在锁外再读 state_。
    std::unexpected<Error> StateErr(std::string_view method, Lifecycle cur, Lifecycle want) const;
    std::unexpected<Error> ModuleErr(std::string_view method, std::string_view module, const Error& e) const;

    const std::string name_;
    Options opts_;
    std::stop_source stop_;

    // 规则：持 mu_ 时不得调模块方法 —— 模块回调里会 GetModule，那会重入。
    // mu_ 只保护 state_ / factories_ / modules_ / byName_ 的读写。
    mutable std::shared_mutex mu_;
    Lifecycle state_ = Lifecycle::New;
    std::vector<ModuleFactory> factories_;
    std::vector<std::unique_ptr<Module>> modules_; // 拓扑序
    std::unordered_map<std::string, Module*, StringHash, std::equal_to<>> byName_;
    std::vector<Capable<ITickable>> tickables_;
    std::vector<Capable<IWaker>> wakers_;

    // wake_ 是信号不是数据（等价于 Go 的 cap=1 空 chan）：可合并、可丢，漏的由下一轮补。
    std::atomic<bool> wake_{false};
    std::mutex wakeMu_;
    std::condition_variable_any wakeCv_;

    std::uint64_t tickCount_ = 0; // 只在主循环线程读写
};

} // namespace breeze
