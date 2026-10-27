// 无测试框架依赖：失败即 abort。文件都写在 /tmp 下（软链在 DrvFs 上不可靠）。
// Fatal 用例 fork 子进程：必须在父进程没有任何 Logger（即没有后台线程）时进行。

#include "logger.h"

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include "hourly_file_sink.h"
#include "logger_macros.h"

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
    auto p = fs::temp_directory_path() / std::format("breeze_logger_test_{}_{}", ::getpid(), tag);
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

spdlog::log_clock::time_point At(int y, int mo, int d, int h, int mi, int s = 0)
{
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    tm.tm_isdst = -1;
    return spdlog::log_clock::from_time_t(std::mktime(&tm));
}

void Feed(HourlyFileSinkMt& sink, spdlog::log_clock::time_point t, std::string_view text)
{
    spdlog::details::log_msg msg(spdlog::source_loc{}, "t", spdlog::level::info, spdlog::string_view_t(text));
    msg.time = t;
    sink.log(msg);
    sink.flush();
}

void TestRotateByHourAndSymlink()
{
    const auto dir = TempDir("rotate");
    fs::create_directories(dir);
    HourlyFileSinkMt sink(dir, "t", 0);

    Feed(sink, At(2026, 10, 26, 10, 59, 59), "before");
    Feed(sink, At(2026, 10, 26, 11, 0, 0), "after");

    CHECK(Has(ReadAll(dir / "t.2026-10-26_10.log"), "before"));
    CHECK(!Has(ReadAll(dir / "t.2026-10-26_10.log"), "after"));
    CHECK(Has(ReadAll(dir / "t.2026-10-26_11.log"), "after"));
    CHECK(fs::is_symlink(dir / "t.log"));
    CHECK(fs::read_symlink(dir / "t.log") == "t.2026-10-26_11.log");
    CHECK(Has(ReadAll(dir / "t.log"), "after"));
    fs::remove_all(dir);
}

void TestClockGoingBackwardsReopens()
{
    const auto dir = TempDir("backward");
    fs::create_directories(dir);
    HourlyFileSinkMt sink(dir, "t", 0);

    Feed(sink, At(2026, 10, 26, 11, 30), "late");
    Feed(sink, At(2026, 10, 26, 10, 30), "early");

    CHECK(Has(ReadAll(dir / "t.2026-10-26_10.log"), "early"));
    CHECK(!Has(ReadAll(dir / "t.2026-10-26_11.log"), "early"));
    fs::remove_all(dir);
}

void TestRetentionOnlyTouchesOwnOldFiles()
{
    const auto dir = TempDir("retain");
    fs::create_directories(dir);
    for (const char* f : {"t.2000-01-01_00.log",
                          "t.2026-10-23_00.log",
                          "t.2026-10-25_00.log",
                          "other.2000-01-01_00.log",
                          "t.notdigits.log",
                          "t.2000-01-01_00.log.bak",
                          // 时间串写法不对：宁可漏删也不误删
                          "t.1999123100.log",      // 旧格式（纯数字）
                          "t.2000-01-01-00.log",   // 小时前是 '-' 不是 '_'
                          "t.2000.01.01_00.log",   // 点号分隔
                          "t.2000-01-01_0x.log",   // 非数字
                          "t.2000-01-01_000.log"}) // 长度不对
        std::ofstream(dir / f) << "x";

    HourlyFileSinkMt sink(dir, "t", 48); // 2026-10-26 12 点往前 48 小时：截止 2026-10-24_12
    Feed(sink, At(2026, 10, 26, 12, 0), "now");

    CHECK(!fs::exists(dir / "t.2000-01-01_00.log"));
    CHECK(!fs::exists(dir / "t.2026-10-23_00.log"));
    CHECK(fs::exists(dir / "t.2026-10-25_00.log"));
    CHECK(fs::exists(dir / "other.2000-01-01_00.log"));
    CHECK(fs::exists(dir / "t.notdigits.log"));
    CHECK(fs::exists(dir / "t.2000-01-01_00.log.bak"));
    for (const char* f : {"t.1999123100.log",
                          "t.2000-01-01-00.log",
                          "t.2000.01.01_00.log",
                          "t.2000-01-01_0x.log",
                          "t.2000-01-01_000.log"})
        CHECK(fs::exists(dir / f));
    CHECK(fs::exists(dir / "t.2026-10-26_12.log"));
    fs::remove_all(dir);
}

// 名字带点号表示类别：gatesvr / gatesvr.res / gatesvr.tracing 共用一个目录，互不干扰。
void TestDottedNamesDontTouchEachOther()
{
    const auto dir = TempDir("dotted");
    fs::create_directories(dir);
    for (const char* f : {"gatesvr.2000-01-01_00.log",
                          "gatesvr.res.2000-01-01_00.log",
                          "gatesvr.tracing.2000-01-01_00.log",
                          "gatesvr.stdout.log",
                          "gatesvr.stderr.log"})
        std::ofstream(dir / f) << "x";

    {
        HourlyFileSinkMt main(dir, "gatesvr", 24);
        Feed(main, At(2026, 10, 26, 12, 0), "m");
    }
    CHECK(!fs::exists(dir / "gatesvr.2000-01-01_00.log")); // 只清自己的
    CHECK(fs::exists(dir / "gatesvr.res.2000-01-01_00.log"));
    CHECK(fs::exists(dir / "gatesvr.tracing.2000-01-01_00.log"));
    CHECK(fs::exists(dir / "gatesvr.stdout.log"));
    CHECK(fs::exists(dir / "gatesvr.stderr.log"));

    {
        HourlyFileSinkMt res(dir, "gatesvr.res", 24);
        Feed(res, At(2026, 10, 26, 12, 0), "r");
    }
    CHECK(!fs::exists(dir / "gatesvr.res.2000-01-01_00.log"));
    CHECK(fs::exists(dir / "gatesvr.tracing.2000-01-01_00.log"));
    CHECK(fs::exists(dir / "gatesvr.2026-10-26_12.log")); // 没被 res 的清理波及

    // 各自的小时文件与软链
    CHECK(fs::read_symlink(dir / "gatesvr.log") == "gatesvr.2026-10-26_12.log");
    CHECK(fs::read_symlink(dir / "gatesvr.res.log") == "gatesvr.res.2026-10-26_12.log");
    CHECK(Has(ReadAll(dir / "gatesvr.log"), "m"));
    CHECK(Has(ReadAll(dir / "gatesvr.res.log"), "r"));
    fs::remove_all(dir);
}

LoggerConfig Cfg(const fs::path& dir, bool async)
{
    LoggerConfig c;
    c.dir = dir.string();
    c.name = "svc";
    c.level = Level::Info; // 显式：默认值随构建类型变，用例断言的是 Info 下的行为
    c.async = async;
    return c;
}

void TestDefaultLevelFollowsBuildType()
{
#ifdef NDEBUG
    CHECK(DefaultLevel() == Level::Info);
#else
    CHECK(DefaultLevel() == Level::Debug);
#endif
    CHECK(LoggerConfig{}.level == DefaultLevel());
}

void TestLevelNamesRoundTrip()
{
    for (const Level l : {Level::Trace, Level::Debug, Level::Info, Level::Warn, Level::Error, Level::Fatal, Level::Off})
    {
        const auto parsed = ParseLevel(ToString(l));
        CHECK(parsed && *parsed == l);
    }
    CHECK(*ParseLevel("INFO") == Level::Info);
    CHECK(*ParseLevel("Warning") == Level::Warn);
    CHECK(*ParseLevel("DeBuG") == Level::Debug);
    CHECK(!ParseLevel(""));
    CHECK(!ParseLevel("verbose"));
    CHECK(!ParseLevel("info "));
}

// 默认 logger 的 SetLogLevel / GetLogLevel：运行时随时改，立即生效。
void TestSetLogLevelOnDefaultLogger()
{
    const auto dir = TempDir("setlevel");
    std::string err;
    auto lg = Logger::Create(Cfg(dir, false), err); // Info
    CHECK(lg);
    g_default_logger.store(lg.get());

    CHECK(GetLogLevel() == Level::Info);
    LOG_DEBUG("dbg-before");
    SetLogLevel(Level::Debug);
    CHECK(GetLogLevel() == Level::Debug);
    LOG_DEBUG("dbg-after");
    SetLogLevel(Level::Error);
    LOG_WARN("warn-hidden");
    LOG_ERROR("err-shown");
    lg->Flush();
    g_default_logger.store(nullptr);

    const auto text = ReadAll(dir / "svc.log");
    CHECK(!Has(text, "dbg-before"));
    CHECK(Has(text, "dbg-after"));
    CHECK(!Has(text, "warn-hidden"));
    CHECK(Has(text, "err-shown"));
    fs::remove_all(dir);
}

void TestLevelFilterAndRuntimeSetLevel()
{
    const auto dir = TempDir("level");
    std::string err;
    auto lg = Logger::Create(Cfg(dir, false), err);
    CHECK(lg);

    BREEZE_LOG_DEBUG(lg, "hidden-{}", 1);
    BREEZE_LOG_INFO(lg, "shown x={}", 42);
    lg->SetLevel(Level::Debug);
    BREEZE_LOG_DEBUG(lg, "now-visible-{}", 2);
    lg->Flush();

    const auto text = ReadAll(dir / "svc.log");
    CHECK(!Has(text, "hidden-1"));
    CHECK(Has(text, "shown x=42"));
    CHECK(Has(text, "now-visible-2"));
    CHECK(Has(text, "logger_test.cpp")); // %s：调用点的文件名
    fs::remove_all(dir);
}

void TestArgsNotEvaluatedWhenFiltered()
{
    const auto dir = TempDir("lazy");
    std::string err;
    auto lg = Logger::Create(Cfg(dir, false), err);
    CHECK(lg);

    int calls = 0;
    auto expensive = [&]
    {
        ++calls;
        return 1;
    };
    BREEZE_LOG_DEBUG(lg, "{}", expensive());
    CHECK(calls == 0);
    BREEZE_LOG_INFO(lg, "{}", expensive());
    CHECK(calls == 1);
    fs::remove_all(dir);
}

void TestAsyncDrainsOnDestroy()
{
    const auto dir = TempDir("async");
    std::string err;
    {
        auto lg = Logger::Create(Cfg(dir, true), err);
        CHECK(lg);
        for (int i = 0; i < 1000; ++i)
            BREEZE_LOG_INFO(lg, "line-{}", i);
    }
    const auto text = ReadAll(dir / "svc.log");
    CHECK(Has(text, "line-0"));
    CHECK(Has(text, "line-999"));
    fs::remove_all(dir);
}

void TestCreateFailureReportsError()
{
    const auto dir = TempDir("fail");
    fs::create_directories(dir);
    std::ofstream(dir / "file") << "x";

    std::string err;
    auto lg = Logger::Create(Cfg(dir / "file" / "sub", false), err);
    CHECK(!lg);
    CHECK(!err.empty());
    fs::remove_all(dir);
}

// 把 stdout / stderr 临时重定向到文件，析构时恢复。Logger 必须在本对象析构前销毁或 Flush。
class FdCapture
{
public:
    FdCapture(int fd, std::FILE* stream, const fs::path& file)
        : fd_(fd)
        , stream_(stream)
    {
        std::fflush(stream_);
        saved_ = ::dup(fd_);
        const int f = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        CHECK(f >= 0);
        ::dup2(f, fd_);
        ::close(f);
    }
    ~FdCapture()
    {
        std::fflush(stream_);
        ::dup2(saved_, fd_);
        ::close(saved_);
    }

private:
    int fd_;
    std::FILE* stream_;
    int saved_;
};

struct StdoutCapture : FdCapture
{
    explicit StdoutCapture(const fs::path& file)
        : FdCapture(STDOUT_FILENO, stdout, file)
    {
    }
};

struct StderrCapture : FdCapture
{
    explicit StderrCapture(const fs::path& file)
        : FdCapture(STDERR_FILENO, stderr, file)
    {
    }
};

LoggerConfig StdoutCfg(const fs::path& dir, bool async)
{
    auto c = Cfg(dir / "logs", async);
    c.output = Output::Stdout;
    return c;
}

void TestStdoutModeWritesNoFiles()
{
    const auto dir = TempDir("stdout");
    fs::create_directories(dir);
    {
        StdoutCapture cap(dir / "out.txt");
        std::string err;
        auto lg = Logger::Create(StdoutCfg(dir, false), err);
        CHECK(lg);
        BREEZE_LOG_DEBUG(lg, "hidden-{}", 1);
        BREEZE_LOG_INFO(lg, "hello-stdout {}", 1);
        lg->Flush();
    }
    const auto text = ReadAll(dir / "out.txt");
    CHECK(Has(text, "hello-stdout 1"));
    CHECK(!Has(text, "hidden-1"));
    CHECK(Has(text, "logger_test.cpp"));
    CHECK(!Has(text, "\x1b["));       // 没有 ANSI 颜色转义
    CHECK(!fs::exists(dir / "logs")); // 没建目录，更没有文件与软链
    fs::remove_all(dir);
}

void TestStdoutAsyncDrainsOnDestroy()
{
    const auto dir = TempDir("stdout_async");
    fs::create_directories(dir);
    {
        StdoutCapture cap(dir / "out.txt");
        std::string err;
        auto lg = Logger::Create(StdoutCfg(dir, true), err);
        CHECK(lg);
        for (int i = 0; i < 500; ++i)
            BREEZE_LOG_INFO(lg, "line-{}", i);
    }
    const auto text = ReadAll(dir / "out.txt");
    CHECK(Has(text, "line-0"));
    CHECK(Has(text, "line-499"));
    CHECK(!fs::exists(dir / "logs"));
    fs::remove_all(dir);
}

void TestStdoutModeIgnoresUnwritableDir()
{
    // 只读根文件系统下 dir 建不出来，Stdout 模式不该因此失败。
    const auto dir = TempDir("stdout_ro");
    fs::create_directories(dir);
    std::ofstream(dir / "file") << "x";
    {
        StdoutCapture cap(dir / "out.txt");
        std::string err;
        auto c = StdoutCfg(dir, false);
        c.dir = (dir / "file" / "sub").string();
        auto lg = Logger::Create(c, err);
        CHECK(lg);
        BREEZE_LOG_INFO(lg, "still-works");
        lg->Flush();
    }
    CHECK(Has(ReadAll(dir / "out.txt"), "still-works"));
    fs::remove_all(dir);
}

// 业务层给自己的 logger 配一族宏：套内层 BREEZE_LOG_<LEVEL>，写到自己的文件，默认 logger 不受影响。
Logger* g_extra = nullptr;
#define EXTRA_LOG_INFO(...) BREEZE_LOG_INFO(g_extra, __VA_ARGS__)
#define EXTRA_LOG_DEBUG(...) BREEZE_LOG_DEBUG(g_extra, __VA_ARGS__)

void TestBusinessMacroFamilyOnTopOfInnerMacros()
{
    const auto dir = TempDir("extra");
    std::string err;
    auto mainLg = Logger::Create(Cfg(dir, false), err); // svc.log，Info
    CHECK(mainLg);
    auto cfg = Cfg(dir, false);
    cfg.name = "svc.res";
    auto extraLg = Logger::Create(cfg, err);
    CHECK(extraLg);

    g_default_logger.store(mainLg.get());
    g_extra = extraLg.get();

    LOG_INFO("to-default {}", 1);
    EXTRA_LOG_INFO("to-extra {}", 2);
    EXTRA_LOG_DEBUG("extra-hidden"); // extra 也是 Info：级别各自独立
    mainLg->Flush();
    extraLg->Flush();
    g_default_logger.store(nullptr);
    g_extra = nullptr;

    const auto mainText = ReadAll(dir / "svc.log");
    const auto extraText = ReadAll(dir / "svc.res.log");
    CHECK(Has(mainText, "to-default 1"));
    CHECK(!Has(mainText, "to-extra"));
    CHECK(Has(extraText, "to-extra 2"));
    CHECK(!Has(extraText, "to-default"));
    CHECK(!Has(extraText, "extra-hidden"));
    fs::remove_all(dir);
}

void TestFallbackToStderrWhenNoDefault()
{
    CHECK(g_default_logger.load() == nullptr);
    CHECK(&Logger::Stderr() == &Logger::Stderr());

    const auto dir = TempDir("fallback");
    fs::create_directories(dir);
    {
        StderrCapture cap(dir / "err.txt");
        LOG_DEBUG("hidden-fallback"); // 兜底级别固定 Info
        LOG_INFO("fallback {}", 5);
        Logger::Stderr().Flush();
    }
    const auto text = ReadAll(dir / "err.txt");
    CHECK(Has(text, "fallback 5"));
    CHECK(!Has(text, "hidden-fallback"));
    CHECK(Has(text, "logger_test.cpp"));
    fs::remove_all(dir);
}

void TestDefaultLoggerUsedByMacro()
{
    const auto dir = TempDir("default");
    std::string err;
    auto lg = Logger::Create(Cfg(dir, false), err);
    CHECK(lg);

    g_default_logger.store(lg.get());
    LOG_INFO("via-default {}", 1);
    lg->Flush();
    g_default_logger.store(nullptr);

    CHECK(Has(ReadAll(dir / "svc.log"), "via-default 1"));
    LOG_INFO("after-reset"); // 撤销后回落到 stderr，不崩溃
    fs::remove_all(dir);
}

void TestFatalWithoutDefaultFallsBackToStderr()
{
    const auto dir = TempDir("fatal_fallback");
    fs::create_directories(dir);
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0)
    {
        const rlimit noCore{0, 0};
        ::setrlimit(RLIMIT_CORE, &noCore);
        const int fd = ::open((dir / "err.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ::dup2(fd, STDERR_FILENO);
        LOG_FATAL("boom-default {}", 3);
    }

    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    CHECK(Has(ReadAll(dir / "err.txt"), "boom-default 3"));
    fs::remove_all(dir);
}

// Shutdown：排空后对象仍有效，晚到的日志转 stderr，Flush 不抛，重复调用与析构都安全。
void TestShutdownDrainsAndRoutesLateLogsToStderr()
{
    for (const bool async : {true, false})
    {
        const auto dir = TempDir(async ? "shutdown_async" : "shutdown_sync");
        fs::create_directories(dir);
        std::string err;
        auto lg = Logger::Create(Cfg(dir / "logs", async), err);
        CHECK(lg);

        for (int i = 0; i < 500; ++i)
            BREEZE_LOG_INFO(lg, "line-{}", i);
        lg->Shutdown();
        // 不销毁 logger，文件里就该已经全了
        CHECK(Has(ReadAll(dir / "logs" / "svc.log"), "line-499"));

        {
            StderrCapture cap(dir / "err.txt");
            BREEZE_LOG_INFO(lg, "late-{}", 1); // 关闭后：转 stderr
            lg->Flush();                       // 关闭后：空操作，不抛
            lg->Shutdown();                    // 幂等
            Logger::Stderr().Flush();
        }
        const auto errText = ReadAll(dir / "err.txt");
        CHECK(Has(errText, "late-1"));
        CHECK(!Has(errText, "LOG ERROR"));
        CHECK(!Has(errText, "[logger]"));
        CHECK(!Has(ReadAll(dir / "logs" / "svc.log"), "late-1")); // 没写进已关闭的文件
        lg.reset();                                               // 析构在 Shutdown 之后，不重复处理
        fs::remove_all(dir);
    }
}

// flushLevel：>= 它的日志写完立即可见，不用等缓冲；运行时可改。
void TestFlushLevelConfigurable()
{
    const auto dir = TempDir("flushlevel");
    auto c = Cfg(dir, false);
    c.flushLevel = Level::Info;
    c.flushIntervalSec = 0;
    std::string err;
    auto lg = Logger::Create(c, err);
    CHECK(lg);
    CHECK(lg->GetFlushLevel() == Level::Info);

    BREEZE_LOG_INFO(lg, "visible-now"); // 没调 Flush
    CHECK(Has(ReadAll(dir / "svc.log"), "visible-now"));

    lg->SetFlushLevel(Level::Error);
    CHECK(lg->GetFlushLevel() == Level::Error);
    lg->SetFlushLevel(Level::Off);
    CHECK(lg->GetFlushLevel() == Level::Off);
    fs::remove_all(dir);
}

// 周期刷盘：不按级别刷、也不手动 Flush，缓冲里的 INFO 仍会在一个周期内出现在文件里。
void TestPeriodicFlush()
{
    const auto dir = TempDir("periodic");
    auto c = Cfg(dir, false);
    c.flushLevel = Level::Off;
    c.flushIntervalSec = 1;
    std::string err;
    auto lg = Logger::Create(c, err);
    CHECK(lg);

    BREEZE_LOG_INFO(lg, "buffered-line");
    bool seen = false;
    for (int i = 0; i < 40 && !seen; ++i) // 最多等 4 秒
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        seen = Has(ReadAll(dir / "svc.log"), "buffered-line");
    }
    CHECK(seen);
    fs::remove_all(dir);
}

// <name>.log 已存在时：普通文件改名 .old 保留、悬空软链被覆盖、目录则放弃软链但照常写日志。
void TestSymlinkDirtyStates()
{
    {
        const auto dir = TempDir("link_file");
        fs::create_directories(dir);
        std::ofstream(dir / "t.log") << "legacy-content";
        {
            StderrCapture cap(dir / "err.txt"); // 改名时会往 stderr 打一行说明
            HourlyFileSinkMt sink(dir, "t", 0);
            Feed(sink, At(2026, 10, 26, 12, 0), "new");
        }
        CHECK(fs::is_symlink(dir / "t.log"));
        CHECK(ReadAll(dir / "t.log.old") == "legacy-content");
        CHECK(Has(ReadAll(dir / "err.txt"), "已改名"));
        CHECK(Has(ReadAll(dir / "t.log"), "new"));
        fs::remove_all(dir);
    }
    {
        const auto dir = TempDir("link_dangling");
        fs::create_directories(dir);
        fs::create_symlink("no-such-file", dir / "t.log");
        {
            HourlyFileSinkMt sink(dir, "t", 0);
            Feed(sink, At(2026, 10, 26, 12, 0), "new");
        }
        CHECK(fs::read_symlink(dir / "t.log") == "t.2026-10-26_12.log");
        CHECK(!fs::exists(dir / "t.log.old"));
        fs::remove_all(dir);
    }
    {
        const auto dir = TempDir("link_dir");
        fs::create_directories(dir / "t.log");
        {
            StderrCapture cap(dir / "err.txt");
            HourlyFileSinkMt sink(dir, "t", 0);
            Feed(sink, At(2026, 10, 26, 12, 0), "new");
        }
        CHECK(fs::is_directory(dir / "t.log"));                  // 没被动
        CHECK(Has(ReadAll(dir / "t.2026-10-26_12.log"), "new")); // 日志照写
        CHECK(Has(ReadAll(dir / "err.txt"), "是目录"));
        fs::remove_all(dir);
    }
}

// 线程名：每个 logger 的后台线程带上名字，top -H / gdb 里能认出来。线程名在线程启动后才设，要轮询。
std::set<std::string> ThreadNames()
{
    std::set<std::string> names;
    for (const auto& e : fs::directory_iterator("/proc/self/task"))
    {
        std::string n = ReadAll(e.path() / "comm");
        if (!n.empty() && n.back() == '\n')
            n.pop_back();
        names.insert(n);
    }
    return names;
}

void TestThreadNames()
{
    const auto dir = TempDir("threadnames");
    auto c = Cfg(dir, true);
    c.flushIntervalSec = 1;
    std::string err;
    auto lg = Logger::Create(c, err);
    CHECK(lg);

    bool pool = false, flusher = false;
    for (int i = 0; i < 50 && !(pool && flusher); ++i) // 刷盘线程第一次回调才命名，最多等 5 秒
    {
        const auto names = ThreadNames();
        pool = names.contains("log:svc");
        flusher = names.contains("logf:svc");
        if (!(pool && flusher))
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(pool);
    CHECK(flusher);

    lg.reset();
    CHECK(!ThreadNames().contains("log:svc")); // 销毁后线程都没了
    CHECK(!ThreadNames().contains("logf:svc"));
    fs::remove_all(dir);
}

// 带显式 file/line 的重载：转接第三方库日志用。
void TestLogWithExplicitLocation()
{
    const auto dir = TempDir("explicit_loc");
    std::string err;
    auto lg = Logger::Create(Cfg(dir, false), err);
    CHECK(lg);
    lg->Log(Level::Info, "/some/dir/fake_lib.cc", 77, "Func", "from-third-party");
    lg->Flush();
    const auto text = ReadAll(dir / "svc.log");
    CHECK(Has(text, "from-third-party"));
    CHECK(Has(text, "fake_lib.cc:77")); // %s 取文件名，%# 取行号
    fs::remove_all(dir);
}

void TestFatalInStdoutMode()
{
    const auto dir = TempDir("fatal_stdout");
    fs::create_directories(dir);
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0)
    {
        const rlimit noCore{0, 0};
        ::setrlimit(RLIMIT_CORE, &noCore);
        const int fd = ::open((dir / "out.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ::dup2(fd, STDOUT_FILENO);
        std::string err;
        auto lg = Logger::Create(StdoutCfg(dir, true), err);
        if (!lg)
            ::_exit(2);
        for (int i = 0; i < 100; ++i)
            BREEZE_LOG_INFO(lg, "ctx-{}", i);
        BREEZE_LOG_FATAL(lg, "boom {}", 9);
    }

    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    const auto text = ReadAll(dir / "out.txt");
    CHECK(Has(text, "ctx-0"));
    CHECK(Has(text, "ctx-99"));
    CHECK(Has(text, "boom 9"));
    fs::remove_all(dir);
}

void TestFatalFlushesQueueThenAborts()
{
    const auto dir = TempDir("fatal");
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0)
    {
        const rlimit noCore{0, 0};
        ::setrlimit(RLIMIT_CORE, &noCore);
        std::string err;
        auto lg = Logger::Create(Cfg(dir, true), err);
        if (!lg)
            ::_exit(2);
        for (int i = 0; i < 200; ++i)
            BREEZE_LOG_INFO(lg, "ctx-{}", i);
        BREEZE_LOG_FATAL(lg, "boom {}", 7);
    }

    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

    const auto text = ReadAll(dir / "svc.log");
    CHECK(Has(text, "ctx-0"));
    CHECK(Has(text, "ctx-199"));
    CHECK(Has(text, "boom 7"));
    fs::remove_all(dir);
}

} // namespace

int main()
{
    // fork 用例要在没有任何后台线程时进行，放最前。
    TestFatalFlushesQueueThenAborts();
    TestFatalInStdoutMode();
    TestFatalWithoutDefaultFallsBackToStderr();

    TestRotateByHourAndSymlink();
    TestClockGoingBackwardsReopens();
    TestRetentionOnlyTouchesOwnOldFiles();
    TestDottedNamesDontTouchEachOther();
    TestLevelFilterAndRuntimeSetLevel();
    TestArgsNotEvaluatedWhenFiltered();
    TestAsyncDrainsOnDestroy();
    TestDefaultLevelFollowsBuildType();
    TestLevelNamesRoundTrip();
    TestSetLogLevelOnDefaultLogger();
    TestShutdownDrainsAndRoutesLateLogsToStderr();
    TestFallbackToStderrWhenNoDefault();
    TestBusinessMacroFamilyOnTopOfInnerMacros();
    TestDefaultLoggerUsedByMacro();
    TestFlushLevelConfigurable();
    TestPeriodicFlush();
    TestSymlinkDirtyStates();
    TestThreadNames();
    TestLogWithExplicitLocation();
    TestStdoutModeWritesNoFiles();
    TestStdoutAsyncDrainsOnDestroy();
    TestStdoutModeIgnoresUnwritableDir();
    TestCreateFailureReportsError();
    // 兜底实例自己被关掉：只能丢弃，不能无限递归。放最后，因为它会让后面的兜底输出消失。
    Logger::Stderr().Shutdown();
    LOG_INFO("dropped-after-fallback-closed");

    std::puts("logger_test: ok");
}
