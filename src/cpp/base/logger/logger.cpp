#include "logger.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>

#include "logger_impl.h"

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

spdlog::source_loc ToSpdlog(const std::source_location& loc)
{
    return {loc.file_name(), static_cast<int>(loc.line()), loc.function_name()};
}

} // namespace

Logger::Logger(std::unique_ptr<Impl> impl, Level level)
    : impl_(std::move(impl))
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
            std::filesystem::create_directories(cfg.dir, ec);
            if (ec)
            {
                err = std::format("创建日志目录 {} 失败: {}", cfg.dir, ec.message());
                return nullptr;
            }
            auto fileSink = std::make_shared<HourlyFileSinkMt>(cfg.dir, cfg.name, cfg.retainHours);
            fileSink->Open(spdlog::log_clock::now());
            impl->sinks.push_back(fileSink);
        }

        if (cfg.async)
        {
            impl->tp = std::make_shared<spdlog::details::thread_pool>(cfg.queueSize, cfg.threads);
            const auto policy = cfg.overflow == Overflow::Block ? spdlog::async_overflow_policy::block
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
        impl->logger->flush_on(spdlog::level::warn);
        impl->logger->set_error_handler([](const std::string& msg)
                                        { std::fprintf(stderr, "[logger] %s\n", msg.c_str()); });

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
        cfg.async = false;
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
    impl_->logger->flush();
    impl_->tp.reset(); // 线程池析构会先处理完队列里的消息再 join
}

void Logger::Log(Level level, std::source_location loc, std::string_view msg)
{
    if (!ShouldLog(level))
        return;
    if (closed_.load(std::memory_order_acquire))
    {
        // 兜底实例自己被关了就只能丢弃，不能再转给自己。
        Logger& fallback = Stderr();
        if (&fallback != this)
            fallback.Log(level, loc, msg);
        return;
    }
    impl_->logger->log(ToSpdlog(loc), ToSpdlog(level), spdlog::string_view_t(msg.data(), msg.size()));
}

// 先 Shutdown，把排在前面的日志（出事前的上下文）写完，再绕过队列直写这一条。
// 异步 logger 的 flush() 只投递请求不等待，abort 紧跟其后会丢掉最后一条。
// 此后别的线程再 Log 会转去 stderr（已关闭），进程马上就没了。
void Logger::Fatal(std::source_location loc, std::string_view msg)
{
    Shutdown();

    spdlog::logger direct(impl_->name, impl_->sinks.begin(), impl_->sinks.end());
    direct.log(ToSpdlog(loc), spdlog::level::critical, spdlog::string_view_t(msg.data(), msg.size()));
    direct.flush();
    std::abort();
}

void Logger::Flush()
{
    if (closed_.load(std::memory_order_acquire))
        return; // 异步时线程池已不在，flush() 会抛
    impl_->logger->flush();
}

} // namespace breeze
