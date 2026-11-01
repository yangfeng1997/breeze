// 握手线格式：通道名规则、Hello 的校验、Reply 头的校验。
// 无测试框架依赖：失败即 abort。

#include "base/net/shm/shm_protocol.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

using namespace breeze::shm::wire;

namespace
{

Hello ValidHello()
{
    Hello h{};
    h.hdr = MakeHeader(MsgType::kHello);
    h.attach_type = static_cast<std::uint32_t>(AttachType::kAttach);
    h.transport = static_cast<std::uint32_t>(Transport::kShmRequired);
    h.pid = 1234;
    CHECK(SetBusId(h, "svr-1.zone_2:a@b"));
    return h;
}

Hello ValidReattach()
{
    Hello h = ValidHello();
    h.attach_type = static_cast<std::uint32_t>(AttachType::kReattach);
    return h;
}

void TestBusIdRules()
{
    CHECK(IsValidBusId("a"));
    CHECK(IsValidBusId("Game-Server_01.zone:3@host"));
    CHECK(IsValidBusId(std::string(kBusIdSize - 1, 'x'))); // 63 个字符，刚好
    CHECK(!IsValidBusId(""));
    CHECK(!IsValidBusId(std::string(kBusIdSize, 'x'))); // 64 个：放不下结尾的 '\0'
    CHECK(!IsValidBusId("has space"));
    CHECK(!IsValidBusId("a/b")); // 路径分隔符
    CHECK(!IsValidBusId("a\\b"));
    CHECK(!IsValidBusId("中文"));
    CHECK(!IsValidBusId(std::string("a\0b", 3))); // 内嵌 '\0'
    CHECK(!IsValidBusId("a\nb"));
}

void TestBusIdRoundTrip()
{
    Hello h{};
    std::memset(h.busid, 0x7F, sizeof(h.busid));
    CHECK(SetBusId(h, "abc"));
    CHECK(GetBusId(h).value() == "abc");
    // 名字之后的字节必须清零（不带出旧内容）。
    for (std::size_t i = 3; i < kBusIdSize; ++i)
        CHECK(h.busid[i] == '\0');

    // 非法名字：返回 false，Hello 不变。
    Hello before = h;
    CHECK(!SetBusId(h, "bad name"));
    CHECK(!SetBusId(h, ""));
    CHECK(std::memcmp(&before, &h, sizeof(h)) == 0);

    // 对端发来的：没有结尾 '\0'（64 个字节全是字符）不能当字符串用。
    Hello unterminated{};
    std::memset(unterminated.busid, 'a', sizeof(unterminated.busid));
    CHECK(!GetBusId(unterminated).has_value());

    // '\0' 之后的内容被忽略，但名字本身仍要合法。
    Hello tail{};
    std::memcpy(tail.busid, "abc\0\xff\xfe", 6);
    CHECK(GetBusId(tail).value() == "abc");
    Hello bad_char{};
    std::memcpy(bad_char.busid, "ab\x01", 4);
    CHECK(!GetBusId(bad_char).has_value());
    Hello empty{};
    CHECK(!GetBusId(empty).has_value()); // 空名字
}

void TestCheckHello()
{
    CHECK(CheckHello(ValidHello()) == 0);

    auto mutated = [](auto fn)
    {
        Hello h = ValidHello();
        fn(h);
        return CheckHello(h);
    };
    CHECK(mutated([](Hello& h) { h.hdr.magic ^= 1; }) == EPROTO);
    CHECK(mutated([](Hello& h) { h.hdr.version = kVersion + 1; }) == EPROTO);
    CHECK(mutated([](Hello& h) { h.hdr.version = 0; }) == EPROTO);
    CHECK(mutated([](Hello& h) { h.hdr.type = static_cast<std::uint16_t>(MsgType::kReply); }) == EPROTO);
    CHECK(mutated([](Hello& h) { h.hdr.type = 0; }) == EPROTO);
    CHECK(mutated([](Hello& h) { h.attach_type = 0; }) == EINVAL);
    CHECK(mutated([](Hello& h) { h.attach_type = 99; }) == EINVAL);
    CHECK(mutated([](Hello& h) { h.flags = 2; }) == EINVAL);
    CHECK(mutated([](Hello& h) { h.flags = 0x80000000u; }) == EINVAL);
    // 传输方式三种都合法（Attach）；0 和越界不合法。
    CHECK(mutated([](Hello& h) { h.transport = static_cast<std::uint32_t>(Transport::kUdsOnly); }) == 0);
    CHECK(mutated([](Hello& h) { h.transport = static_cast<std::uint32_t>(Transport::kShmPreferred); }) == 0);
    CHECK(mutated([](Hello& h) { h.transport = 0; }) == EINVAL);
    CHECK(mutated([](Hello& h) { h.transport = 4; }) == EINVAL);
    // 丢弃保留队列：Attach 可以带。
    CHECK(mutated([](Hello& h) { h.flags = kFlagDiscardRetained; }) == 0);
    CHECK(mutated([](Hello& h) { std::memset(h.busid, 0, sizeof(h.busid)); }) == EINVAL);
    CHECK(mutated([](Hello& h) { h.busid[0] = '/'; }) == EINVAL);
    CHECK(mutated([](Hello& h) { std::memset(h.busid, 'a', sizeof(h.busid)); }) == EINVAL);
    // 魔数 / 版本不符优先于别的错误：对端不是我们的协议时，别的字段没有意义。
    CHECK(mutated(
              [](Hello& h)
              {
                  h.hdr.magic = 0;
                  h.flags = 7;
              }) == EPROTO);
    // 容量不在这里校验（创建通道时校验）：Attach 的任意值都过得了 CheckHello。
    CHECK(mutated([](Hello& h) { h.tx_capacity = 12345; }) == 0);
    CHECK(mutated([](Hello& h) { h.pid = -1; }) == 0); // pid 只供日志
}

void TestCheckReattachHello()
{
    CHECK(CheckHello(ValidReattach()) == 0);
    auto mutated = [](auto fn)
    {
        Hello h = ValidReattach();
        fn(h);
        return CheckHello(h);
    };
    CHECK(mutated([](Hello& h) { h.transport = static_cast<std::uint32_t>(Transport::kShmPreferred); }) == 0);
    // Reattach 恢复的是手里已有的内存：不能退化成仅 UDS，没有容量、载荷上限、可丢弃的队列。
    CHECK(mutated([](Hello& h) { h.transport = static_cast<std::uint32_t>(Transport::kUdsOnly); }) == EINVAL);
    CHECK(mutated([](Hello& h) { h.flags = kFlagDiscardRetained; }) == EINVAL);
    CHECK(mutated([](Hello& h) { h.tx_capacity = 1 << 20; }) == EINVAL);
    CHECK(mutated([](Hello& h) { h.rx_capacity = 1 << 20; }) == EINVAL);
    CHECK(mutated([](Hello& h) { h.tx_max_payload = 1; }) == EINVAL);
    CHECK(mutated([](Hello& h) { h.rx_max_payload = 1; }) == EINVAL);
    CHECK(mutated([](Hello& h) { h.busid[0] = '/'; }) == EINVAL);
}

void TestControl()
{
    CHECK(CheckControl(MakeControl(ControlOp::kPrepareRestart, 30000)) == 0);
    CHECK(CheckControl(MakeControl(ControlOp::kPrepareRestart, 1)) == 0);
    CHECK(CheckControl(MakeControl(ControlOp::kPrepareRestart, 0)) == EINVAL); // 0 毫秒没有意义
    CHECK(CheckControl(MakeControl(ControlOp::kBye, 0)) == 0);
    CHECK(CheckControl(MakeControl(ControlOp::kBye, 1)) == EINVAL); // Bye 不带参数
    CHECK(CheckControl(MakeControl(static_cast<ControlOp>(0), 0)) == EINVAL);
    CHECK(CheckControl(MakeControl(static_cast<ControlOp>(3), 5)) == EINVAL);

    Control c = MakeControl(ControlOp::kBye, 0);
    c.hdr.magic ^= 1;
    CHECK(CheckControl(c) == EPROTO);
    c = MakeControl(ControlOp::kBye, 0);
    c.hdr.version = kVersion + 1;
    CHECK(CheckControl(c) == EPROTO);
    c = MakeControl(ControlOp::kBye, 0);
    c.hdr.type = static_cast<std::uint16_t>(MsgType::kHello);
    CHECK(CheckControl(c) == EPROTO);
    // 头不符优先于 op 不合法：对端不是我们的协议时，别的字段没有意义。
    c = MakeControl(static_cast<ControlOp>(99), 7);
    c.hdr.magic = 0;
    CHECK(CheckControl(c) == EPROTO);
}

void TestReplyHeader()
{
    Reply r{};
    r.hdr = MakeHeader(MsgType::kReply);
    CHECK(CheckReplyHeader(r) == 0);
    Reply bad = r;
    bad.hdr.magic = 0;
    CHECK(CheckReplyHeader(bad) == EPROTO);
    bad = r;
    bad.hdr.version = kVersion + 1;
    CHECK(CheckReplyHeader(bad) == EPROTO);
    bad = r;
    bad.hdr.type = static_cast<std::uint16_t>(MsgType::kHello);
    CHECK(CheckReplyHeader(bad) == EPROTO);
}

void TestWireSizes()
{
    // 两端同机同布局直接收发：尺寸写死，改了字段就要升 kVersion。
    CHECK(sizeof(Header) == 8 && sizeof(Hello) == 112 && sizeof(Reply) == 80 && sizeof(Control) == 16);
    CHECK(MakeHeader(MsgType::kHello).magic == kMagic && MakeHeader(MsgType::kHello).version == kVersion);
    // 魔数的字节序列是 'B' 'S' 'H' 'M'（小端），抓包 / hexdump 时一眼能认。
    const Header h = MakeHeader(MsgType::kHello);
    CHECK(std::memcmp(&h.magic, "BSHM", 4) == 0);
}

} // namespace

int main()
{
    TestBusIdRules();
    TestBusIdRoundTrip();
    TestCheckHello();
    TestCheckReattachHello();
    TestControl();
    TestReplyHeader();
    TestWireSizes();
    std::puts("shm_protocol_test: OK");
    return 0;
}
