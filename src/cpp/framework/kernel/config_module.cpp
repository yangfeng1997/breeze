#include "config_module.h"

#include <format>

#include "logger_macros.h"

namespace breeze
{

Result<> ConfigModule::Init()
{
    auto cfg = Config::Load({path_});
    if (!cfg)
        return Fail(Errc::Module, std::format("加载配置 {} 失败: {}", path_, cfg.error().Format()));
    auto common = cfg->Get<config::CommonConfig>("");
    if (!common)
        return Fail(Errc::Module, std::format("读取公共配置失败: {}", common.error().Format()));

    cfg_.emplace(std::move(*cfg));
    common_ = std::move(*common);
    // ConfigDump 里密钥是 ***，可以放心打。
    LOG_INFO("config ready file={}\n{}", path_, ConfigDump(common_));
    return {};
}

} // namespace breeze
