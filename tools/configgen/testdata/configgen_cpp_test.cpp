// 端到端：schema → 生成的头文件 → base/config 运行时。样例 schema 在
// testdata/schema/。 无测试框架依赖：失败即 abort。

#include <cstdio>
#include <cstdlib>

#include "sample_config.h"

using namespace breeze;
using config::sample::Creds;
using config::sample::GateConfig;
using config::sample::Mode;
using config::sample_types::Level;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,    \
                   #cond);                                                     \
      std::abort();                                                            \
    }                                                                          \
  } while (0)

namespace {

Config Load(std::string_view yaml) {
  auto c = Config::LoadString(yaml, "g.yaml");
  CHECK(c);
  return *c;
}

bool Has(const std::string &s, std::string_view needle) {
  return s.find(needle) != std::string::npos;
}

// 只写必填项：其余字段全部取 schema 里声明的默认值。
void TestDefaults() {
  auto g = Load("g:\n  tcp_port: 8000\n").Get<GateConfig>("g");
  CHECK(g);
  CHECK(g->tcpPort == 8000);
  CHECK(g->name == "gate" && g->heartbeatSec == 30 && g->ratio == 0.5);
  CHECK(g->big == 18446744073709551615ULL && g->neg == -5 &&
        g->smallF == 1.5f && g->on);
  CHECK(g->mode == Mode::Fast);
  CHECK(g->level == Level::Info); // import 进来的枚举，默认值也生效
  CHECK(g->creds.user == "u" && g->tuning.a == 1 && g->tuning.b == 2);
  CHECK(g->modes.empty() && g->endpoints.empty() && g->limits.empty() &&
        !g->opt && !g->optEp);
}

// 每一种类型都读一遍。
void TestFullDocument() {
  auto g = Load("g:\n"
                "  name: gw1\n"
                "  tcp_port: 9000\n"
                "  heartbeat_sec: 10\n"
                "  ratio: 0.25\n"
                "  neg: 3\n"
                "  on: false\n" // 键 on：schema 里要加引号，数据文件里 yaml-cpp
                                // 按 YAML 1.2 当字符串
                "  mode: drop_oldest\n"
                "  level: error\n"
                "  modes: [safe, drop_oldest]\n"
                "  creds: { user: bob, password: hunter2, pin: 4321 }\n"
                "  tuning: { a: 7 }\n"
                "  endpoints:\n"
                "    - { host: a, port: 80 }\n"
                "    - { host: b, port: 443 }\n"
                "  limits: { x: 1, y: 2 }\n"
                "  by_name: { main: { host: m, port: 1 } }\n"
                "  opt: 5\n"
                "  opt_ep: { host: o, port: 2 }\n"
                "  tags: [t1, t2]\n")
               .Get<GateConfig>("g");
  CHECK(g);
  CHECK(g->name == "gw1" && g->tcpPort == 9000 && g->heartbeatSec == 10 &&
        g->ratio == 0.25 && g->neg == 3);
  CHECK(!g->on && g->mode == Mode::DropOldest && g->level == Level::Error);
  CHECK((g->modes ==
         std::vector<Mode>{Mode::Safe, Mode::DropOldest})); // list<枚举>
  CHECK(g->creds.user == "bob" && g->creds.password == "hunter2" &&
        g->creds.pin == 4321);
  CHECK(g->tuning.a == 7 && g->tuning.b == 2); // 嵌套结构体里没写的键保持默认
  CHECK(g->endpoints.size() == 2 && g->endpoints[1].host == "b" &&
        g->endpoints[1].port == 443);
  CHECK(g->limits.at("y") == 2);
  CHECK(g->byName.at("main").host == "m");
  CHECK(g->opt && *g->opt == 5 && g->optEp && g->optEp->port == 2);
  CHECK((g->tags == std::vector<std::string>{"t1", "t2"}));
}

void TestSchemaConstraintsAreEnforced() {
  auto required = Load("g:\n  name: x\n").Get<GateConfig>("g");
  CHECK(!required && Has(required.error().message, "缺少必填项 'tcp_port'"));

  auto range = Load("g:\n  tcp_port: 70000\n").Get<GateConfig>("g");
  CHECK(!range && range.error().path == "g.tcp_port" &&
        range.error().line == 2);
  CHECK(Has(range.error().message, "超出范围 [1, 65535]"));

  auto negRange = Load("g:\n  tcp_port: 1\n  neg: 11\n").Get<GateConfig>("g");
  CHECK(!negRange && Has(negRange.error().message, "超出范围 [-10, 10]"));

  auto mode = Load("g:\n  tcp_port: 1\n  mode: turbo\n").Get<GateConfig>("g");
  CHECK(!mode && Has(mode.error().message,
                     "fast | safe | drop_oldest")); // 报错里列出可选值

  auto level = Load("g:\n  tcp_port: 1\n  level: trace\n").Get<GateConfig>("g");
  CHECK(!level && Has(level.error().message,
                      "debug | info | warn | error")); // import 的枚举同样

  // 嵌套 / 容器里的约束：路径精确到元素
  auto elem = Load("g:\n  tcp_port: 1\n  endpoints:\n    - { host: a }\n")
                  .Get<GateConfig>("g");
  CHECK(!elem && elem.error().path == "g.endpoints[0]" &&
        Has(elem.error().message, "缺少必填项 'port'"));
  auto badEnum =
      Load("g:\n  tcp_port: 1\n  modes: [fast, nope]\n").Get<GateConfig>("g");
  CHECK(!badEnum && badEnum.error().path == "g.modes[1]");

  // min_count：写了空列表也算违反；没写则不检查
  auto emptySeeds =
      Load("g:\n  tcp_port: 1\n  seeds: []\n").Get<GateConfig>("g");
  CHECK(!emptySeeds && emptySeeds.error().path == "g.seeds" &&
        Has(emptySeeds.error().message, "至少需要 1 个元素"));
  CHECK(Load("g:\n  tcp_port: 1\n  seeds: [a]\n").Get<GateConfig>("g"));
  CHECK(Load("g:\n  tcp_port: 1\n").Get<GateConfig>("g"));
}

// 枚举没有显式默认值时落在 Unspecified，且 Unspecified 不是合法的配置值。
void TestEnumWithoutDefaultIsUnspecified() {
  struct Probe {
    Mode m{};
  };
  CHECK(Probe{}.m == Mode::Unspecified);
  CHECK(ConfigTraits<Mode>::Table().size() == 3);

  auto bad =
      Load("g:\n  tcp_port: 1\n  mode: unspecified\n").Get<GateConfig>("g");
  CHECK(!bad && Has(bad.error().message, "fast | safe | drop_oldest"));
}

void TestTypoDetected() {
  auto cfg = Load("g:\n  tcp_port: 1\n  tcp_prot: 2\n");
  CHECK(cfg.Get<GateConfig>("g"));
  auto un = cfg.Unused();
  CHECK(un.size() == 1 && un[0].path == "g.tcp_prot" && un[0].line == 3);
}

GateConfig Sample() {
  GateConfig g;
  g.tcpPort = 8000;
  g.creds.password = "hunter2";
  g.creds.pin = 4321;
  g.modes = {Mode::Safe};
  g.endpoints = {{"a", 80}};
  return g;
}

// secret 来自 schema：Dump 与 Redacted 里都不会出现真实值。
void TestSecretsFromSchema() {
  const GateConfig g = Sample();
  const std::string text = ConfigDump(g);
  CHECK(Has(text, "password: ***") && Has(text, "pin: ***"));
  CHECK(!Has(text, "hunter2") && !Has(text, "4321"));
  CHECK(Has(text, "mode: fast") &&
        Has(text, "level: info"));   // 枚举输出名字，不是整数
  CHECK(Has(text, "modes: [safe]")); // list<枚举> 也是
  CHECK(Has(text, "endpoints: [{host: \"a\", port: 80}]"));

  const GateConfig r = ConfigRedacted(g);
  CHECK(r.creds.password == "***" && r.creds.pin == 0 && r.creds.user == "u");
  CHECK(g.creds.password == "hunter2"); // 原对象不动
}

// reloadable 来自 schema：标了的可以变，其余的变了就被列出来。
void TestReloadableFromSchema() {
  const GateConfig cur = Sample();
  GateConfig cand = cur;
  cand.heartbeatSec = 99; // reloadable
  cand.ratio = 0.9;       // reloadable
  cand.tuning.a = 100;    // tuning 整个子树 reloadable
  CHECK(ConfigNonReloadableChanges(cur, cand).empty());

  cand.tcpPort = 8001;
  cand.name = "other";
  cand.creds.password = "new"; // secret 一定不可热更
  cand.endpoints.push_back({"b", 81});
  const auto changed = ConfigNonReloadableChanges(cur, cand);
  CHECK((changed == std::vector<std::string>{"name", "tcp_port",
                                             "creds.password", "endpoints"}));
}

} // namespace

int main() {
  TestDefaults();
  TestFullDocument();
  TestSchemaConstraintsAreEnforced();
  TestEnumWithoutDefaultIsUnspecified();
  TestTypoDetected();
  TestSecretsFromSchema();
  TestReloadableFromSchema();
  std::puts("configgen_cpp_test: ok");
}
