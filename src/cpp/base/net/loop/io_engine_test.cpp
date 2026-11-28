// 故障路径直接驱动引擎：取消与迟到 CQE 不许触碰失效 sink，零长 UDP 也必须有队列上界。
#include "base/net/loop/io_engine.h"

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <cstring>

#include "base/net/loop/io_operation_table.h"
#include "base/net/socket/unique_fd.h"
#include "base/net/testing/net_test_util.h"

using namespace breeze;
using namespace breeze::testing;

namespace
{
void TestOperationSlotRetirement()
{
    detail::IoOperationTable<int, std::uint8_t> table;
    CHECK(table.empty() && table.Get(0) == nullptr && table.Get(1) == nullptr);
    std::vector<std::uint64_t> retired_ids;
    for (unsigned generation = 1; generation <= UINT8_MAX; ++generation)
    {
        auto value = std::make_unique<int>(42);
        auto* address = value.get();
        const auto id = table.Insert(std::move(value));
        CHECK((id >> 32) == generation && static_cast<std::uint32_t>(id) == 0);
        CHECK(!table.empty() && table.Get(id) == address);
        for (auto stale : retired_ids)
            CHECK(table.Get(stale) == nullptr);
        CHECK(table.Remove(id).get() == address && table.Get(id) == nullptr && table.empty());
        retired_ids.push_back(id);
    }
    // 槽0代次已耗尽，继续工作必须分配槽1，所有旧ID仍失效。
    const auto next = table.Insert(std::make_unique<int>(7));
    CHECK(static_cast<std::uint32_t>(next) == 1 && (next >> 32) == 1);
    auto* address = table.Get(next);
    for (int i = 0; i < 4096; ++i)
        table.Insert(std::make_unique<int>(i));
    CHECK(table.Get(next) == address && *address == 7); // 扩容不搬走内核引用对象。
    for (auto stale : retired_ids)
        CHECK(table.Get(stale) == nullptr);
    table.Clear();
    CHECK(table.empty());
}
struct DatagramSink final : IDatagramCompletion
{
    unsigned received = 0;
    unsigned errors = 0;
    void OnIoDatagram(const sockaddr_storage&, socklen_t, std::span<const std::byte>) override { ++received; }
    void OnIoDatagramError(int) override { ++errors; }
};
struct StreamSink final : IStreamCompletion
{
    unsigned reads = 0;
    unsigned writes = 0;
    void OnIoRead(std::span<std::byte>, int) override { ++reads; }
    void OnIoWrite(std::size_t, int) override { ++writes; }
};
void Pump(IoEngine& engine)
{
    std::array<IoEvent, 4> events;
    const int n = engine.Wait(events, 10);
    CHECK(n >= 0);
    for (int i = 0; i < n; ++i)
        events[i].target->OnEvents(events[i].events);
    engine.Dispatch();
    engine.Flush();
}
} // namespace

int main()
{
    const LeakCheck leak;
    TestOperationSlotRetirement();
    int error = 0;
    auto engine = MakeIoEngine(IoBackend::IoUring, error);
    if (!engine || engine->backend() != IoBackend::IoUring)
    {
        CHECK(error == 0 || error == ENOTSUP || error == EPERM || error == ENOSYS);
        CHECK(std::getenv("BREEZE_REQUIRE_URING") == nullptr);
        auto fallback = MakeIoEngine(IoBackend::Auto, error);
        CHECK(fallback != nullptr && error == 0 && fallback->backend() == IoBackend::Epoll);
        return 77;
    }
    {
        int pair[2];
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
        UniqueFd receiver(pair[0]), sender(pair[1]);
        StreamSink sink;
        const auto id = engine->AttachStream(receiver.get(), sink, error);
        CHECK(id != 0 && error == 0);
        const char bytes[] = "pending";
        iovec buffer{const_cast<char*>(bytes), sizeof(bytes)};
        CHECK(engine->WriteStream(id, std::span<const iovec>(&buffer, 1)) == 0);
        engine->Detach(id);
        receiver.reset();
        for (int i = 0; i < 4; ++i)
            Pump(*engine);
        CHECK(sink.reads == 0 && sink.writes == 0);
    }
    {
        int pair[2];
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
        UniqueFd receiver(pair[0]), sender(pair[1]);
        StreamSink sink;
        const auto id = engine->AttachStream(sender.get(), sink, error);
        CHECK(id != 0 && error == 0);
        SharedBuffer owner = SharedBuffer::Create(64);
        std::memset(owner.MutableBytes().data(), 0x5a, owner.size());
        CHECK(owner.UseCount() == 1);
        CHECK(engine->WriteOwnedStream(id, owner) == 0);
        CHECK(owner.UseCount() == 2); // 在途请求引用原块，没有复制成第二个payload。
        engine->Detach(id);
        sender.reset();
        CHECK(owner.UseCount() == 2); // 取消提交不等于内核已经放弃这块内存。
        for (int n = 0; n < 4; ++n)
            Pump(*engine);
        CHECK(owner.UseCount() == 1 && sink.reads == 0 && sink.writes == 0);
    }
    {
        int pair[2];
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
        UniqueFd receiver(pair[0]), sender(pair[1]);
        StreamSink sink;
        const auto id = engine->AttachStream(sender.get(), sink, error);
        CHECK(id != 0 && error == 0);
        SharedBuffer owner = SharedBuffer::Create(64);
        std::memset(owner.MutableBytes().data(), 'p', owner.size());
        std::array<std::byte, 4> prefix{std::byte{'h'}, std::byte{'e'}, std::byte{'a'}, std::byte{'d'}};
        CHECK(engine->WriteOwnedStreamV(id, prefix, owner) == 0);
        CHECK(owner.UseCount() == 2); // payload引用原块；小前缀由引擎独立持有。
        prefix.fill(std::byte{'x'});
        for (unsigned n = 0; n < 10 && sink.writes == 0; ++n)
            Pump(*engine);
        CHECK(sink.writes == 1 && owner.UseCount() == 1);
        std::array<char, 68> got{};
        CHECK(::recv(receiver.get(), got.data(), got.size(), MSG_WAITALL) == static_cast<ssize_t>(got.size()));
        CHECK(std::memcmp(got.data(), "head", 4) == 0);
        for (std::size_t i = 4; i < got.size(); ++i)
            CHECK(got[i] == 'p');
        CHECK(engine->WriteOwnedStreamV(id, {}, owner) == 0);
        engine->Detach(id);
        sender.reset();
        for (unsigned n = 0; n < 4; ++n)
            Pump(*engine);
        CHECK(owner.UseCount() == 1 && sink.writes == 1);
    }
    {
        int pair[2];
        CHECK(::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
        UniqueFd receiver(pair[0]), sender(pair[1]);
        DatagramSink sink;
        const auto id = engine->AttachDatagram(receiver.get(), sink, error);
        CHECK(id != 0 && error == 0);
        unsigned accepted = 0;
        for (; accepted < 4096; ++accepted)
        {
            const int rc = engine->SendDatagram(id, {}, {}, nullptr, 0);
            if (rc == EAGAIN)
                break;
            CHECK(rc == 0);
        }
        CHECK(accepted > 0 && accepted <= 256); // 字节为零也不能无限堆请求。
        engine->Detach(id);
        receiver.reset();
        for (int i = 0; i < 4; ++i)
            Pump(*engine);
        CHECK(sink.received == 0 && sink.errors == 0);
    }
    {
        // 两个 socket 的突发超过共享池256个buffer，ENOBUFS必须能重挂并最终交付所有数据。
        std::array<UniqueFd, 4> fds;
        std::array<DatagramSink, 2> sinks;
        std::array<IoRegistration, 2> ids{};
        for (unsigned i = 0; i < 2; ++i)
        {
            int pair[2];
            CHECK(::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
            fds[i * 2].reset(pair[0]);
            fds[i * 2 + 1].reset(pair[1]);
            ids[i] = engine->AttachDatagram(pair[0], sinks[i], error);
            CHECK(ids[i] != 0 && error == 0);
            const char value = 'x';
            for (unsigned n = 0; n < 160; ++n)
                CHECK(::send(pair[1], &value, 1, MSG_NOSIGNAL) == 1);
        }
        for (unsigned round = 0; round < 100 && sinks[0].received + sinks[1].received != 320; ++round)
            Pump(*engine);
        CHECK(sinks[0].received == 160 && sinks[1].received == 160);
        CHECK(engine->stats().receive_buffer_exhaustions > 0);
        CHECK(sinks[0].errors == 0 && sinks[1].errors == 0);
        for (auto id : ids)
            engine->Detach(id);
    }
    {
        // 批量链必须保持数据报顺序；一批失败也不能连带取消后续独立数据报。
        int pair[2];
        CHECK(::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
        UniqueFd receiver(pair[0]), sender(pair[1]);
        DatagramSink sink;
        const auto id = engine->AttachDatagram(sender.get(), sink, error);
        CHECK(id != 0 && error == 0);
        for (unsigned n = 0; n < 128; ++n)
        {
            const auto data = std::as_bytes(std::span(&n, 1));
            CHECK(engine->SendDatagram(id, {}, data, nullptr, 0) == 0);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (unsigned expected = 0; expected < 128;)
        {
            CHECK(std::chrono::steady_clock::now() < deadline);
            Pump(*engine);
            unsigned value = 0;
            const auto n = ::recv(receiver.get(), &value, sizeof(value), MSG_DONTWAIT);
            if (n < 0)
            {
                CHECK(errno == EAGAIN);
                continue;
            }
            CHECK(n == sizeof(value) && value == expected);
            ++expected;
        }
        engine->Detach(id);
    }
    {
        // 同一条硬链接批次中第一个发送失败，不能把后续独立报文当成ECANCELED丢掉。
        int pair[2];
        CHECK(::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
        UniqueFd receiver(pair[0]), sender(pair[1]);
        DatagramSink sink;
        const auto id = engine->AttachDatagram(sender.get(), sink, error);
        CHECK(id != 0 && error == 0);
        sockaddr_storage invalid{};
        invalid.ss_family = AF_INET; // AF_UNIX连接不能向AF_INET地址发送，错误在CQE异步返回。
        const std::array<std::byte, 1> value{std::byte{0x5a}};
        CHECK(engine->SendDatagram(id, {}, value, &invalid, sizeof(sockaddr_storage)) == 0);
        CHECK(engine->SendDatagram(id, {}, value, nullptr, 0) == 0);
        std::array<std::byte, 1> received{};
        bool delivered = false;
        for (unsigned round = 0; round < 100 && (!delivered || sink.errors == 0); ++round)
        {
            Pump(*engine);
            if (!delivered)
            {
                const auto n = ::recv(receiver.get(), received.data(), received.size(), MSG_DONTWAIT);
                if (n < 0)
                    CHECK(errno == EAGAIN);
                else
                {
                    CHECK(n == 1 && received == value);
                    delivered = true;
                }
            }
        }
        CHECK(delivered && sink.errors == 1);
        engine->Detach(id);
    }
    {
        // CQE 已收割但尚未回调时撤销注册，再在同一socket建新注册；旧数据不能交给新sink。
        int pair[2];
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
        UniqueFd receiver(pair[0]), sender(pair[1]);
        StreamSink old_sink, new_sink;
        auto old_id = engine->AttachStream(receiver.get(), old_sink, error);
        CHECK(old_id != 0 && error == 0);
        engine->Flush();
        const char old_byte = 'a';
        CHECK(::send(sender.get(), &old_byte, 1, MSG_NOSIGNAL) == 1);
        std::array<IoEvent, 1> ready;
        bool consumed = false;
        for (int round = 0; round < 10 && !consumed; ++round)
        {
            CHECK(engine->Wait(ready, 100) >= 0); // 故意不 Dispatch，旧完成项仍由引擎持有。
            char peek;
            const auto n = ::recv(receiver.get(), &peek, 1, MSG_PEEK | MSG_DONTWAIT);
            consumed = n < 0 && errno == EAGAIN;
            CHECK(consumed || (n == 1 && peek == old_byte));
            if (!consumed)
            {
                engine->Dispatch(); // readiness完成后才会获得真实读取名额；仍不分发读取CQE。
                engine->Flush();
            }
        }
        CHECK(consumed);
        engine->Detach(old_id);
        auto new_id = engine->AttachStream(receiver.get(), new_sink, error);
        CHECK(new_id != 0 && new_id != old_id && error == 0);
        const char new_byte = 'b';
        CHECK(::send(sender.get(), &new_byte, 1, MSG_NOSIGNAL) == 1);
        for (int round = 0; round < 10 && new_sink.reads == 0; ++round)
            Pump(*engine);
        CHECK(old_sink.reads == 0 && old_sink.writes == 0 && new_sink.reads == 1);
        engine->Detach(new_id);
    }
    {
        // 活跃流远多于共享buffer，读取名额必须轮转；不能靠ENOBUFS重试让部分连接饥饿。
        constexpr unsigned count = 2048;
        StreamSink sink;
        std::vector<UniqueFd> fds;
        std::vector<IoRegistration> ids;
        const auto exhausted = engine->stats().receive_buffer_exhaustions;
        for (unsigned i = 0; i < count; ++i)
        {
            int pair[2];
            CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
            fds.emplace_back(pair[0]);
            fds.emplace_back(pair[1]);
            const auto id = engine->AttachStream(pair[0], sink, error);
            CHECK(id != 0 && error == 0);
            ids.push_back(id);
            const char value = 'x';
            CHECK(::send(pair[1], &value, 1, MSG_NOSIGNAL) == 1);
        }
        const auto until = std::chrono::steady_clock::now() + 5s;
        while (sink.reads < count && std::chrono::steady_clock::now() < until)
            Pump(*engine);
        CHECK(sink.reads == count);
        CHECK(engine->stats().receive_buffer_exhaustions == exhausted);
        for (auto id : ids)
            engine->Detach(id);
        for (unsigned n = 0; n < 32; ++n)
            Pump(*engine);
        CHECK(sink.reads == count && sink.writes == 0);
    }
    engine->Drain(); // 前一个测试留下的空报文可能仍在等待对端接收，不能计入本测试的fd基线。
    engine = MakeIoEngine(IoBackend::IoUring, error);
    CHECK(engine != nullptr && error == 0);
    {
        const auto before = CountEntries("/proc/self/fd");
        StreamSink sink;
        std::vector<IoRegistration> ids;
        std::vector<UniqueFd> sockets;
        for (unsigned n = 0; n < 1100; ++n)
        {
            int pair[2];
            CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
            sockets.emplace_back(pair[0]);
            sockets.emplace_back(pair[1]);
            const auto id = engine->AttachStream(pair[0], sink, error);
            CHECK(id != 0 && error == 0);
            ids.push_back(id);
            const char value = 'x';
            CHECK(::send(pair[1], &value, 1, MSG_NOSIGNAL) == 1);
        }
        std::array<IoEvent, 1024> ready;
        CHECK(engine->Wait(ready, 100) >= 0);
        engine->Dispatch();
        engine->Flush(); // 至少一批真实RECV持有读取名额，其余仍等候FIFO/就绪。
        const auto submitted = engine->stats().submissions;
        const auto delivered = sink.reads;
        for (auto id : ids)
            engine->Detach(id);
        CHECK(engine->stats().submissions == submitted); // 大批Detach自身不提交，须给CQ收割机会。
        sockets.clear();
        engine->Drain(); // 超过单轮1024完成项预算，不能依赖下一轮才释放重复fd。
        const auto after = CountEntries("/proc/self/fd");
        if (after != before)
            std::fprintf(stderr, "Drain fd before=%d after=%d\n", before, after);
        CHECK(after == before);
        CHECK(sink.reads == delivered && sink.writes == 0);
    }
    CHECK(engine->stats().pending_send_bytes == 0);
    std::puts("io_engine_test: ok");
}
