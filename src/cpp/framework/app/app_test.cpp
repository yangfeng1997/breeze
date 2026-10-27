// 对照 src/go/core/app/app_test.go 与 loop_test.go。无测试框架依赖：失败即 abort。

#include "app.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "logger.h"

using namespace breeze;
using namespace std::chrono_literals;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

namespace
{

class TestModule : public Module
{
public:
    TestModule(App& app,
               std::string name,
               std::vector<std::string_view> deps = {},
               std::vector<std::string>* log = nullptr)
        : Module(app)
        , name_(std::move(name))
        , deps_(std::move(deps))
        , log_(log)
    {
    }

    std::string_view Name() const override { return name_; }
    std::vector<std::string_view> DependsOn() const override { return deps_; }
    Result<> Init() override
    {
        if (log_)
            log_->push_back(name_);
        return {};
    }
    Result<> Start() override { return startErr_ ? Fail(Errc::Module, "boom") : Result<>{}; }

    bool startErr_ = false;

private:
    std::string name_;
    std::vector<std::string_view> deps_;
    std::vector<std::string>* log_;
};

ModuleFactory Make(std::string name, std::vector<std::string_view> deps = {}, std::vector<std::string>* log = nullptr)
{
    return [=](App& a) { return std::make_unique<TestModule>(a, name, deps, log); };
}

// 无依赖按注册序；有依赖时依赖者靠后。b(1) c(2) 先于 a(0)。
void TestOrder()
{
    std::vector<std::string> log;
    App app("t", Options{});
    app.RegisterModule(Make("a", {"c"}, &log));
    app.RegisterModule(Make("b", {}, &log));
    app.RegisterModule(Make("c", {}, &log));
    CHECK(app.Build());
    CHECK(app.Init());
    CHECK((log == std::vector<std::string>{"b", "c", "a"}));
}

// App 的启动日志走默认 logger（g_default_logger）。整个测试进程把它指向 /tmp 下的文件，既不污染终端，
// 也让这里能读回来验证。
void TestLifecycleLogs(Logger& lg, const std::filesystem::path& dir)
{
    {
        App app("lifecycle-logs", Options{});
        app.RegisterModule(Make("a"));
        CHECK(app.Build());
        CHECK(app.Init());
        CHECK(app.Start());
    }
    lg.Flush();

    std::ifstream in(dir / "t.log");
    std::stringstream ss;
    ss << in.rdbuf();
    const auto text = ss.str();
    CHECK(text.find("app built app=lifecycle-logs modules=1") != std::string::npos);
    CHECK(text.find("module inited app=lifecycle-logs module=a") != std::string::npos);
    CHECK(text.find("module started app=lifecycle-logs module=a") != std::string::npos);
}

void TestBuildErrors()
{
    {
        App app("t", Options{});
        app.RegisterModule(Make("a", {"b"}));
        app.RegisterModule(Make("b", {"a"}));
        auto r = app.Build();
        CHECK(!r && r.error().code == Errc::Cycle);
    }
    {
        App app("t", Options{});
        app.RegisterModule(Make("a", {"a"}));
        CHECK(app.Build().error().code == Errc::SelfDep);
    }
    {
        App app("t", Options{});
        app.RegisterModule(Make("a", {"nope"}));
        CHECK(app.Build().error().code == Errc::UnknownDep);
    }
    {
        App app("t", Options{});
        app.RegisterModule(Make("a"));
        app.RegisterModule(Make("a"));
        CHECK(app.Build().error().code == Errc::DuplicateModule);
    }
    {
        App app("t", Options{});
        CHECK(app.Init().error().code == Errc::Phase); // 未 Build 就 Init
    }
    {
        App app("t", Options{});
        CHECK(app.Build());
        bool threw = false;
        try
        {
            app.RegisterModule(Make("late"));
        }
        catch (const std::logic_error&)
        {
            threw = true;
        }
        CHECK(threw);
    }
}

// 返回错误的 Start 之后的模块不再被启动，且不回滚前面的。
void TestStartFailure()
{
    App app("t", Options{});
    app.RegisterModule(
        [](App& a)
        {
            auto m = std::make_unique<TestModule>(a, "bad");
            m->startErr_ = true;
            return m;
        });
    std::stop_source ss;
    auto r = app.Run(ss.get_token());
    CHECK(!r && r.error().code == Errc::Module);
    CHECK(app.StopToken().stop_requested()); // 启动失败那条路 token 也会 stop
}

class Ticker : public TestModule, public ITickable
{
public:
    Ticker(App& a, std::stop_source& ss)
        : TestModule(a, "ticker")
        , ss_(ss)
    {
    }
    void Tick(Clock::time_point, std::uint64_t n) override
    {
        seen.push_back(n);
        if (n == 3)
            ss_.request_stop();
    }
    std::vector<std::uint64_t> seen;

private:
    std::stop_source& ss_;
};

void TestTickCount()
{
    std::stop_source ss;
    App app("t", Options{.tickInterval = 1ms});
    Ticker* t = nullptr;
    app.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<Ticker>(a, ss);
            t = m.get();
            return m;
        });
    CHECK(app.Run(ss.get_token()));
    CHECK((t->seen == std::vector<std::uint64_t>{1, 2, 3}));
}

class Inbox : public TestModule, public IWaker
{
public:
    Inbox(App& a, std::stop_source& ss)
        : TestModule(a, "inbox")
        , ss_(ss)
    {
    }
    Result<> Start() override
    {
        pending = 20;
        app().Notify();
        return {};
    }
    bool Wake() override
    {
        ++rounds;
        pending -= std::min(pending, 8); // 自定上界 8
        if (pending == 0)
            ss_.request_stop();
        return pending > 0;
    }
    int pending = 0;
    int rounds = 0;

private:
    std::stop_source& ss_;
};

// 20 条、每轮 8 条：8+8+4 = 3 轮。验证"积压时自补 token"，否则第二轮永远等不到。
void TestWakeBacklog()
{
    std::stop_source ss;
    App app("t", Options{});
    Inbox* in = nullptr;
    app.RegisterModule(
        [&](App& a)
        {
            auto m = std::make_unique<Inbox>(a, ss);
            in = m.get();
            return m;
        });
    CHECK(app.Run(ss.get_token()));
    CHECK(in->rounds == 3);
}

// 4 个生产者各 post 1000 次后 Notify：信号可合并，但最后一次投递必须被看到，
// 否则 Wake 永远读不到 4000，主循环不退出（测试会挂死 —— 用 ctest 超时兜底）。
// 生产者在 Start 里起，由模块的 jthread 持有；它们先于主循环退出之前就已发完。
void TestNotifyFromThreads()
{
    constexpr int kProducers = 4, kEach = 1000;
    std::stop_source ss;
    App app("t", Options{});

    struct Counter : TestModule, IWaker
    {
        Counter(App& a, std::stop_source& ss)
            : TestModule(a, "counter")
            , ss_(ss)
        {
        }
        Result<> Start() override
        {
            for (int i = 0; i < kProducers; ++i)
            {
                producers_.emplace_back(
                    [this]
                    {
                        for (int k = 0; k < kEach; ++k)
                        {
                            posted_.fetch_add(1, std::memory_order_relaxed);
                            app().Notify();
                        }
                    });
            }
            return {};
        }
        bool Wake() override
        {
            if (posted_.load(std::memory_order_relaxed) == kProducers * kEach)
                ss_.request_stop();
            return false;
        }
        std::atomic<int> posted_{0};
        std::stop_source& ss_;
        std::vector<std::jthread> producers_; // 末位成员：先于 posted_ 析构，即先 join
    };

    app.RegisterModule([&](App& a) { return std::make_unique<Counter>(a, ss); });
    CHECK(app.Run(ss.get_token()));
}

} // namespace

int main()
{
    const auto dir = std::filesystem::temp_directory_path() / std::format("breeze_app_test_{}", ::getpid());
    std::filesystem::remove_all(dir);
    LoggerConfig cfg;
    cfg.file.dir = dir.string();
    cfg.name = "t";
    cfg.async.enabled = false;
    std::string err;
    auto lg = Logger::Create(cfg, err);
    CHECK(lg);
    g_default_logger.store(lg.get());

    TestOrder();
    TestLifecycleLogs(*lg, dir);
    TestBuildErrors();
    TestStartFailure();
    TestTickCount();
    TestWakeBacklog();
    TestNotifyFromThreads();

    g_default_logger.store(nullptr);
    std::filesystem::remove_all(dir);
    std::puts("all ok");
    return 0;
}
