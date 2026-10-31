// 无测试框架依赖：失败即 abort。日志写在 /tmp 下。

#include "framework/kernel/log_module.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "base/logger/logger_macros.h"
#include "framework/app/app.h"

using namespace breeze;
namespace fs = std::filesystem;

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

fs::path TempDir(const char* tag)
{
    auto p = fs::temp_directory_path() / std::format("breeze_logmod_test_{}_{}", ::getpid(), tag);
    fs::remove_all(p);
    return p;
}

std::string ReadAll(const fs::path& p)
{
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool Has(const std::string& s, std::string_view needle) { return s.find(needle) != std::string::npos; }

LoggerConfig Cfg(const fs::path& dir, const char* name)
{
    LoggerConfig c;
    c.file.dir = dir.string();
    c.name = name;
    c.async.enabled = false;
    return c;
}

// 在 Init 时记下默认 logger 是否已就绪，并打一条日志。
class Probe final : public Module
{
public:
    Probe(App& app, bool* ready)
        : Module(app)
        , ready_(ready)
    {
    }
    std::string_view Name() const override { return "probe"; }
    Result<> Init() override
    {
        *ready_ = g_default_logger.load() != nullptr;
        LOG_INFO("probe-init");
        return {};
    }
    Result<> Start() override { return {}; }

private:
    bool* ready_;
};

ModuleFactory MakeLog(LoggerConfig cfg)
{
    return [cfg](App& a) { return std::make_unique<LogModule>(a, cfg); };
}

ModuleFactory MakeProbe(bool* ready)
{
    return [ready](App& a) { return std::make_unique<Probe>(a, ready); };
}

// LogModule 注册在前：即使 Probe 不声明 DependsOn("log")，Init 时默认 logger 也已就绪。
void TestRegisteredFirstInitsFirst()
{
    const auto dir = TempDir("first");
    bool ready = false;
    {
        App app("t", Options{});
        app.RegisterModule(MakeLog(Cfg(dir, "svc")));
        app.RegisterModule(MakeProbe(&ready));
        CHECK(app.Build());
        CHECK(!g_default_logger.load()); // Build 之后、Init 之前还没有
        CHECK(app.Init());
        CHECK(app.Start());
        CHECK(g_default_logger.load());
        LOG_INFO("while-running {}", 1);
    }
    CHECK(ready);

    const auto text = ReadAll(dir / "svc.log");
    CHECK(Has(text, "log ready name=svc"));
    CHECK(Has(text, "probe-init"));
    CHECK(Has(text, "while-running 1"));

    CHECK(!g_default_logger.load()); // ~App 之后已撤销
    LOG_INFO("after-app-destroyed"); // 回落 stderr，不崩溃
    fs::remove_all(dir);
}

// 反例：注册在后则 Probe 先 Init，此时没有默认 logger —— 这就是"必须第一个注册"的原因。
void TestRegisteredLastInitsAfter()
{
    const auto dir = TempDir("last");
    bool ready = true;
    {
        App app("t", Options{});
        app.RegisterModule(MakeProbe(&ready));
        app.RegisterModule(MakeLog(Cfg(dir, "svc")));
        CHECK(app.Build());
        CHECK(app.Init());
    }
    CHECK(!ready);
    fs::remove_all(dir);
}

void TestInitFailureLeavesNoDefault()
{
    const auto dir = TempDir("fail");
    fs::create_directories(dir);
    std::ofstream(dir / "file") << "x";

    App app("t", Options{});
    app.RegisterModule(MakeLog(Cfg(dir / "file" / "sub", "svc")));
    CHECK(app.Build());
    auto r = app.Init();
    CHECK(!r);
    CHECK(r.error().code == Errc::Module);
    CHECK(!g_default_logger.load());
    fs::remove_all(dir);
}

// 两个 App 并存时，先销毁的那个不能把后者的默认 logger 撤掉。
void TestDestroyOlderAppKeepsNewerLogger()
{
    const auto dir1 = TempDir("app1");
    const auto dir2 = TempDir("app2");

    auto app1 = std::make_unique<App>("a1", Options{});
    app1->RegisterModule(MakeLog(Cfg(dir1, "one")));
    CHECK(app1->Build());
    CHECK(app1->Init());
    Logger* l1 = g_default_logger.load();

    auto app2 = std::make_unique<App>("a2", Options{});
    app2->RegisterModule(MakeLog(Cfg(dir2, "two")));
    CHECK(app2->Build());
    CHECK(app2->Init());
    Logger* l2 = g_default_logger.load();
    CHECK(l1 && l2 && l1 != l2);

    app1.reset();
    CHECK(g_default_logger.load() == l2); // 没被 app1 的析构撤掉
    LOG_INFO("still-two");

    app2.reset();
    CHECK(!g_default_logger.load());
    CHECK(Has(ReadAll(dir2 / "two.log"), "still-two"));
    fs::remove_all(dir1);
    fs::remove_all(dir2);
}

} // namespace

int main()
{
    TestRegisteredFirstInitsFirst();
    TestRegisteredLastInitsAfter();
    TestInitFailureLeavesNoDefault();
    TestDestroyOlderAppKeepsNewerLogger();
    std::puts("log_module_test: ok");
}
