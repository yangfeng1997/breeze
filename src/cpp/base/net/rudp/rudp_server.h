// rudp 服务端：一个 UDP 端口，客户端凭准入票据握手后得到一条可靠有序的消息通道（KCP）。
// 设计与威胁模型见 docs/网络库设计.md §4.14。
//
// 与 TcpServer 的区别：
// - 没有 codec / FrameCodecFactory：消息自带边界，handler 每次收到完整的一条。
// - 准入靠票据：控制面（login / gate）经可信通道把 RUdpTicket 发给客户端，服务端用 IssueTicket / AddTicket 登记；
//   客户端握手时出示，一次有效。没登记的来源发什么都建不起会话。
// - 一个 socket、一个 loop：所有会话都在 Start 时选定的那个 loop 上（要横向扩展就多开几个服务端、监听不同端口）。
// - 会话按来源 ip:port 固定，不支持迁移（客户端换网络要重新握手，拿新票据）。
// - 没有加密与完整性保护（v1）：票据防的是"谁能建会话"，不防在路径上的人读 / 改 / 注入数据报。
//
// 生命周期、Start / Stop 的约定同 StreamServer（见 stream/stream_server.h）：
// handler 只持引用，须比 EventLoopGroup::Shutdown 活得久；对象自己也须覆盖到 Shutdown 返回之后；
// 析构时若 group 仍在运行，会先同步关掉所有会话、等它们回收完（保险，不是用法）。

#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>

#include "connection_handler.h"
#include "connection_options.h"
#include "event_loop_group.h"
#include "net_server.h"
#include "rudp_endpoint.h"
#include "rudp_options.h"
#include "rudp_ticket.h"

namespace breeze
{

struct RUdpServerOptions
{
    ConnectionOptions
        connection; // 水位、idle_timeout：对每条会话生效。客户端默认每秒一个心跳，idle_timeout 设成几倍间隔
    RUdpOptions rudp;
    // 空 = 全部接受。在 Hello 到达的 IO 线程上、校验票据之前运行；PeerInfo::endpoint 是对端的 udp://ip:port，
    // 没有 credentials。拒绝的计入 rejected_filter，对端收到 Reject。
    AcceptFilter accept_filter;
    // 已登记但还没被使用的票据上限，防控制面出 bug 时把内存吃光；满了 IssueTicket 返回空。
    std::size_t max_pending_tickets = 65536;
};

class RUdpServer final : public NetServer
{
public:
    RUdpServer(EventLoopGroup& group,
               const RUdpEndpoint& endpoint,
               RUdpServerOptions options,
               ConnectionHandler& handler);
    ~RUdpServer() override;

    RUdpServer(const RUdpServer&) = delete;
    RUdpServer& operator=(const RUdpServer&) = delete;

    // group 必须已 Start（否则是编程错误，LOG_FATAL）；只能调用一次。阻塞到 bind 在 IO 线程上完成。
    // 0 = 成功，否则 errno（EADDRINUSE、EACCES……），失败时已回滚；选项不合法（RUdpOptions::Valid 为假）EINVAL；
    // group 正在 / 已经停机 ESHUTDOWN。
    [[nodiscard]] int Start() override;

    // 停止接受新会话（之后的 Hello 收到 Reject），已建立的会话继续服务，由 EventLoopGroup::Shutdown 收尾。
    // 最后一个会话结束后端口随之释放。异步；对没 Start 过的对象是空操作。
    void Stop() override;

    // 实际绑定的地址：端口填 0 时 Start 成功之后这里是系统分配的端口。Start 之前就是构造时给的端点。
    const Endpoint& endpoint() const noexcept override { return endpoint_; }
    RUdpEndpoint local_endpoint() const;

    // ---- 准入票据，任意线程可调（Start 之前也行），不要求 group 在运行 ----

    // 生成并登记一张票据（随机 conv 与 token），ttl 内凭它握手一次有效。ttl ≤ 0、或待用票据已满返回空。
    [[nodiscard]] std::optional<RUdpTicket> IssueTicket(std::chrono::milliseconds ttl);
    // 登记别处（比如 login 进程）签发的票据。conv 为 0、ttl ≤ 0、conv 已被未过期的占用、待用票据已满返回 false。
    [[nodiscard]] bool AddTicket(const RUdpTicket& ticket, std::chrono::milliseconds ttl);
    // 当前登记的票据数，含已过期但还没被清扫的。
    std::size_t pending_tickets() const;

private:
    struct Impl;

    Endpoint endpoint_;
    std::unique_ptr<Impl> impl_;
};

} // namespace breeze
