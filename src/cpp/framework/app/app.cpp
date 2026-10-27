// App 的装配：构造/析构、RegisterModule、Build 与 depsSort。
// 生命周期见 lifecycle.cpp，主循环见 loop.cpp。

#include "app.h"

#include <algorithm>
#include <format>
#include <map>
#include <ranges>
#include <stdexcept>

#include "logger_macros.h"

namespace breeze
{

namespace
{

std::string Join(const auto& names)
{
    std::string out;
    for (std::string_view n : names)
    {
        if (!out.empty())
            out += ',';
        out += n;
    }
    return out;
}

// 对 modules 按 DependsOn 做稳定拓扑排序，返回按序的下标。
// 无依赖关系的模块保持注册序：每步取注册序号最小的就绪节点，该语义靠 app_test 固定。
Result<std::vector<std::size_t>> DepsSort(const std::vector<std::unique_ptr<Module>>& modules)
{
    const std::size_t n = modules.size();

    std::map<std::string_view, std::size_t> idx;
    for (std::size_t i = 0; i < n; ++i)
        idx.emplace(modules[i]->Name(), i);

    // dependents[j] = 依赖 j 的模块序号，按注册序追加，天然有序
    std::vector<std::vector<std::size_t>> dependents(n);
    std::vector<std::size_t> indeg(n, 0);

    for (std::size_t i = 0; i < n; ++i)
    {
        // 同一个依赖名写多遍只算一次入度，否则该模块永远凑不齐 0 入度
        std::vector<std::string_view> seen;
        for (std::string_view dep : modules[i]->DependsOn())
        {
            if (dep.empty() || std::ranges::find(seen, dep) != seen.end())
                continue;
            seen.push_back(dep);

            auto it = idx.find(dep);
            if (it == idx.end())
            {
                return Fail(Errc::UnknownDep,
                            std::format("module {}: {}: {}", modules[i]->Name(), Describe(Errc::UnknownDep), dep));
            }
            if (it->second == i)
            {
                return Fail(Errc::SelfDep, std::format("module {}: {}", modules[i]->Name(), Describe(Errc::SelfDep)));
            }
            dependents[it->second].push_back(i);
            ++indeg[i];
        }
    }

    std::vector<std::size_t> out;
    out.reserve(n);
    std::vector<bool> done(n, false);
    while (out.size() < n)
    {
        std::size_t next = n;
        for (std::size_t i = 0; i < n; ++i)
        {
            if (!done[i] && indeg[i] == 0)
            {
                next = i;
                break;
            }
        }
        if (next == n)
            break; // 剩下的都有入度，必然成环
        done[next] = true;
        out.push_back(next);
        for (std::size_t d : dependents[next])
            --indeg[d];
    }

    if (out.size() < n)
    {
        // 环上的与挂在环后面的都排不进去，一起报出来更好定位
        std::vector<std::string_view> stuck;
        for (std::size_t i = 0; i < n; ++i)
        {
            if (!done[i])
                stuck.push_back(modules[i]->Name());
        }
        return Fail(Errc::Cycle, std::format("{} among: {}", Describe(Errc::Cycle), Join(stuck)));
    }
    return out;
}

} // namespace

std::string_view ToString(Lifecycle s) noexcept
{
    switch (s)
    {
        case Lifecycle::New:
            return "new";
        case Lifecycle::Built:
            return "built";
        case Lifecycle::Inited:
            return "inited";
        case Lifecycle::Started:
            return "started";
    }
    return "unknown";
}

App::App(std::string name, Options opts)
    : name_(std::move(name))
    , opts_(std::move(opts))
{
}

App::~App()
{
    stop_.request_stop(); // 从未 Run 时，模块的 jthread 否则永远等不到 token

    std::vector<std::unique_ptr<Module>> modules;
    {
        // 析构里模块仍可能调 GetModule：先清索引再释放，且不能持锁析构模块（会重入 mu_）。
        std::unique_lock lk(mu_);
        byName_.clear();
        tickables_.clear();
        wakers_.clear();
        modules = std::move(modules_);
    }
    while (!modules.empty())
        modules.pop_back(); // 拓扑序逆序：依赖者先走
}

void App::RegisterModule(ModuleFactory f)
{
    std::unique_lock lk(mu_);
    if (state_ != Lifecycle::New)
    {
        // 编程错误，无法通过返回值表达（对照 Go 版 panic）
        throw std::logic_error("app: RegisterModule called after Build");
    }
    factories_.push_back(std::move(f));
}

Module* App::GetModule(std::string_view name) const
{
    std::shared_lock lk(mu_);
    auto it = byName_.find(name);
    return it == byName_.end() ? nullptr : it->second;
}

std::unexpected<Error> App::StateErr(std::string_view method, Lifecycle cur, Lifecycle want) const
{
    return Fail(
        Errc::Phase,
        std::format(
            "{}: {}: current {}, want {}: {}", name_, method, ToString(cur), ToString(want), Describe(Errc::Phase)));
}

std::unexpected<Error> App::ModuleErr(std::string_view method, std::string_view module, const Error& e) const
{
    return Fail(Errc::Module, std::format("{}: module {}: {}: {}", name_, module, method, e.msg));
}

Result<> App::Build()
{
    std::vector<ModuleFactory> factories;
    {
        std::unique_lock lk(mu_);
        if (state_ != Lifecycle::New)
            return StateErr("build", state_, Lifecycle::New);
        if (name_.empty())
            return Fail(Errc::EmptyName, std::format("{}", Describe(Errc::EmptyName)));
        factories = factories_;
    }

    // 工厂在锁外调用：工厂里可能回调 App，持锁会重入
    std::vector<std::unique_ptr<Module>> built;
    built.reserve(factories.size());
    std::map<std::string, std::size_t, std::less<>> seen;
    for (std::size_t i = 0; i < factories.size(); ++i)
    {
        const auto at = [&](Errc c, std::string_view detail = {})
        {
            return Fail(
                c, std::format("{}: factory #{}{}{}: {}", name_, i, detail.empty() ? "" : ": ", detail, Describe(c)));
        };
        if (!factories[i])
            return at(Errc::NilFactory);
        auto m = factories[i](*this);
        if (!m)
            return at(Errc::NilModule);
        const std::string_view mname = m->Name();
        if (mname.empty())
            return at(Errc::EmptyName);
        if (!seen.emplace(std::string(mname), i).second)
            return at(Errc::DuplicateModule, mname);
        built.push_back(std::move(m));
    }

    auto order = DepsSort(built);
    if (!order)
        return Fail(order.error().code, std::format("{}: {}", name_, order.error().msg));

    std::vector<std::unique_ptr<Module>> sorted;
    sorted.reserve(built.size());
    for (std::size_t i : *order)
        sorted.push_back(std::move(built[i]));

    // 按拓扑序筛出实现了能力接口的模块，Build 之后冻结。
    // 两段刻意写开：加第三个能力时照抄一段即可。
    std::vector<Capable<ITickable>> tickables;
    std::vector<Capable<IWaker>> wakers;
    std::unordered_map<std::string, Module*, StringHash, std::equal_to<>> byName;
    for (auto& m : sorted)
    {
        byName.emplace(std::string(m->Name()), m.get());
        if (auto* t = dynamic_cast<ITickable*>(m.get()))
            tickables.push_back({m.get(), t});
        if (auto* w = dynamic_cast<IWaker*>(m.get()))
            wakers.push_back({m.get(), w});
    }

    // 能力名进启动日志：忘了继承 ITickable/IWaker 会静默不收集（override 只拦得住签名写错），
    // 模块没出现在这行里就是没被收到。
    const auto namesOf = [](const auto& caps)
    { return Join(caps | std::views::transform([](const auto& c) { return c.module->Name(); })); };
    const auto summary = std::format("app built app={} modules={} tickable=[{}] waker=[{}]",
                                     name_,
                                     sorted.size(),
                                     namesOf(tickables),
                                     namesOf(wakers));

    {
        std::unique_lock lk(mu_);
        modules_ = std::move(sorted);
        byName_ = std::move(byName);
        tickables_ = std::move(tickables);
        wakers_ = std::move(wakers);
        state_ = Lifecycle::Built;
    }
    LOG_INFO("{}", summary);
    return {};
}

Result<> App::Run(std::stop_token st)
{
    // 无论从哪条路返回，进程都算正在退出
    struct StopOnExit
    {
        std::stop_source& s;
        ~StopOnExit() { s.request_stop(); }
    } guard{stop_};

    if (auto r = Build(); !r)
        return r;
    if (auto r = Init(); !r)
        return r;
    if (auto r = Start(); !r)
        return r;

    RunLoop(std::move(st));
    return {};
}

} // namespace breeze
