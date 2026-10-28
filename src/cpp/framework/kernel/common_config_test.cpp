// config/schema/common.yaml 的回归测试：schema 被改动、改坏时在这里暴露。
// 无测试框架依赖：失败即 abort。

#include "config/common_config.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

using namespace breeze;
using config::CommonConfig;
using config::Env;

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

// 正常配置；密钥用 ${ENV:...} 引用。整个文件就是 CommonConfig（它的几个 section 在根上）。
constexpr const char* kGood =
    "cluster: { name: gameserver, env: dev, world_id: 2 }\n"
    "etcd: { endpoints: ['http://h:2379', 'http://h:2479'] }\n"
    "redis:\n"
    "  nodes: [{ host: h, port: 6379 }, { host: h, port: 6380 }]\n"
    "  password: ${ENV:BREEZE_T_REDIS_PWD}\n"
    "mongo: { uri: '${ENV:BREEZE_T_MONGO_URI}', database: admin }\n";

void SetSecrets()
{
    ::setenv("BREEZE_T_REDIS_PWD", "redis-secret-123", 1);
    ::setenv("BREEZE_T_MONGO_URI", "mongodb://admin:mongo-secret@h:27017/?authSource=admin", 1);
}

ConfigResult<CommonConfig> Read(std::string_view yaml)
{
    auto c = Config::LoadString(yaml, "common.yaml");
    if (!c)
        return std::unexpected(c.error());
    return c->Get<CommonConfig>("");
}

bool Has(const std::string& s, std::string_view needle) { return s.find(needle) != std::string::npos; }

void TestGoodConfig()
{
    SetSecrets();
    auto r = Read(kGood);
    CHECK(r);
    CHECK(r->cluster.name == "gameserver" && r->cluster.env == Env::Dev && r->cluster.worldId == 2);
    CHECK(r->etcd.endpoints.size() == 2);
    CHECK(r->redis.nodes.size() == 2 && r->redis.nodes[1].port == 6380);
    CHECK(r->redis.password == "redis-secret-123"); // 占位符已展开
    CHECK(r->mongo.database == "admin" && Has(r->mongo.uri, "mongodb://"));
    // 超时有默认值
    CHECK(r->etcd.dialTimeoutMs == 5000 && r->redis.connectTimeoutMs == 5000 && r->mongo.connectTimeoutMs == 5000);
}

// 密钥永远不会出现在打印、日志里。
void TestSecretsNeverPrinted()
{
    SetSecrets();
    auto r = Read(kGood);
    CHECK(r);
    const std::string text = ConfigDump(*r);
    CHECK(Has(text, "password: ***") && Has(text, "uri: ***"));
    CHECK(!Has(text, "redis-secret-123") && !Has(text, "mongo-secret"));
    const CommonConfig red = ConfigRedacted(*r);
    CHECK(red.redis.password == "***" && red.mongo.uri == "***" && red.mongo.database == "admin");
}

void TestSecretsMustBeInjected()
{
    ::unsetenv("BREEZE_T_REDIS_PWD");
    ::unsetenv("BREEZE_T_MONGO_URI");
    auto r = Read(kGood);
    CHECK(!r);
    CHECK(Has(r.error().message, "BREEZE_T_REDIS_PWD") && Has(r.error().message, "BREEZE_T_MONGO_URI")); // 一次列全
}

void TestConstraints()
{
    SetSecrets();
    auto with = [](std::string_view from, std::string_view to)
    {
        std::string y = kGood;
        const auto pos = y.find(from);
        CHECK(pos != std::string::npos);
        return y.replace(pos, from.size(), to);
    };

    auto env = Read(with("env: dev", "env: staging"));
    CHECK(!env && Has(env.error().message, "dev | test | prod"));

    auto world = Read(with("world_id: 2", "world_id: 0"));
    CHECK(!world && world.error().path == "cluster.world_id");

    auto port = Read(with("port: 6380", "port: 99999"));
    CHECK(!port && Has(port.error().message, "超出范围 [1, 65535]"));

    auto timeout = Read(with("database: admin", "database: admin, connect_timeout_ms: 0"));
    CHECK(!timeout && timeout.error().path == "mongo.connect_timeout_ms");

    // 四个 section 都必填：整段缺失要报错，不能悄悄取空默认值
    for (const char* section : {"cluster", "etcd", "redis", "mongo"})
    {
        const std::string y = std::string("a: 1\n") + kGood; // 先加一行无关键，避免删到只剩空文档
        std::string out;
        bool skipping = false;
        std::istringstream in(y);
        for (std::string line; std::getline(in, line);)
        {
            const bool top = !line.empty() && line[0] != ' ';
            if (top)
                skipping = line.rfind(std::string(section) + ":", 0) == 0;
            if (!skipping)
                out += line + "\n";
        }
        auto r = Read(out);
        CHECK(!r);
        CHECK(Has(r.error().message, std::string("缺少必填项 '") + section + "'"));
    }
    // 必填列表不能是空的
    auto noEndpoints = Read(with("['http://h:2379', 'http://h:2479']", "[]"));
    CHECK(!noEndpoints && noEndpoints.error().path == "etcd.endpoints" &&
          Has(noEndpoints.error().message, "至少需要 1 个元素"));
    auto noNodes = Read(with("[{ host: h, port: 6379 }, { host: h, port: 6380 }]", "[]"));
    CHECK(!noNodes && noNodes.error().path == "redis.nodes" && Has(noNodes.error().message, "至少需要 1 个元素"));
    auto noDb = Read(with(", database: admin", ""));
    CHECK(!noDb && Has(noDb.error().message, "缺少必填项 'database'"));
}

// 拼写错误能被查出来。
void TestTypoDetected()
{
    SetSecrets();
    auto cfg = Config::LoadString(
        "cluster: { name: g, env: dev, world_id: 1 }\n"
        "etcd: { endpoints: [a], dial_timout_ms: 1 }\n"
        "redis: { nodes: [{ host: h, port: 1 }], password: x }\n"
        "mongo: { uri: m, database: d }\n",
        "common.yaml");
    CHECK(cfg);
    CHECK(cfg->Get<CommonConfig>(""));
    const auto un = cfg->Unused();
    CHECK(un.size() == 1 && un[0].path == "etcd.dial_timout_ms" && un[0].line == 2);
}

// cluster.env 之外的字段都不允许热更；超时这类也不行（连接已建立）。
void TestNothingIsReloadable()
{
    SetSecrets();
    auto cur = Read(kGood);
    CHECK(cur);
    CommonConfig next = *cur;
    CHECK(ConfigNonReloadableChanges(*cur, next).empty());
    next.redis.connectTimeoutMs = 1;
    next.cluster.worldId = 9;
    CHECK((ConfigNonReloadableChanges(*cur, next) ==
           std::vector<std::string>{"cluster.world_id", "redis.connect_timeout_ms"}));
}

// 仓库里真实的 config/common.yaml + config/env/*.yaml：与 schema 对得上，没有拼错的键。
void TestShippedFiles()
{
    const std::string dir = BREEZE_CONFIG_DIR;
    auto load = [&](const char* env) { return Config::Load({dir + "/common.yaml", dir + "/env/" + env + ".yaml"}); };

    // dev 开箱即用：不注入任何东西也能读
    ::unsetenv("BREEZE_REDIS_PWD");
    ::unsetenv("BREEZE_MONGO_URI");
    ::unsetenv("BREEZE_WORLD_ID");
    auto dev = load("dev");
    CHECK(dev);
    auto devCfg = dev->Get<CommonConfig>("");
    CHECK(devCfg && devCfg->cluster.env == Env::Dev && devCfg->cluster.worldId == 1);
    CHECK(dev->Unused().empty());

    // test / prod：密钥与 world_id 必须注入，缺了一次报全
    for (const char* env : {"test", "prod"})
    {
        auto missing = load(env);
        CHECK(!missing);
        CHECK(Has(missing.error().message, "BREEZE_REDIS_PWD") && Has(missing.error().message, "BREEZE_MONGO_URI") &&
              Has(missing.error().message, "BREEZE_WORLD_ID"));
    }
    ::setenv("BREEZE_REDIS_PWD", "redis-secret-123", 1);
    ::setenv("BREEZE_MONGO_URI", "mongodb://admin:mongo-secret@h:27017/?authSource=admin", 1);
    ::setenv("BREEZE_WORLD_ID", "7", 1);
    for (const auto& [env, want] : {std::pair{"test", Env::Test}, std::pair{"prod", Env::Prod}})
    {
        auto c = load(env);
        CHECK(c);
        auto r = c->Get<CommonConfig>("");
        CHECK(r && r->cluster.env == want && r->cluster.worldId == 7 && r->cluster.name == "gameserver");
        CHECK(r->etcd.endpoints.size() == 3 && r->redis.nodes.size() == 3);
        CHECK(c->Unused().empty());
    }

    // common.yaml 是全量、带本机默认值的配置，单独就能读（此时环境变量都没设）
    ::unsetenv("BREEZE_REDIS_PWD");
    ::unsetenv("BREEZE_MONGO_URI");
    auto base = Config::Load({dir + "/common.yaml"});
    CHECK(base);
    auto baseCfg = base->Get<CommonConfig>("");
    CHECK(baseCfg && baseCfg->cluster.env == Env::Dev && baseCfg->etcd.dialTimeoutMs == 5000);
    CHECK(base->Unused().empty());
}

} // namespace

int main()
{
    TestGoodConfig();
    TestSecretsNeverPrinted();
    TestSecretsMustBeInjected();
    TestConstraints();
    TestTypoDetected();
    TestNothingIsReloadable();
    TestShippedFiles();
    std::puts("common_config_test: ok");
}
