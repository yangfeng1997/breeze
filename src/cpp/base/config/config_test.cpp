// 无测试框架依赖：失败即 abort。

#include "base/config/config.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

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

enum class Mode
{
    Fast,
    Safe
};

struct Inner
{
    int a = 1;
    std::string s = "inner-default";
};

struct Outer
{
    std::string name;
    std::uint8_t small = 7;
    double ratio = 0.5;
    bool on = false;
    std::vector<std::string> tags;
    std::optional<int> opt = 5;
    std::map<std::string, int> limits;
    Inner inner;
    Mode mode = Mode::Fast;
    int port = 8000;
};

struct Item
{
    int a = 0;
};

} // namespace

namespace breeze
{

template <>
struct ConfigTraits<Inner> : StructTraits<Inner>
{
    static FieldTable<Inner> MakeFields() { return {Field("a", &Inner::a), Field("s", &Inner::s)}; }
};

template <>
struct ConfigTraits<Outer> : StructTraits<Outer>
{
    static FieldTable<Outer> MakeFields()
    {
        return {
            Field("name", &Outer::name).Required(),
            Field("small", &Outer::small),
            Field("ratio", &Outer::ratio),
            Field("on", &Outer::on),
            Field("tags", &Outer::tags),
            Field("opt", &Outer::opt),
            Field("limits", &Outer::limits),
            Field("inner", &Outer::inner),
            Field("mode", &Outer::mode).Enum({{"fast", Mode::Fast}, {"safe", Mode::Safe}}),
            Field("port", &Outer::port).Range(1, 65535),
        };
    }
};

// 只读 a：用来验证"序列里的结构体多出来的键会被查出来"。
template <>
struct ConfigTraits<Item> : StructTraits<Item>
{
    static FieldTable<Item> MakeFields() { return {Field("a", &Item::a)}; }
};

} // namespace breeze

namespace
{

Config Load(std::string_view yaml)
{
    auto c = Config::LoadString(yaml, "t.yaml");
    CHECK(c);
    return *c;
}

bool Has(const std::string& s, std::string_view needle) { return s.find(needle) != std::string::npos; }

void TestScalarsAndNested()
{
    auto cfg = Load(
        "svc:\n"
        "  name: gate\n"
        "  small: 200\n"
        "  ratio: 1.25\n"
        "  on: true\n"
        "  tags: [a, b]\n"
        "  opt: 9\n"
        "  limits: {x: 1, y: 2}\n"
        "  inner:\n"
        "    a: 42\n"
        "  mode: safe\n"
        "  port: 9000\n");
    auto o = cfg.Get<Outer>("svc");
    CHECK(o);
    CHECK(o->name == "gate" && o->small == 200 && o->ratio == 1.25 && o->on);
    CHECK((o->tags == std::vector<std::string>{"a", "b"}));
    CHECK(o->opt && *o->opt == 9);
    CHECK(o->limits.size() == 2 && o->limits.at("y") == 2);
    CHECK(o->inner.a == 42);
    CHECK(o->inner.s == "inner-default"); // 嵌套结构体里没写的键保留默认值
    CHECK(o->mode == Mode::Safe && o->port == 9000);
    CHECK(cfg.Unused().empty());
}

void TestDefaultsKept()
{
    auto o = Load("svc:\n  name: x\n").Get<Outer>("svc");
    CHECK(o);
    CHECK(o->small == 7 && o->ratio == 0.5 && !o->on && o->tags.empty());
    CHECK(o->opt && *o->opt == 5);
    CHECK(o->inner.a == 1 && o->mode == Mode::Fast && o->port == 8000);
}

void TestOptionalExplicitNull()
{
    auto o = Load("svc:\n  name: x\n  opt: ~\n").Get<Outer>("svc");
    CHECK(o);
    CHECK(!o->opt);
}

void TestRequiredMissingAndEmptySection()
{
    auto e = Load("svc:\n  port: 1\n").Get<Outer>("svc");
    CHECK(!e);
    CHECK(Has(e.error().message, "缺少必填项 'name'"));
    CHECK(e.error().path == "svc" && e.error().line == 1 && e.error().file == "t.yaml");

    // `svc:` 后面什么都没写 = 空表：默认值照样生效，必填项照样报错
    auto empty = Load("svc:\n").Get<Outer>("svc");
    CHECK(!empty && Has(empty.error().message, "缺少必填项"));
    auto inner = Load("inner:\n").Get<Inner>("inner");
    CHECK(inner && inner->a == 1);

    auto missing = Load("a: 1\n").Get<Outer>("svc");
    CHECK(!missing && Has(missing.error().message, "缺少配置段 'svc'"));
}

void TestErrorsCarryPathAndLine()
{
    auto bad = Load("svc:\n  name: x\n  port: abc\n").Get<Outer>("svc");
    CHECK(!bad);
    CHECK(bad.error().path == "svc.port" && bad.error().line == 3);
    CHECK(Has(bad.error().message, "期望整数"));
    CHECK(Has(bad.error().Format(), "t.yaml:3: svc.port: "));

    auto cases = std::vector<std::pair<const char*, const char*>>{
        {"port: 70000", "超出范围 [1, 65535]"}, // Range
        {"port: 0", "超出范围"},
        {"small: 300", "超出范围 [0, 255]"}, // 类型自带的范围
        {"small: -1", "期望非负整数"},
        {"on: yes", "期望 true 或 false"},
        {"on: True", "期望 true 或 false"},
        {"mode: turbo", "fast | safe"}, // Enum 错误里列出可选值
        {"tags: notalist", "期望序列"},
        {"inner: 5", "期望映射"},
        {"port:", "期望整数，实际是空值"},
        {"port: 1e3", "期望整数"}, // 不接受科学计数法
        {"port: 0x10", "期望整数"},
        {"port: 12abc", "期望整数"},
        {"ratio: abc", "期望数字"},
    };
    for (const auto& [line, expect] : cases)
    {
        auto r = Load(std::string("svc:\n  name: x\n  ") + line + "\n").Get<Outer>("svc");
        CHECK(!r);
        if (!Has(r.error().message, expect))
            std::fprintf(stderr, "case [%s]: got [%s], want [%s]\n", line, r.error().message.c_str(), expect);
        CHECK(Has(r.error().message, expect));
        CHECK(r.error().line == 3); // 都指向出错的那一行
    }
}

void TestGetOr()
{
    auto cfg = Load("a: 5\nb: abc\n");
    CHECK(*cfg.GetOr<int>("missing", 42) == 42);
    CHECK(*cfg.GetOr<int>("a", 42) == 5);
    auto bad = cfg.GetOr<int>("b", 42); // 存在但不合法：报错，不悄悄退回默认值
    CHECK(!bad);
}

void TestChildDoesNotInsertKeys()
{
    auto cfg = Load("svc:\n  name: x\n");
    CHECK(cfg.Get<Outer>("svc")); // 查了一堆不存在的键
    CHECK(cfg.Unused().empty());  // 没有因为查询而冒出空值键
}

std::string WriteTemp(const fs::path& dir, const char* name, const char* content)
{
    fs::create_directories(dir);
    const auto p = dir / name;
    std::ofstream(p) << content;
    return p.string();
}

void TestMergeOrderAndOrigin()
{
    const auto dir = fs::temp_directory_path() / std::format("breeze_config_test_{}", ::getpid());
    fs::remove_all(dir);
    const auto base = WriteTemp(dir,
                                "base.yaml",
                                "svc:\n"
                                "  name: base\n"
                                "  port: 1000\n"
                                "  tags: [a, b, c]\n"
                                "  limits: {x: 1}\n"
                                "other: 1\n");
    const auto over = WriteTemp(dir,
                                "over.yaml",
                                "svc:\n"
                                "  port: 2000\n"
                                "  tags: [z]\n"
                                "  limits: {y: 2}\n");
    auto cfg = Config::Load({base, over});
    CHECK(cfg);
    auto o = cfg->Get<Outer>("svc");
    CHECK(o);
    CHECK(o->name == "base");                          // 只有 base 写了
    CHECK(o->port == 2000);                            // 后者覆盖
    CHECK((o->tags == std::vector<std::string>{"z"})); // 序列整体替换
    CHECK(o->limits.size() == 2);                      // 映射递归合并
    CHECK(*cfg->GetOr<int>("other", 0) == 1);

    // 错误指向实际生效的那个文件和行
    const auto bad = WriteTemp(dir, "bad.yaml", "svc:\n  port: abc\n");
    auto cfg2 = Config::Load({base, bad});
    CHECK(cfg2);
    auto e = cfg2->Get<Outer>("svc");
    CHECK(!e);
    CHECK(e.error().file == bad && e.error().line == 2);
    // 没被覆盖的值出错，指向 base
    const auto bad2 = WriteTemp(dir, "bad2.yaml", "svc:\n  name: x\n");
    auto cfg3 = Config::Load({base, bad2});
    CHECK(cfg3);
    fs::remove_all(dir);
}

void TestUnusedKeys()
{
    auto cfg = Load(
        "svc:\n"
        "  name: x\n"
        "  porrt: 1\n" // 拼写错误，第 3 行
        "unused_top: 1\n"
        "list:\n"
        "  - k: 1\n"
        "    kk: 2\n");
    CHECK(cfg.Get<Outer>("svc"));
    CHECK((cfg.Get<std::vector<std::map<std::string, int>>>("list")));
    auto un = cfg.Unused();
    CHECK(un.size() == 2);
    CHECK(un[0].path == "svc.porrt" && un[0].line == 3 && un[0].file == "t.yaml");
    CHECK(un[1].path == "unused_top" && un[1].line == 4);
    CHECK(Has(un[0].Format(), "t.yaml:3: svc.porrt: "));

    cfg.Ignore("unused_top"); // 有意不读的子树
    CHECK(cfg.Unused().size() == 1);

    // 序列里的结构体多出来的键
    auto cfg2 = Load("items:\n  - a: 1\n    b: 2\n");
    CHECK(cfg2.Get<std::vector<Item>>("items"));
    auto un2 = cfg2.Unused();
    CHECK(un2.size() == 1 && un2[0].path == "items[0].b" && un2[0].line == 3);

    // 整段没人读：只报一条，不往下展开
    auto cfg3 = Load("orphan:\n  a: 1\n  b: 2\n");
    CHECK(cfg3.Unused().size() == 1);
}

void TestLoadErrors()
{
    auto missing = Config::Load({"/nonexistent/path.yaml"});
    CHECK(!missing && Has(missing.error().message, "文件不存在"));

    auto syntax = Config::LoadString("a: [1, 2\nb: 3\n", "s.yaml");
    CHECK(!syntax && syntax.error().file == "s.yaml" && syntax.error().line > 0);

    auto seq = Config::LoadString("- a\n- b\n", "s.yaml");
    CHECK(!seq && Has(seq.error().message, "顶层必须是映射"));

    auto empty = Config::LoadString("", "s.yaml");
    CHECK(empty); // 空文件 = 空表
    CHECK(empty->Unused().empty());
    CHECK(!empty->Get<int>("x"));
}

} // namespace

// ---------------------------------------------------------------------------------------------
// 占位符、密钥、热更对比
// ---------------------------------------------------------------------------------------------

namespace
{

struct Creds
{
    std::string user = "u";
    std::string password = "pw-secret";
    int pin = 1234;
};

struct Tuning
{
    int a = 1;
    int b = 2;
};

struct Svc
{
    std::string name = "svc";
    int port = 80;
    double ratio = 0.5;
    Creds creds;
    Inner inner;
    Tuning tuning;
    std::vector<int> ids;
    std::optional<int> opt;
    std::map<std::string, int> limits;
    std::vector<Creds> extra; // 故意没有 operator==：验证按字段表比较
    Mode mode = Mode::Fast;
    bool on = false;
};

} // namespace

namespace breeze
{

template <>
struct ConfigTraits<Creds> : StructTraits<Creds>
{
    static FieldTable<Creds> MakeFields()
    {
        return {
            Field("user", &Creds::user),
            Field("password", &Creds::password).Secret(),
            Field("pin", &Creds::pin).Secret(),
        };
    }
};

template <>
struct ConfigTraits<Tuning> : StructTraits<Tuning>
{
    static FieldTable<Tuning> MakeFields() { return {Field("a", &Tuning::a), Field("b", &Tuning::b)}; }
};

template <>
struct ConfigTraits<Svc> : StructTraits<Svc>
{
    static FieldTable<Svc> MakeFields()
    {
        return {
            Field("name", &Svc::name),
            Field("port", &Svc::port),
            Field("ratio", &Svc::ratio).Reloadable(),
            Field("creds", &Svc::creds),
            Field("inner", &Svc::inner),
            Field("tuning", &Svc::tuning).Reloadable(), // 整个子树可热更
            Field("ids", &Svc::ids),
            Field("opt", &Svc::opt),
            Field("limits", &Svc::limits),
            Field("extra", &Svc::extra),
            Field("mode", &Svc::mode).Enum({{"fast", Mode::Fast}, {"safe", Mode::Safe}}),
            Field("on", &Svc::on),
        };
    }
};

} // namespace breeze

namespace
{

std::string Str(const Config& c, const char* path)
{
    auto v = c.Get<std::string>(path);
    CHECK(v);
    return *v;
}

void TestEnvExpansion()
{
    ::setenv("BREEZE_T_HOST", "h1", 1);
    ::setenv("BREEZE_T_PORT", "6379", 1);
    ::unsetenv("BREEZE_T_MISSING");
    auto cfg = Load(
        "host: ${ENV:BREEZE_T_HOST}\n"
        "port: ${ENV:BREEZE_T_PORT}\n"
        "url: redis://${ENV:BREEZE_T_HOST}:${ENV:BREEZE_T_PORT}/0\n"
        "def: ${ENV:BREEZE_T_MISSING:-fallback}\n"
        "empty: ${ENV:BREEZE_T_MISSING:-}\n"
        "lit: $${not_a_var}\n"
        "\"${ENV:BREEZE_T_MISSING}\": 1\n" // 键不展开
        "nested:\n"
        "  list:\n"
        "    - ${ENV:BREEZE_T_HOST}\n"
        "    - x\n");
    CHECK(Str(cfg, "host") == "h1");
    CHECK(*cfg.Get<int>("port") == 6379); // 展开后按目标类型解析
    CHECK(Str(cfg, "url") == "redis://h1:6379/0");
    CHECK(Str(cfg, "def") == "fallback");
    CHECK(Str(cfg, "empty").empty());
    CHECK(Str(cfg, "lit") == "${not_a_var}"); // $${ 转义
    auto list = cfg.Get<std::vector<std::string>>("nested.list");
    CHECK(list && (*list == std::vector<std::string>{"h1", "x"}));
}

void TestEnvMissingReportsAllAtOnce()
{
    ::unsetenv("BREEZE_T_NOPE_A");
    ::unsetenv("BREEZE_T_NOPE_B");
    auto r = Config::LoadString("a: ${ENV:BREEZE_T_NOPE_A}\nb: 1\nc:\n  d: ${ENV:BREEZE_T_NOPE_B}\n", "t.yaml");
    CHECK(!r);
    const auto& m = r.error().message;
    CHECK(Has(m, "共 2 处错误"));
    CHECK(Has(m, "BREEZE_T_NOPE_A") && Has(m, "BREEZE_T_NOPE_B")); // 一次列全
    CHECK(Has(m, "t.yaml:1: a: ") && Has(m, "t.yaml:4: c.d: "));   // 各带自己的位置

    auto one = Config::LoadString("a: ${ENV:BREEZE_T_NOPE_A}\n", "t.yaml");
    CHECK(!one && one.error().path == "a" && one.error().line == 1 && Has(one.error().message, "未设置"));
}

void TestPlaceholderErrors()
{
    auto tmpl = Config::LoadString("port: ${gate_port}\n", "t.yaml");
    CHECK(!tmpl && Has(tmpl.error().message, "未替换的占位符") && Has(tmpl.error().message, "gate_port"));

    auto open = Config::LoadString("a: ${ENV:X\n", "t.yaml");
    CHECK(!open && Has(open.error().message, "缺少结尾"));

    auto bad = Config::LoadString("a: ${ENV:1BAD}\n", "t.yaml");
    CHECK(!bad && Has(bad.error().message, "不合法"));
}

void TestFilePlaceholder()
{
    const auto dir = fs::temp_directory_path() / std::format("breeze_config_secret_{}", ::getpid());
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream(dir / "unix") << "s3cr3t\n";
        std::ofstream(dir / "crlf", std::ios::binary) << "pw\r\n";
    }
    auto cfg = Load(std::format("a: ${{FILE:{}/unix}}\nb: ${{FILE:{}/crlf}}\n", dir.string(), dir.string()));
    CHECK(Str(cfg, "a") == "s3cr3t"); // 末尾换行去掉
    CHECK(Str(cfg, "b") == "pw");

    auto missing = Config::LoadString("a: ${FILE:/nonexistent/secret}\n", "t.yaml");
    CHECK(!missing && Has(missing.error().message, "无法读取文件"));
    fs::remove_all(dir);
}

// 展开发生在合并之后：被后面文件覆盖掉的值，引用了没设置的环境变量也不算错。
void TestOnlyEffectiveValuesAreExpanded()
{
    ::unsetenv("BREEZE_T_UNSET_PW");
    const auto dir = fs::temp_directory_path() / std::format("breeze_config_eff_{}", ::getpid());
    fs::remove_all(dir);
    const auto base = WriteTemp(dir, "base.yaml", "pw: ${ENV:BREEZE_T_UNSET_PW}\nkeep: 1\n");
    const auto over = WriteTemp(dir, "over.yaml", "pw: literal\n");

    auto ok = Config::Load({base, over});
    CHECK(ok);
    CHECK(Str(*ok, "pw") == "literal");

    auto bad = Config::Load({base}); // 不覆盖就是错的，且指向 base 文件的第 1 行
    CHECK(!bad);
    CHECK(bad.error().file == base && bad.error().line == 1 && bad.error().path == "pw");
    fs::remove_all(dir);
}

Svc Sample()
{
    Svc s;
    s.ids = {1, 2, 3};
    s.limits = {{"x", 1}};
    return s;
}

void TestDumpMasksSecrets()
{
    const Svc s = Sample();
    const std::string text = ConfigDump(s);
    CHECK(text ==
          "name: \"svc\"\n"
          "port: 80\n"
          "ratio: 0.5\n"
          "creds:\n"
          "  user: \"u\"\n"
          "  password: ***\n"
          "  pin: ***\n"
          "inner:\n"
          "  a: 1\n"
          "  s: \"inner-default\"\n"
          "tuning:\n"
          "  a: 1\n"
          "  b: 2\n"
          "ids: [1, 2, 3]\n"
          "opt: ~\n"
          "limits:\n"
          "  x: 1\n"
          "extra: []\n"
          "mode: fast\n"
          "on: false\n");
    CHECK(!Has(text, "pw-secret") && !Has(text, "1234")); // 真实值永远不出现

    Svc t = Sample();
    t.opt = 7;
    t.mode = Mode::Safe;
    t.limits.clear();
    t.extra = {Creds{"a", "p2", 9}};
    const std::string text2 = ConfigDump(t);
    CHECK(Has(text2, "opt: 7\n") && Has(text2, "mode: safe\n") && Has(text2, "limits: {}\n"));
    CHECK(Has(text2, "extra: [{user: \"a\", password: ***, pin: ***}]\n")); // 容器里的结构体也打码
    CHECK(!Has(text2, "p2"));
}

void TestRedactedCopy()
{
    Svc s = Sample();
    s.extra = {Creds{"a", "p2", 9}};
    const Svc r = ConfigRedacted(s);
    CHECK(r.creds.password == "***" && r.creds.pin == 0); // 字符串 ***，其余零值
    CHECK(r.creds.user == "u" && r.name == "svc" && r.port == 80);
    CHECK(r.extra.size() == 1 && r.extra[0].password == "***" && r.extra[0].user == "a"); // 容器元素递归
    CHECK(s.creds.password == "pw-secret" && s.extra[0].password == "p2");                // 原对象不动
}

void TestFieldTableBuiltOnce()
{
    CHECK(&ConfigTraits<Svc>::Fields() == &ConfigTraits<Svc>::Fields());
    CHECK(ConfigTraits<Svc>::Fields().fields.size() == 12);
}

void TestReloadDiff()
{
    const Svc cur = Sample();

    // 没改 / 只改了允许热更的：干净
    Svc cand = cur;
    CHECK(ConfigNonReloadableChanges(cur, cand).empty());
    cand.ratio = 9.9;  // Reloadable()
    cand.tuning.a = 5; // tuning 整个子树 Reloadable()
    cand.tuning.b = 6;
    CHECK(ConfigNonReloadableChanges(cur, cand).empty());
    CHECK(ConfigCheckReload(cur, cand));

    // 不允许变的：全部列出，点号路径，按字段表顺序
    cand.port = 81;
    cand.inner.a = 7;                    // 嵌套结构体里没标 Reloadable 的字段
    cand.creds.password = "hunter2-new"; // Secret 一定不可热更
    cand.ids = {9};
    cand.opt = 1;
    cand.limits["y"] = 2;
    cand.extra.push_back(Creds{}); // 元素没有 operator==，靠字段表比较
    cand.mode = Mode::Safe;
    const auto changed = ConfigNonReloadableChanges(cur, cand);
    CHECK((changed ==
           std::vector<std::string>{"port", "creds.password", "inner.a", "ids", "opt", "limits", "extra", "mode"}));

    auto r = ConfigCheckReload(cur, cand);
    CHECK(!r);
    CHECK(Has(r.error().message, "以下配置项不支持热更: port, creds.password, inner.a"));
    CHECK(!Has(r.error().message, "hunter2-new")); // 只列路径，不带值

    // 只改一个标量也能查出来
    Svc one = cur;
    one.name = "other";
    CHECK((ConfigNonReloadableChanges(cur, one) == std::vector<std::string>{"name"}));
}

} // namespace

int main()
{
    TestScalarsAndNested();
    TestDefaultsKept();
    TestOptionalExplicitNull();
    TestRequiredMissingAndEmptySection();
    TestErrorsCarryPathAndLine();
    TestGetOr();
    TestChildDoesNotInsertKeys();
    TestMergeOrderAndOrigin();
    TestUnusedKeys();
    TestLoadErrors();
    TestEnvExpansion();
    TestEnvMissingReportsAllAtOnce();
    TestPlaceholderErrors();
    TestFilePlaceholder();
    TestOnlyEffectiveValuesAreExpanded();
    TestDumpMasksSecrets();
    TestRedactedCopy();
    TestFieldTableBuiltOnce();
    TestReloadDiff();
    std::puts("config_test: ok");
}
