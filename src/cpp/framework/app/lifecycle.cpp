// Init / Start。两者都**不回滚**：遇错只返回，不碰任何已跑过的模块。
// 启动阶段只有"往前走"一种状态推进，不维护逐模块账本；补偿代码只在失败时跑一次，
// 永远缺真实执行路径，与其维护不如把"失败就退出"说清楚。

#include "base/defer/defer.h"
#include "base/logger/logger_macros.h"
#include "framework/app/app.h"

namespace breeze
{

namespace
{

// 持锁取快照、锁外遍历：模块回调里会 GetModule，持 mu_ 调用会重入。
std::vector<Module*> Snapshot(const std::vector<std::unique_ptr<Module>>& modules)
{
    std::vector<Module*> out;
    out.reserve(modules.size());
    for (const auto& m : modules)
        out.push_back(m.get());
    return out;
}

} // namespace

// 出错模块之前的模块收不到任何通知，框架也不释放它们 Init 期占用的资源。
std::expected<void, Error> App::Init()
{
    std::vector<Module*> mods;
    {
        std::unique_lock lk(mu_);
        if (state_ != Lifecycle::Built)
            return StateErr("init", state_, Lifecycle::Built);
        mods = Snapshot(modules_);
        state_ = Lifecycle::Initializing;
    }
    const auto fail = MakeDefer(
        [this]
        {
            std::unique_lock lk(mu_);
            if (state_ == Lifecycle::Initializing)
                state_ = Lifecycle::Failed;
        });

    for (Module* m : mods)
    {
        if (auto r = m->Init(); !r)
            return ModuleErr("init", m->Name(), r.error());
        LOG_INFO("module inited app={} module={}", name_, m->Name());
    }
    {
        std::unique_lock lk(mu_);
        state_ = Lifecycle::Inited;
    }
    return {};
}

// 比 Init 更需要小心：已 Start 的模块可能已起线程、开了监听，框架不会叫停。
// 返回错误的模块由 Module::Start 的约定自清理，其余随进程退出被带走。
std::expected<void, Error> App::Start()
{
    std::vector<Module*> mods;
    {
        std::unique_lock lk(mu_);
        if (state_ != Lifecycle::Inited)
            return StateErr("start", state_, Lifecycle::Inited);
        mods = Snapshot(modules_);
        state_ = Lifecycle::Starting;
    }
    const auto fail = MakeDefer(
        [this]
        {
            std::unique_lock lk(mu_);
            if (state_ == Lifecycle::Starting)
                state_ = Lifecycle::Failed;
        });

    for (Module* m : mods)
    {
        if (auto r = m->Start(); !r)
            return ModuleErr("start", m->Name(), r.error());
        LOG_INFO("module started app={} module={}", name_, m->Name());
    }
    {
        std::unique_lock lk(mu_);
        state_ = Lifecycle::Started;
    }
    return {};
}

std::expected<void, Error> App::Stop()
{
    std::vector<Module*> mods;
    {
        std::unique_lock lk(mu_);
        if (state_ == Lifecycle::Stopped || state_ == Lifecycle::Shutdown)
            return {};
        if (state_ != Lifecycle::Started)
            return StateErr("stop", state_, Lifecycle::Started);
        mods = Snapshot(modules_);
        state_ = Lifecycle::Stopping;
    }

    // 先广播再逐模块 join，避免依赖 token 的工作线程阻塞 Stop。
    stop_.request_stop();
    for (auto it = mods.rbegin(); it != mods.rend(); ++it)
    {
        (*it)->Stop();
        LOG_INFO("module stopped app={} module={}", name_, (*it)->Name());
    }
    {
        std::unique_lock lk(mu_);
        state_ = Lifecycle::Stopped;
    }
    return {};
}

std::expected<void, Error> App::Shutdown()
{
    std::vector<Module*> mods;
    {
        std::unique_lock lk(mu_);
        if (state_ == Lifecycle::Shutdown)
            return {};
        if (state_ != Lifecycle::Stopped)
            return StateErr("shutdown", state_, Lifecycle::Stopped);
        mods = Snapshot(modules_);
        state_ = Lifecycle::ShuttingDown;
    }

    for (auto it = mods.rbegin(); it != mods.rend(); ++it)
    {
        (*it)->Shutdown();
        LOG_INFO("module shutdown app={} module={}", name_, (*it)->Name());
    }
    {
        std::unique_lock lk(mu_);
        state_ = Lifecycle::Shutdown;
    }
    return {};
}

} // namespace breeze
