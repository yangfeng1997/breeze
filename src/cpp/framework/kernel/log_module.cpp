#include "framework/kernel/log_module.h"

#include "base/logger/logger_macros.h"

namespace breeze
{

Result<> LogModule::Init()
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

LogModule::~LogModule()
{
    if (!logger_)
        return;
    Logger* mine = logger_.get();
    g_default_logger.compare_exchange_strong(mine, nullptr, std::memory_order_acq_rel);
    // logger_ 随后析构，~Logger 会 Shutdown 排空队列。
    // 等 Module 有了 Shutdown 阶段，这里改成：先撤销，再显式 logger_->Shutdown()。
}

} // namespace breeze
