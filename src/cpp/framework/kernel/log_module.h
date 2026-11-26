// 日志模块：Init 时登记默认 logger，Shutdown 时撤销并排空，析构兜底。
//
// **必须第一个注册**。depsSort 每步取注册序号最小的就绪节点，LogModule 无依赖且序号 0，
// 于是第一个 Init、最后一个 Shutdown —— 其它模块收尾时文件日志仍可写。
// 这靠注册顺序隐式保证，由 log_module_test.cpp 的 TestRegisteredFirstInitsFirst 钉住。
//
// 在它 Init 之前（工厂、构造函数、Build 的启动摘要）打的日志走 Logger::Stderr()，
// 不会进文件 —— 启动脚本应把 stderr 重定向下来。
//
// 模块的线程须在 Stop 中退出，避免日志 Shutdown 后继续写入。

#pragma once

#include <memory>
#include <string_view>

#include "base/logger/logger.h"
#include "framework/app/module.h"

namespace breeze
{

class LogModule final : public Module
{
public:
    LogModule(App& app, LoggerConfig cfg)
        : Module(app)
        , cfg_(std::move(cfg))
    {
    }
    ~LogModule() override;

    std::string_view Name() const override { return "log"; }
    std::expected<void, Error> Init() override;
    std::expected<void, Error> Start() override { return {}; }
    void Shutdown() override;

private:
    LoggerConfig cfg_;
    std::unique_ptr<Logger> logger_;
};

} // namespace breeze
