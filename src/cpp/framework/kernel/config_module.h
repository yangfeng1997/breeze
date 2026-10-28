// 配置模块：Init 时加载 --config 指定的**那一个**文件（通常是 common.yaml），不会再找别的文件。
//
// 不隐式加载 env/*.yaml、<svr>.yaml 之类：路径全来自命令行。环境差异要在传进来的文件里
// 已经解决（common.yaml 自带本机开发默认值，直接就能用；test / prod 传部署时合并好的那一份）。
// 业务自己的配置文件由业务自己 Config::Load（见 config_module_test.cpp 的 TestBusinessLoadsOwnFile）。
//
// 注册在 LogModule 之后：加载失败时日志已就绪。用到配置的模块应 DependsOn("config")。
// Raw() / Common() 只在 Init 之后有效。

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "args_parser.h"
#include "config.h"
#include "config/common_config.h"
#include "module.h"

namespace breeze
{

// 所有进程统一的 --config 参数（必填），main 解析命令行时调用。
inline void AddConfigArg(ArgsParser& args, std::string& path)
{
    args.Add("-c,--config", path, "配置文件路径（common.yaml）", kRequired);
}

class ConfigModule final : public Module
{
public:
    ConfigModule(App& app, std::string path)
        : Module(app)
        , path_(std::move(path))
    {
    }

    std::string_view Name() const override { return "config"; }
    Result<> Init() override;
    Result<> Start() override { return {}; }

    // 整份 yaml（已展开 ${ENV:}）。业务可以 Get<T>("某段") 读 common.yaml 里自己关心的段。
    Config& Raw() { return *cfg_; }
    // 框架自己用的公共配置，Init 时已读出并校验。
    const config::CommonConfig& Common() const { return common_; }

private:
    std::string path_;
    std::optional<Config> cfg_;
    config::CommonConfig common_;
};

} // namespace breeze
