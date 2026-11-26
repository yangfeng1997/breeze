#include "framework/kernel/log_module.h"

#include "base/logger/logger_macros.h"

namespace breeze
{

std::expected<void, Error> LogModule::Init()
{
    std::string err;
    logger_ = Logger::Create(cfg_, err);
    if (!logger_)
        return Fail(Errc::Module, std::move(err));

    // 直接覆盖：同进程里先后存在两个 App（测试）时，后者生效；撤销时只认自己。
    g_default_logger.store(logger_.get(), std::memory_order_release);
    LOG_INFO("log ready name={} output={}", cfg_.name, static_cast<int>(cfg_.output));
    return {};
}

LogModule::~LogModule() { Shutdown(); }

void LogModule::Shutdown()
{
    if (!logger_)
        return;
    Logger* mine = logger_.get();
    g_default_logger.compare_exchange_strong(mine, nullptr, std::memory_order_acq_rel);
    logger_->Shutdown();
}

} // namespace breeze
