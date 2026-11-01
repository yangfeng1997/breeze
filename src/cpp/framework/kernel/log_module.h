// 日志模块：Init 时按配置创建 Logger 并登记为默认 logger（g_default_logger），析构时撤销。
//
// **必须第一个注册**。depsSort 每步取注册序号最小的就绪节点，LogModule 无依赖且序号 0，
// 于是总是第一个 Init、最后一个析构 —— 其它模块 Init 时 g_default_logger 已就绪，析构时日志仍可写。
// 这靠注册顺序隐式保证，由 log_module_test.cpp 的 TestRegisteredFirstInitsFirst 钉住。
//
// 在它 Init 之前（工厂、构造函数、Build 的启动摘要）打的日志走 Logger::Stderr()，
// 不会进文件 —— 启动脚本应把 stderr 重定向下来。
//
// 约束：模块起的线程必须在自己的析构里停掉（App 对 jthread 的既有要求），
// 否则 ~LogModule 销毁 Logger 时它们还在写。

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
    Result<> Init() override;
    Result<> Start() override { return {}; }

private:
    LoggerConfig cfg_;
    std::unique_ptr<Logger> logger_;
};

} // namespace breeze
