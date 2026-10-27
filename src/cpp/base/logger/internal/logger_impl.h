// Logger::Impl 的定义，含 spdlog 头文件。
//
// **只许 base/logger 内部包含**：CMake 里本目录是 PRIVATE 头文件路径，使用方 include 不到。

#pragma once

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <vector>

#include "hourly_file_sink.h"
#include "logger.h"

namespace breeze
{

struct Logger::Impl
{
    // 销毁线程池会先处理完队列再 join，所以 ~Logger / Fatal 都**显式** reset 它来排空日志。
    std::shared_ptr<spdlog::details::thread_pool> tp; // 同步模式为空
    std::vector<spdlog::sink_ptr> sinks;
    std::shared_ptr<spdlog::logger> logger;
    std::string name;
};

} // namespace breeze
