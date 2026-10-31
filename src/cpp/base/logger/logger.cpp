#include "base/logger/logger.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "base/logger/internal/logger_impl.h"

namespace breeze
{

namespace
{

spdlog::level::level_enum ToSpdlog(Level l)
{
    switch (l)
    {
        case Level::Trace:
            return spdlog::level::trace;
        case Level::Debug:
            return spdlog::level::debug;
        case Level::Info:
            return spdlog::level::info;
        case Level::Warn:
            return spdlog::level::warn;
        case Level::Error:
            return spdlog::level::err;
        case Level::Fatal:
            return spdlog::level::critical;
        case Level::Off:
            return spdlog::level::off;
    }
    return spdlog::level::info;
}

Level FromSpdlog(spdlog::level::level_enum l)
{
    switch (l)
    {
        case spdlog::level::trace:
            return Level::Trace;
        case spdlog::level::debug:
            return Level::Debug;
        case spdlog::level::info:
            return Level::Info;
        case spdlog::level::warn:
            return Level::Warn;
        case spdlog::level::err:
            return Level::Error;
        case spdlog::level::critical:
            return Level::Fatal;
        default:
            return Level::Off;
    }
}

// top -H / gdb 里一眼分清哪个线程属于哪个 logger。Linux 上线程名最长 15 字符，超出截断。
void SetThreadName([[maybe_unused]] const std::string& name)
{
#ifdef __linux__
    ::prctl(PR_SET_NAME, name.substr(0, 15).c_str(), 0, 0, 0);
#endif
}

} // namespace

Level DefaultLevel()
{
#ifdef NDEBUG
    return Level::Info;
#else
    return Level::Debug;
#endif
}

std::string_view ToString(Level level)
{
    switch (level)
    {
        case Level::Trace:
            return "trace";
        case Level::Debug:
            return "debug";
        case Level::Info:
            return "info";
        case Level::Warn:
            return "warn";
        case Level::Error:
            return "error";
        case Level::Fatal:
            return "fatal";
        case Level::Off:
            return "off";
    }
    return "unknown";
}

std::optional<Level> ParseLevel(std::string_view name)
{
    std::string lower(name);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "warning")
        lower = "warn";
    for (const Level l : {Level::Trace, Level::Debug, Level::Info, Level::Warn, Level::Error, Level::Fatal, Level::Off})
        if (lower == ToString(l))
            return l;
    return std::nullopt;
}

Logger::Logger(std::unique_ptr<Impl> impl, Level level)
    : impl_(std::move(impl))
    , initialLevel_(static_cast<std::uint8_t>(level))
    , level_(static_cast<std::uint8_t>(level))
{
}

std::unique_ptr<Logger> Logger::Create(const LoggerConfig& cfg, std::string& err)
{
    try
    {
        auto impl = std::make_unique<Impl>();
        impl->name = cfg.name;

        if (cfg.output == Output::Stdout)
        {
            // 不用 color sink：采集系统里 ANSI 转义会变成乱码。
            impl->sinks.push_back(std::make_shared<spdlog::sinks::stdout_sink_mt>());
        }
        else if (cfg.output == Output::Stderr)
        {
            impl->sinks.push_back(std::make_shared<spdlog::sinks::stderr_sink_mt>());
        }
        else
        {
            std::error_code ec;
            std::filesystem::create_directories(cfg.file.dir, ec);
            if (ec)
            {
                err = std::format("创建日志目录 {} 失败: {}", cfg.file.dir, ec.message());
                return nullptr;
            }
            auto fileSink = std::make_shared<HourlyFileSinkMt>(
                cfg.file.dir, cfg.name, cfg.file.retainHours, cfg.file.maxFileBytes, cfg.file.maxFilesPerHour);
            fileSink->Open(spdlog::log_clock::now());
            impl->sinks.push_back(fileSink);
        }

        if (cfg.async.enabled)
        {
            impl->tp = std::make_shared<spdlog::details::thread_pool>(
                cfg.async.queueSize, cfg.async.threads, [tname = "log:" + cfg.name] { SetThreadName(tname); });
            const auto policy = cfg.async.overflow == Overflow::Block ? spdlog::async_overflow_policy::block
                                                                      : spdlog::async_overflow_policy::overrun_oldest;
            impl->logger = std::make_shared<spdlog::async_logger>(
                cfg.name, impl->sinks.begin(), impl->sinks.end(), impl->tp, policy);
        }
        else
        {
            impl->logger = std::make_shared<spdlog::logger>(cfg.name, impl->sinks.begin(), impl->sinks.end());
        }

        impl->logger->set_pattern(cfg.pattern);
        // 过滤只在 Logger::ShouldLog 做；sink 与 spdlog 自己的级别永远放开，
        // 否则运行时 SetLevel 调低后，被创建时级别挡掉的 sink 仍然出不来。
        impl->logger->set_level(spdlog::level::trace);
        impl->logger->flush_on(ToSpdlog(cfg.flush.level));
        impl->logger->set_error_handler([](const std::string& msg)
                                        { std::fprintf(stderr, "[logger] %s\n", msg.c_str()); });

        if (cfg.flush.intervalSec > 0)
        {
            // 线程名要等第一次回调才设得上（periodic_worker 没有线程启动钩子），所以最迟一个周期后才可见。
            impl->flusher = std::make_unique<spdlog::details::periodic_worker>(
                [raw = impl->logger.get(), tname = "logf:" + cfg.name]
                {
                    thread_local bool named = false;
                    if (!named)
                    {
                        SetThreadName(tname);
                        named = true;
                    }
                    raw->flush();
                },
                std::chrono::seconds(cfg.flush.intervalSec));
        }

        return std::unique_ptr<Logger>(new Logger(std::move(impl), cfg.level));
    }
    catch (const std::exception& e)
    {
        err = e.what();
        return nullptr;
    }
}

Logger& Logger::Stderr()
{
    static Logger* const fallback = []
    {
        LoggerConfig cfg;
        cfg.output = Output::Stderr;
        cfg.name = "stderr";
        cfg.level = Level::Info;
        cfg.async.enabled = false;
        cfg.flush.intervalSec = 0; // 同步写 stderr，无缓冲，不需要刷盘线程
        std::string err;
        auto lg = Create(cfg, err);
        if (!lg) // stderr sink 不会失败；真失败了没有任何地方可以报，只能终止
            std::abort();
        return lg.release();
    }();
    return *fallback;
}

Logger::~Logger()
{
    if (!impl_)
        return;
    Shutdown();
}

void Logger::Shutdown()
{
    if (closed_.exchange(true, std::memory_order_acq_rel))
        return;
    impl_->flusher.reset(); // 先停周期刷盘线程，再动线程池
    impl_->logger->flush();
    impl_->tp.reset(); // 线程池析构会先处理完队列里的消息再 join
}

void Logger::Log(Level level, std::source_location loc, std::string_view msg)
{
    Log(level, loc.file_name(), static_cast<int>(loc.line()), loc.function_name(), msg);
}

void Logger::Log(Level level, const char* file, int line, const char* func, std::string_view msg)
{
    if (!ShouldLog(level))
        return;
    if (closed_.load(std::memory_order_acquire))
    {
        // 兜底实例自己被关了就只能丢弃，不能再转给自己。
        Logger& fallback = Stderr();
        if (&fallback != this)
            fallback.Log(level, file, line, func, msg);
        return;
    }
    impl_->logger->log(
        spdlog::source_loc{file, line, func}, ToSpdlog(level), spdlog::string_view_t(msg.data(), msg.size()));
}

// 先 Shutdown，把排在前面的日志（出事前的上下文）写完，再绕过队列直写这一条。
// 异步 logger 的 flush() 只投递请求不等待，abort 紧跟其后会丢掉最后一条。
// 此后别的线程再 Log 会转去 stderr（已关闭），进程马上就没了。
void Logger::Fatal(std::source_location loc, std::string_view msg)
{
    Shutdown();

    spdlog::logger direct(impl_->name, impl_->sinks.begin(), impl_->sinks.end());
    direct.log(spdlog::source_loc{loc.file_name(), static_cast<int>(loc.line()), loc.function_name()},
               spdlog::level::critical,
               spdlog::string_view_t(msg.data(), msg.size()));
    direct.flush();
    std::abort();
}

void Logger::SetFlushLevel(Level level) { impl_->logger->flush_on(ToSpdlog(level)); }

Level Logger::GetFlushLevel() const { return FromSpdlog(impl_->logger->flush_level()); }

void Logger::Flush()
{
    if (closed_.load(std::memory_order_acquire))
        return; // 异步时线程池已不在，flush() 会抛
    impl_->logger->flush();
}

} // namespace breeze
