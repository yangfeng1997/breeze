// 日志：spdlog 的薄封装。
//
// 默认 logger 是进程级的 g_default_logger，由 LogModule::Init 登记、析构时撤销；日志宏（LOG_INFO 等）
// 每次调用时读它，为空则落到 Logger::Stderr()，所以任何阶段（Build、Init 之前、~App 之后）
// 打日志都不会崩，也不会丢。要指定别的 logger 用 BREEZE_LOG_<LEVEL>(logger, ...)。
//
// spdlog 只出现在 .cpp 与 internal/，本头不含它。日志宏见 logger_macros.h。
//
// 输出目标二选一（Output），不会同时写两处：
//   File   按小时滚动的文件 <name>.<YYYY-MM-DD_HH>.log，<name>.log 是始终指向当前文件的软链。
//          进程退出不删软链，便于事后直接看最后一个文件。
//          启动期、崩溃信息不经 Logger，由启动脚本重定向，如 `>> x.stdout.log 2>> x.stderr.log`。
//   Stdout 只写 stdout，不建目录、不建文件。容器里由运行时收集并轮转。
//   Stderr 只写 stderr，同上。Logger::Stderr() 的兜底实例用它。

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>

namespace breeze
{

enum class Level : std::uint8_t
{
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
    Off, // 只用于 SetLevel，表示全部关闭（Fatal 宏除外，见 Logger::Fatal）
};

// 按构建类型的默认运行时级别：Release（NDEBUG）Info，其余 Debug。
// 定义在 logger.cpp 里：由库按自己的构建类型决定，不随使用方翻译单元的宏而变。
Level DefaultLevel();

// 小写名字：trace debug info warn error fatal off。供配置文件、管理命令把字符串转成级别。
std::string_view ToString(Level level);
// 大小写不敏感，另接受 warning；认不出返回 nullopt。
std::optional<Level> ParseLevel(std::string_view name);

// 异步队列满时的取舍。spdlog 无法按级别区分，DropOldest 可能恰好丢掉要看的 ERROR；
// Block 则会在磁盘变慢时拖住调用线程（包括主循环）。
enum class Overflow : std::uint8_t
{
    Block,
    DropOldest,
};

// 输出目标。Stdout / Stderr 下 dir / retainHours 无效，也不会创建任何文件。
enum class Output : std::uint8_t
{
    File,
    Stdout,
    Stderr,
};

struct LoggerConfig
{
    Output output = Output::File;
    std::string dir = "logs"; // 仅 File
    // File 下是文件名前缀；同时是 pattern 里 %n。同一 dir 下必须唯一，否则会写同一个文件、抢同一条软链。
    // 可含点号表示类别，如 gatesvr / gatesvr.res / gatesvr.tracing：旧文件清理只认
    // <name>.<YYYY-MM-DD_HH>.log，不会碰同前缀的其它 logger（见 logger_test 的 TestDottedNames…）。
    std::string name = "app";
    Level level = DefaultLevel();

    // false 时在调用线程里同步写，queueSize / threads / overflow 无效。
    bool async = true;
    std::uint32_t queueSize = 16384;
    std::uint32_t threads = 1;
    Overflow overflow = Overflow::Block;

    // 写完 >= 此级别的日志后立即刷盘（异步时投递刷盘请求）。Off 表示不按级别刷。
    Level flushLevel = Level::Warn;
    // 周期刷盘间隔（秒），0 关闭。否则低流量时 INFO 会一直留在缓冲里，进程崩溃后看不到。
    // 每个开启的 logger 多一个线程（名字 logf:<name>）。
    std::uint32_t flushIntervalSec = 1;

    // 仅 File。每次滚动时删除早于此小时数的文件，0 不清理。只认 <name>.<YYYY-MM-DD_HH>.log，不动其它文件。
    std::uint32_t retainHours = 168;

    std::string pattern = "[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] [%s:%#] %v";
};

class Logger
{
public:
    // 失败返回 nullptr 并填 err（目录建不出、文件打不开等）。File 下成功时当前小时的文件与软链已就绪。
    static std::unique_ptr<Logger> Create(const LoggerConfig& cfg, std::string& err);

    // 没有默认 logger 时的兜底：同步写 stderr，级别 Info。进程内共享的唯一实例，**故意不销毁**，
    // 这样进程退出、其它静态对象析构时仍可用。
    static Logger& Stderr();

    // 兜底调用 Shutdown()，再释放。
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    bool ShouldLog(Level level) const noexcept
    {
        return static_cast<std::uint8_t>(level) >= level_.load(std::memory_order_relaxed);
    }

    // 级别不够直接返回。msg 已格式化完毕，不再解释 `{}`。
    void Log(Level level, std::source_location loc, std::string_view msg);

    // 位置由调用方给：转接第三方库日志（它们自带 file/line）时用，std::source_location 没法手工构造。
    // file / func 必须是 NUL 结尾且在调用期间有效。
    void Log(Level level, const char* file, int line, const char* func, std::string_view msg);

    // 写入、排空队列、abort。不受 level 过滤影响，Fatal 一定出得去。
    [[noreturn]] void Fatal(std::source_location loc, std::string_view msg);

    // 运行时可调，立即对所有线程生效。注意编译期剥离在前：Release 下 TRACE 宏已不存在，
    // 运行时设成 Trace 也出不来 TRACE（见 logger_macros.h 的 BREEZE_LOG_ACTIVE_LEVEL）。
    void SetLevel(Level level) noexcept { level_.store(static_cast<std::uint8_t>(level), std::memory_order_relaxed); }
    Level GetLevel() const noexcept { return static_cast<Level>(level_.load(std::memory_order_relaxed)); }

    // 运行时可调：写完 >= level 的日志立即刷盘。
    void SetFlushLevel(Level level);
    Level GetFlushLevel() const;

    // 异步时只是投递刷盘请求，不等落盘；要保证落盘用 Shutdown、析构或 Fatal。关闭后是空操作。
    void Flush();

    // 排空并关闭：队列里已有的日志全部写完（异步时 join 后台线程）才返回。幂等。
    // **对象本身仍然有效**：之后再来的 Log() 一律转去 Logger::Stderr()，不丢、不抛、不崩 ——
    // 这样晚到的调用（刚读到指针的线程）不会碰到已释放的内存，不必靠"线程都已停掉"的约定。
    // 真正释放留给析构。唯一的窄窗口：恰好在 Shutdown 期间已进入 Log() 的异步调用，
    // 会撞上已销毁的线程池，由 error handler 往 stderr 打一行，这条日志丢失。
    void Shutdown();

private:
    struct Impl;
    Logger(std::unique_ptr<Impl> impl, Level level);

    std::unique_ptr<Impl> impl_;
    std::atomic<std::uint8_t> level_;
    std::atomic<bool> closed_{false};
};

// 默认 logger。**只许 LogModule 写**：Init 登记，析构时用 compare_exchange 撤销（只撤销自己的）。
// 用原子是因为框架不强制"Init 先于任何线程"，违反时不该是静默的未定义行为；x86 上读取无额外开销。
inline std::atomic<Logger*> g_default_logger{nullptr};

inline Logger& CurrentLogger()
{
    Logger* l = g_default_logger.load(std::memory_order_acquire);
    return l ? *l : Logger::Stderr();
}

// 改 / 读默认 logger 的运行时级别。没有默认 logger 时作用在 stderr 兜底实例上。
inline void SetLogLevel(Level level) noexcept { CurrentLogger().SetLevel(level); }
inline Level GetLogLevel() noexcept { return CurrentLogger().GetLevel(); }

namespace detail
{
// 让 BREEZE_LOG_<LEVEL> 同时接受对象、裸指针与智能指针。
inline Logger& Ref(Logger& l) { return l; }
inline Logger& Ref(Logger* l) { return *l; }
inline Logger& Ref(const std::unique_ptr<Logger>& l) { return *l; }
inline Logger& Ref(const std::shared_ptr<Logger>& l) { return *l; }
} // namespace detail

} // namespace breeze
