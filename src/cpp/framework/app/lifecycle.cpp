// Init / Start。两者都**不回滚**：遇错只返回，不碰任何已跑过的模块。
// 启动阶段只有"往前走"一种状态推进，不维护逐模块账本；补偿代码只在失败时跑一次，
// 永远缺真实执行路径，与其维护不如把"失败就退出"说清楚。

#include "app.h"
#include "logger_macros.h"

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
Result<> App::Init()
{
    std::vector<Module*> mods;
    {
        std::unique_lock lk(mu_);
        if (state_ != Lifecycle::Built)
            return StateErr("init", state_, Lifecycle::Built);
        state_ = Lifecycle::Inited;
        mods = Snapshot(modules_);
    }

    for (Module* m : mods)
    {
        if (auto r = m->Init(); !r)
            return ModuleErr("init", m->Name(), r.error());
        LOG_INFO("module inited app={} module={}", name_, m->Name());
    }
    return {};
}

// 比 Init 更需要小心：已 Start 的模块可能已起线程、开了监听，框架不会叫停。
// 返回错误的模块由 Module::Start 的约定自清理，其余随进程退出被带走。
Result<> App::Start()
{
    std::vector<Module*> mods;
    {
        std::unique_lock lk(mu_);
        if (state_ != Lifecycle::Inited)
            return StateErr("start", state_, Lifecycle::Inited);
        state_ = Lifecycle::Started;
        mods = Snapshot(modules_);
    }

    for (Module* m : mods)
    {
        if (auto r = m->Start(); !r)
            return ModuleErr("start", m->Name(), r.error());
        LOG_INFO("module started app={} module={}", name_, m->Name());
    }
    return {};
}

} // namespace breeze
