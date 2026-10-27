// 服务进程骨架 App：模块注册 → 拓扑排序 → 启动编排 → 事件驱动主循环。
// 对照 src/go/core/app，语义以其 CLAUDE.md 为准。
//
// **只管启动，不管停机**：没有 Stop/Shutdown、没有信号处理、启动失败不回滚。
// Run 阻塞在调用方的 stop_token 上，token 结束即返回，之后的收尾全是调用方的事。
//
// 与 Go 版唯一的实质差别是析构 —— 见 ~App。

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

#include "app_options.h"
#include "errors.h"
#include "module.h"

namespace breeze
{

enum class Lifecycle : std::uint8_t
{
    New,     // 已创建，未 Build
    Built,   // Build 完成
    Inited,  // Init 完成
    Started, // Start 完成 —— 终态
};

std::string_view ToString(Lifecycle s) noexcept;

using ModuleFactory = std::function<std::unique_ptr<Module>(App&)>;

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

    // 进程级 token，Run 返回时（或 ~App，若从未 Run）才 request_stop —— 比模块的线程活得短。
    // 只表示"进程正在退出"，不承载剩余时间，模块不得依赖它完成自身清理。
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

    Result<> Build();
    Result<> Init();
    Result<> Start();

    // Build → Init → Start → 主循环。返回错误只表示启动阶段失败，框架不做任何收尾。
    // 主循环跑在**调用方线程**上，不另起线程：返回后 Tick/Wake 与调用方的收尾在时间上
    // 不可能重叠，mu_ 的用法才不用改。
    Result<> Run(std::stop_token st);

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
