// liburing 类型不出现在公共配置或连接头中。
#pragma once
#include <memory>

#include "base/net/loop/io_engine.h"

namespace breeze
{
std::unique_ptr<IoEngine> MakeUringEngine(UringReceiveMode mode);
}
