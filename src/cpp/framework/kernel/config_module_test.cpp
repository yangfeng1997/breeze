// 无测试框架依赖：失败即 abort。临时文件写在 /tmp 下。

#include "framework/kernel/config_module.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

const fs::path kRealCommon = fs::path(BREEZE_CONFIG_DIR) / "common.yaml";

fs::path TempDir(const char* tag)
{
    auto p = fs::temp_directory_path() / std::format("breeze_cfgmod_test_{}_{}", ::getpid(), tag);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

std::string ReadAll(const fs::path& p)
{
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void Write(const fs::path& p, const std::string& text)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

bool Has(const std::string& s, std::string_view needle) { return s.find(needle) != std::string::npos; }

ModuleFactory MakeConfig(std::string path)
{
    return [path = std::move(path)](App& a) { return std::make_unique<ConfigModule>(a, path); };
}

// 起一个只含 ConfigModule 的 App 并 Init；返回 Init 的结果，模块指针经 out 给出。
Result<> InitWith(App& app, const fs::path& file)
{
    app.RegisterModule(MakeConfig(file.string()));
    if (auto b = app.Build(); !b)
        return b;
    return app.Init();
}

// 真实的 config/common.yaml 单独就能起：自带本机开发默认值，不依赖任何别的文件。
void TestLoadsRealCommonYaml()
{
    ::unsetenv("BREEZE_REDIS_PWD");
    ::unsetenv("BREEZE_MONGO_URI");
    App app("t");
    CHECK(InitWith(app, kRealCommon));
    auto* m = app.GetModule<ConfigModule>("config");
    CHECK(m);
    CHECK(m->Common().cluster.name == "gameserver" && m->Common().cluster.env == config::Env::Dev);
    CHECK(m->Common().etcd.dialTimeoutMs == 5000);
    // 业务也能经 Raw() 读 common.yaml 里的段
    auto name = m->Raw().Get<std::string>("cluster.name");
    CHECK(name && *name == "gameserver");
}

// 只加载传进来的那一个文件：旁边有 env/dev.yaml 也不会被读。
void TestLoadsOnlyTheGivenFile()
{
    ::unsetenv("BREEZE_REDIS_PWD");
    ::unsetenv("BREEZE_MONGO_URI");
    const auto dir = TempDir("only");
    std::string text = ReadAll(kRealCommon);
    const auto pos = text.find("world_id: 1 ");
    CHECK(pos != std::string::npos);
    text.replace(pos, std::string("world_id: 1").size(), "world_id: 42");
    Write(dir / "common.yaml", text);
    Write(dir / "env" / "dev.yaml", "cluster:\n  world_id: 7\n"); // 若被隐式合并，world_id 会变成 7
    Write(dir / "dev.yaml", "cluster:\n  world_id: 8\n");

    App app("t");
    CHECK(InitWith(app, dir / "common.yaml"));
    CHECK(app.GetModule<ConfigModule>("config")->Common().cluster.worldId == 42);
}

// 业务自己的文件由业务自己加载，与框架那一份互不影响。
void TestBusinessLoadsOwnFile()
{
    const auto dir = TempDir("biz");
    Write(dir / "gatesvr.yaml", "gate:\n  tcp_port: 9000\n");
    App app("t");
    CHECK(InitWith(app, kRealCommon));

    auto own = Config::Load({(dir / "gatesvr.yaml").string()});
    CHECK(own);
    auto port = own->Get<int>("gate.tcp_port");
    CHECK(port && *port == 9000);
    CHECK(app.GetModule<ConfigModule>("config")->Common().cluster.worldId == 1); // 框架那份没被影响
}

void TestErrors()
{
    const auto dir = TempDir("err");

    // 文件不存在：错误里有路径
    {
        App app("t");
        auto r = InitWith(app, dir / "nope.yaml");
        CHECK(!r && r.error().code == Errc::Module && Has(r.error().msg, "nope.yaml"));
    }
    // 缺必填项：错误里有哪一项、哪个文件
    {
        Write(dir / "bad.yaml", "cluster: { name: g, env: dev, world_id: 1 }\n");
        App app("t");
        auto r = InitWith(app, dir / "bad.yaml");
        CHECK(!r && Has(r.error().msg, "缺少必填项") && Has(r.error().msg, "bad.yaml"));
    }
}

void TestConfigArg()
{
    auto parse = [](std::vector<const char*> argv, std::string& path)
    {
        ArgsParser p("t");
        AddConfigArg(p, path);
        return p.Parse(static_cast<int>(argv.size()), argv.data());
    };
    std::string path;
    CHECK(parse({"svr", "--config", "/etc/common.yaml"}, path).proceed && path == "/etc/common.yaml");
    path.clear();
    CHECK(parse({"svr", "-c", "x.yaml"}, path).proceed && path == "x.yaml");
    path.clear();
    CHECK(!parse({"svr"}, path).proceed); // 必填
}

} // namespace

int main()
{
    TestLoadsRealCommonYaml();
    TestLoadsOnlyTheGivenFile();
    TestBusinessLoadsOwnFile();
    TestErrors();
    TestConfigArg();
    std::puts("config_module_test: ok");
}
