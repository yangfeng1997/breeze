#include "base/net/agent/agent_server.h"
#include <cerrno>

#include "base/logger/logger.h"
#include "base/logger/logger_macros.h"
#include "base/net/loop/group_access.h"

namespace breeze::agent
{

AgentServer::AgentServer(EventLoopGroup& group, const Endpoint& endpoint, RouteTable& table, AgentServerOptions options)
    : group_(group)
    , default_codec_(options.max_frame)
    , codec_(options.codec != nullptr ? options.codec : &default_codec_)
    , connection_(options.connection)
    , router_(table, std::move(options.router))
    , server_(group, endpoint, std::move(options.shm), router_, ShmUdsDataPath{&router_, codec_, connection_})
{
}

AgentServer::~AgentServer() = default;

int AgentServer::Start()
{
    // 路由器与路由表是单线程访问的：group 里有第二个 loop，Agent 之间的链路就可能落在别的线程上回调它。
    if (GroupAccess::TryLoopAt(group_, 1) != nullptr)
    {
        LOG_ERROR("AgentServer 要求 EventLoopGroup 只有一个 loop（io_threads = 1）：路由器是单线程的");
        return EINVAL;
    }
    return server_.Start();
}

} // namespace breeze::agent
