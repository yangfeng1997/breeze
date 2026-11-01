// IConnection 接口：没有数据成员（守住 StreamConnection 的热字段布局）、能被与流无关的实现填满、
// 业务回调只经接口就能工作。
// 假连接代表将来的 RUdpConnection：没有 fd、不经 epoll。
// 无测试框架依赖：失败即 abort。

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "base/buffer/shared_buffer.h"
#include "base/net/connection/connection_interface.h"
#include "base/net/connection/net_client_interface.h"
#include "base/net/connection/net_server_interface.h"

using namespace breeze;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

// 接口只有虚函数：对象里只有一个 vptr（继承链 IConnection → IPollable 上只有一个），不带任何数据。
// 派生类的成员排在基类之后，基类一旦带上数据，StreamConnection 里"热字段集中在前两条缓存行"的布局就整体后移
// （设计 §9.5）。这个断言守的就是这件事。
static_assert(std::is_abstract_v<IConnection>);
static_assert(sizeof(IConnection) == sizeof(void*), "IConnection 接口不能带数据成员");
static_assert(std::has_virtual_destructor_v<IConnection>);
static_assert(!std::is_copy_constructible_v<IConnection> && !std::is_move_constructible_v<IConnection>);

namespace
{

struct NoteContext final : ConnectionContext
{
    explicit NoteContext(int v)
        : value(v)
    {
    }
    int value;
};

struct OtherContext final : ConnectionContext
{
};

// 与流无关的 IConnection：把调用记下来。
class FakeConnection final : public IConnection
{
public:
    explicit FakeConnection(ConnectionHandle h = ConnectionHandle{})
        : handle_(h)
    {
    }

    SendResult Send(SharedBuffer payload) override
    {
        if (!connected_)
            return SendResult::Closed;
        sent.emplace_back(reinterpret_cast<const char*>(payload.bytes().data()), payload.size());
        return SendResult::Ok;
    }
    SendResult Send(std::span<const std::byte> payload) override
    {
        if (!connected_)
            return SendResult::Closed;
        sent.emplace_back(reinterpret_cast<const char*>(payload.data()), payload.size());
        return SendResult::Ok;
    }
    void Close() override { connected_ = false; }
    void CloseGracefully(std::chrono::milliseconds timeout) override
    {
        graceful_ms = static_cast<int>(timeout.count());
        connected_ = false;
    }
    void PauseRead() override { paused_ = true; }
    void ResumeRead() override { paused_ = false; }
    void SetContext(std::unique_ptr<ConnectionContext> ctx) noexcept override { ctx_ = std::move(ctx); }
    bool IsConnected() const noexcept override { return connected_; }
    bool IsPaused() const noexcept override { return paused_; }
    const PeerInfo& peer() const noexcept override { return peer_; }
    ConnectionHandle handle() const noexcept override { return handle_; }

    std::vector<std::string> sent;
    int graceful_ms = -1;

protected:
    ConnectionContext* context() const noexcept override { return ctx_.get(); }

private:
    // 没有自己的 fd，不经 epoll：不会被调用。
    void OnEvents(std::uint32_t) override { std::abort(); }
    void SendFromTask(SharedBuffer) override {}
    void CloseFromTask(CloseReason) override { connected_ = false; }
    void CloseIdle() override { connected_ = false; }
    void NotifyClosed() override {}
    std::uint32_t slot_index() const noexcept override { return 0; }

    bool connected_ = true;
    bool paused_ = false;
    PeerInfo peer_;
    ConnectionHandle handle_;
    std::unique_ptr<ConnectionContext> ctx_;
};

// 业务回调只认接口：同一份代码对任何实现都工作。
class EchoOnce final : public IConnectionHandler
{
public:
    void OnConnected(IConnection& c) noexcept override
    {
        c.SetContext(std::make_unique<NoteContext>(7));
        connected_handle = c.handle();
    }
    void OnMessage(IConnection& c, std::span<const std::byte> payload) noexcept override
    {
        last_send = c.Send(payload);
        if (c.GetContext<NoteContext>()->value == 7)
            ++messages;
        if (messages == 2)
            c.CloseGracefully(std::chrono::milliseconds(50));
    }
    void OnClosed(IConnection&, CloseReason reason, int) noexcept override { closed_reason = reason; }

    ConnectionHandle connected_handle;
    SendResult last_send = SendResult::Closed;
    int messages = 0;
    CloseReason closed_reason = CloseReason::IoError;
};

void TestDispatchThroughInterface()
{
    FakeConnection fake;
    IConnection& c = fake; // 经基类引用调用：真的是虚调用
    CHECK(c.IsConnected() && !c.IsPaused());

    CHECK(c.Send(SharedBuffer::CopyFrom(std::as_bytes(std::span("abc", 3)))) == SendResult::Ok);
    CHECK(c.Send(std::as_bytes(std::span("de", 2))) == SendResult::Ok);
    CHECK(fake.sent == (std::vector<std::string>{"abc", "de"}));

    c.PauseRead();
    CHECK(c.IsPaused());
    c.ResumeRead();
    CHECK(!c.IsPaused());

    c.Close();
    CHECK(!c.IsConnected());
    CHECK(c.Send(std::as_bytes(std::span("x", 1))) == SendResult::Closed);
    CHECK(fake.sent.size() == 2);
}

void TestContextThroughInterface()
{
    FakeConnection fake;
    IConnection& c = fake;
    CHECK(c.GetContext<NoteContext>() == nullptr); // 没设过：空
    c.SetContext(std::make_unique<NoteContext>(42));
    CHECK(c.GetContext<NoteContext>() != nullptr && c.GetContext<NoteContext>()->value == 42);
    c.SetContext(std::make_unique<OtherContext>()); // 换一个：旧的随之销毁
    CHECK(c.GetContext<OtherContext>() != nullptr);
}

void TestHandle()
{
    const ConnectionHandle h = ConnectionHandle::Make(1, 2, 3, 4);
    FakeConnection with(h);
    FakeConnection without;
    const IConnection& a = with;
    const IConnection& b = without;
    CHECK(a.handle() == h && a.handle().valid());
    CHECK(!b.handle().valid() && b.handle().raw() == 0); // 没有句柄：无效句柄
}

// 与流无关的服务端 / 客户端：NetModule 与业务只认 INetServer / INetClient，将来的 RUdpServer / RUdpClient 就是这个角色。
class FakeServer final : public INetServer
{
public:
    explicit FakeServer(Endpoint ep)
        : ep_(std::move(ep))
    {
    }
    int Start() override
    {
        ++starts;
        return fail_with;
    }
    void Stop() override { ++stops; }
    const Endpoint& endpoint() const noexcept override { return ep_; }

    int fail_with = 0;
    int starts = 0;
    int stops = 0;

private:
    Endpoint ep_;
};

class FakeClient final : public INetClient
{
public:
    explicit FakeClient(Endpoint ep)
        : ep_(std::move(ep))
    {
    }
    int Start() override
    {
        running = true;
        return 0;
    }
    void Stop() override { running = false; }
    SendResult Send(SharedBuffer payload) override
    {
        if (!running)
            return SendResult::Closed;
        sent += payload.size();
        return SendResult::Ok;
    }
    ConnectionHandle handle() const noexcept override
    {
        return running ? ConnectionHandle::Make(0, 0, 1, 1) : ConnectionHandle{};
    }
    const Endpoint& endpoint() const noexcept override { return ep_; }

    bool running = false;
    std::size_t sent = 0;

private:
    Endpoint ep_;
};

void TestServerAndClientInterfaces()
{
    static_assert(std::is_abstract_v<INetServer> && std::is_abstract_v<INetClient>);
    static_assert(std::has_virtual_destructor_v<INetServer> && std::has_virtual_destructor_v<INetClient>);
    static_assert(!std::is_copy_constructible_v<INetServer> && !std::is_copy_constructible_v<INetClient>);

    // 经基类指针持有、启停、销毁：NetModule 就是这样用的。
    std::vector<std::unique_ptr<INetServer>> servers;
    servers.push_back(std::make_unique<FakeServer>(*Endpoint::Udp("127.0.0.1", 0)));
    servers.push_back(std::make_unique<FakeServer>(*Endpoint::Tcp("127.0.0.1", 0)));
    for (auto& s : servers)
        CHECK(s->Start() == 0);
    CHECK(servers[0]->endpoint().kind() == Endpoint::Kind::Udp && servers[1]->endpoint().kind() == Endpoint::Kind::Tcp);
    static_cast<FakeServer&>(*servers[1]).fail_with = EADDRINUSE;
    CHECK(servers[1]->Start() == EADDRINUSE);
    for (auto& s : servers)
        s->Stop();
    CHECK(static_cast<FakeServer&>(*servers[0]).starts == 1 && static_cast<FakeServer&>(*servers[0]).stops == 1);

    std::unique_ptr<INetClient> c = std::make_unique<FakeClient>(*Endpoint::Udp("127.0.0.1", 9));
    CHECK(c->Send(SharedBuffer::CopyFrom(std::as_bytes(std::span("a", 1)))) == SendResult::Closed &&
          !c->handle().valid());
    CHECK(c->Start() == 0 && c->handle().valid());
    CHECK(c->Send(SharedBuffer::CopyFrom(std::as_bytes(std::span("abc", 3)))) == SendResult::Ok);
    CHECK(static_cast<FakeClient&>(*c).sent == 3 && c->endpoint().kind() == Endpoint::Kind::Udp);
    c->Stop();
    CHECK(!c->handle().valid());
}

void TestHandlerOnlySeesInterface()
{
    EchoOnce handler;
    FakeConnection fake(ConnectionHandle::Make(0, 0, 5, 1));
    IConnection& c = fake;
    handler.OnConnected(c);
    CHECK(handler.connected_handle == c.handle());

    const auto msg = std::as_bytes(std::span("ping", 4));
    handler.OnMessage(c, msg);
    CHECK(handler.last_send == SendResult::Ok && handler.messages == 1 && fake.sent.size() == 1 &&
          fake.sent[0] == "ping");
    handler.OnMessage(c, msg);
    CHECK(handler.messages == 2 && fake.graceful_ms == 50 && !c.IsConnected()); // 第二条之后优雅关闭

    handler.OnClosed(c, CloseReason::LocalClose, 0);
    CHECK(handler.closed_reason == CloseReason::LocalClose);
}

} // namespace

int main()
{
    TestDispatchThroughInterface();
    TestContextThroughInterface();
    TestHandle();
    TestServerAndClientInterfaces();
    TestHandlerOnlySeesInterface();
    std::puts("connection_test: ok");
    return 0;
}
