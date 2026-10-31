// 无测试框架依赖：失败即 abort。

#include "framework/kernel/log_config_yaml.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include "base/logger/logger_macros.h"

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

Config Load(std::string_view yaml)
{
    auto c = Config::LoadString(yaml, "t.yaml");
    CHECK(c);
    return *c;
}

bool Has(const std::string& s, std::string_view needle) { return s.find(needle) != std::string::npos; }

void TestAllFields()
{
    auto cfg = Load(
        "log:\n"
        "  output: stdout\n"
        "  name: gatesvr_1\n"
        "  level: warning\n" // 大小写不敏感，接受 warning
        "  pattern: '%v'\n"
        "  file:  { dir: ../log, retain_hours: 24, max_file_bytes: 1048576, max_files_per_hour: 2 }\n"
        "  async: { enabled: false, queue_size: 100, threads: 2, overflow: drop_oldest }\n"
        "  flush: { level: Error, interval_sec: 5 }\n");
    auto c = cfg.Get<LoggerConfig>("log");
    CHECK(c);
    CHECK(c->output == Output::Stdout && c->name == "gatesvr_1" && c->level == Level::Warn && c->pattern == "%v");
    CHECK(c->file.dir == "../log" && c->file.retainHours == 24 && c->file.maxFileBytes == 1048576 &&
          c->file.maxFilesPerHour == 2);
    CHECK(!c->async.enabled && c->async.queueSize == 100 && c->async.threads == 2 &&
          c->async.overflow == Overflow::DropOldest);
    CHECK(c->flush.level == Level::Error && c->flush.intervalSec == 5);
    CHECK(cfg.Unused().empty()); // 每个键都被读到了
}

// 没写的键保持 LoggerConfig 自己的默认值，嵌套结构体也是。
void TestDefaultsKept()
{
    auto c = Load("log:\n  name: svc\n  file: { dir: /tmp/x }\n").Get<LoggerConfig>("log");
    CHECK(c);
    const LoggerConfig d;
    CHECK(c->name == "svc" && c->file.dir == "/tmp/x");
    CHECK(c->output == d.output && c->level == d.level && c->pattern == d.pattern);
    CHECK(c->file.retainHours == d.file.retainHours && c->file.maxFileBytes == d.file.maxFileBytes);
    CHECK(c->async.enabled == d.async.enabled && c->async.queueSize == d.async.queueSize);
    CHECK(c->flush.intervalSec == d.flush.intervalSec);
}

void TestErrors()
{
    auto level = Load("log:\n  level: verbose\n").Get<LoggerConfig>("log");
    CHECK(!level);
    CHECK(level.error().path == "log.level" && level.error().line == 2);
    CHECK(Has(level.error().message, "trace | debug | info")); // 列出可选值

    auto output = Load("log:\n  output: console\n").Get<LoggerConfig>("log");
    CHECK(!output && Has(output.error().message, "file | stdout | stderr"));

    auto overflow = Load("log:\n  async: { overflow: wait }\n").Get<LoggerConfig>("log");
    CHECK(!overflow && Has(overflow.error().message, "block | drop_oldest"));

    auto q = Load("log:\n  async: { queue_size: 0 }\n").Get<LoggerConfig>("log");
    CHECK(!q && q.error().path == "log.async.queue_size" && Has(q.error().message, "超出范围"));

    auto neg = Load("log:\n  file: { retain_hours: -1 }\n").Get<LoggerConfig>("log");
    CHECK(!neg && Has(neg.error().message, "非负整数"));
}

// 拼写错误能被查出来：这是不用 schema 也能防住"配了没生效"的关键。
void TestTypoDetected()
{
    auto cfg = Load(
        "log:\n"
        "  level: info\n"
        "  file:\n"
        "    retain_hour: 24\n"); // 少了个 s，第 4 行
    CHECK(cfg.Get<LoggerConfig>("log"));
    auto un = cfg.Unused();
    CHECK(un.size() == 1);
    CHECK(un[0].path == "log.file.retain_hour" && un[0].line == 4);
}

// 端到端：yaml → LoggerConfig → 真正能写日志的 Logger。
void TestBuildsWorkingLogger()
{
    const auto dir = fs::temp_directory_path() / std::format("breeze_logcfg_test_{}", ::getpid());
    fs::remove_all(dir);
    auto cfg =
        Load(std::format("log:\n"
                         "  name: fromyaml\n"
                         "  level: debug\n"
                         "  file: {{ dir: {} }}\n"
                         "  async: {{ enabled: false }}\n"
                         "  flush: {{ level: info }}\n",
                         dir.string()));
    auto lc = cfg.Get<LoggerConfig>("log");
    CHECK(lc);
    std::string err;
    auto lg = Logger::Create(*lc, err);
    CHECK(lg);
    BREEZE_LOG_DEBUG(lg, "hello-from-yaml {}", 7);
    lg->Flush();

    std::ifstream in(dir / "fromyaml.log");
    std::stringstream ss;
    ss << in.rdbuf();
    CHECK(Has(ss.str(), "hello-from-yaml 7"));
    lg.reset();
    fs::remove_all(dir);
}

} // namespace

int main()
{
    TestAllFields();
    TestDefaultsKept();
    TestErrors();
    TestTypoDetected();
    TestBuildsWorkingLogger();
    std::puts("log_config_yaml_test: ok");
}
