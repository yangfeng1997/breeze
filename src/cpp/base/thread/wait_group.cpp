#include "base/thread/wait_group.h"

#include <cstdlib>

#include "base/logger/logger.h"
#include "base/logger/logger_macros.h"

namespace breeze
{

void WaitGroup::FailNegative(int count, int delta)
{
    LOG_FATAL("WaitGroup 计数降到了负数 count={} delta={}：Done 多于 Add", count, delta);
    std::abort(); // LOG_FATAL 本身不返回；这里让编译器也知道
}

} // namespace breeze
