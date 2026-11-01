// 线格式、布局计算、memfd 封印、eventfd 校验与镜像映射。

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "base/shm/shm_defs.h"
#include "base/shm/shm_fd.h"
#include "base/shm/shm_memfd.h"
#include "base/shm/shm_mirror_map.h"
#include "base/shm/shm_test_util.h"

using namespace breeze::shm;
using breeze::shm::testing::Arm;

namespace
{

void TestRecordSize()
{
    CHECK(RecordSize(0) == 64);
    CHECK(RecordSize(1) == 64);
    CHECK(RecordSize(48) == 64); // 16 字节头 + 48 字节载荷正好一个缓存行
    CHECK(RecordSize(49) == 128);
    CHECK(RecordSize(64) == 128); // 设计文档 §8 的表：64 B 载荷占 128 B
    CHECK(RecordSize(128) == 192);
    CHECK(RecordSize(256) == 320);
    CHECK(RecordSize(1024) == 1088);
}

void TestCapacityRules()
{
    constexpr std::uint64_t kMax = kDefaultMaxRingCapacity;
    CHECK(!IsValidCapacity(0, kMax));
    CHECK(!IsValidCapacity(kMinRingCapacity / 2, kMax)); // 低于下限
    CHECK(IsValidCapacity(kMinRingCapacity, kMax));
    CHECK(!IsValidCapacity(kMinRingCapacity + 4096, kMax)); // 页的倍数但不是 2 的幂
    CHECK(!IsValidCapacity(3 * kMinRingCapacity, kMax));
    CHECK(IsValidCapacity(kDefaultRingCapacity, kMax));
    CHECK(IsValidCapacity(kMax, kMax));
    CHECK(!IsValidCapacity(2 * kMax, kMax)); // 超过上限
    CHECK(IsValidCapacity(2 * kMax, 2 * kMax));
}

void TestMaxPayloadRules()
{
    CHECK(DefaultMaxPayload(kMinRingCapacity) == kMinRingCapacity / 16);  // 1 MiB 的环：64 KiB
    CHECK(DefaultMaxPayload(kDefaultRingCapacity) == kDefaultMaxPayload); // 16 MiB 的环：1 MiB
    CHECK(DefaultMaxPayload(kDefaultMaxRingCapacity) == kDefaultMaxPayload);
    CHECK(!IsValidMaxPayload(0, kMinRingCapacity));
    CHECK(IsValidMaxPayload(1, kMinRingCapacity));
    CHECK(IsValidMaxPayload(kMinRingCapacity / 16, kMinRingCapacity));
    CHECK(!IsValidMaxPayload(kMinRingCapacity / 16 + 1, kMinRingCapacity));
}

void TestComputeLayout()
{
    const std::uint32_t mp = DefaultMaxPayload(kDefaultRingCapacity);
    auto l = ComputeLayout(kDefaultRingCapacity, kDefaultRingCapacity, mp, mp, 4096, kDefaultMaxRingCapacity);
    CHECK(l.has_value());
    CHECK(l->page_size == 4096);
    CHECK(l->tx.control_offset == 4096);
    CHECK(l->rx.control_offset == 8192);
    CHECK(l->tx.data_offset == 12288);
    CHECK(l->rx.data_offset == 12288 + kDefaultRingCapacity);
    CHECK(l->total_size == 12288 + 2 * kDefaultRingCapacity);
    CHECK(l->tx.max_payload == mp && l->rx.capacity == kDefaultRingCapacity);

    // 两条环可以不一样大。
    auto asym = ComputeLayout(kMinRingCapacity, 4 * kMinRingCapacity, 1024, 4096, 4096, kDefaultMaxRingCapacity);
    CHECK(asym.has_value());
    CHECK(asym->rx.data_offset == 12288 + kMinRingCapacity);
    CHECK(asym->total_size == 12288 + 5 * kMinRingCapacity);

    // 64 KiB 页（部分 ARM64 内核）：偏移随页大小，数据区仍页对齐。
    auto big_page = ComputeLayout(kMinRingCapacity, kMinRingCapacity, 1024, 1024, 65536, kDefaultMaxRingCapacity);
    CHECK(big_page.has_value());
    CHECK(big_page->tx.data_offset == 3 * 65536 && big_page->tx.data_offset % 65536 == 0);
    CHECK(big_page->rx.data_offset % 65536 == 0);

    // 不合法的参数一律 EINVAL，不取整不裁剪。
    auto bad = [&](std::uint64_t txc, std::uint64_t rxc, std::uint32_t txm, std::uint32_t rxm, std::uint32_t page)
    {
        auto r = ComputeLayout(txc, rxc, txm, rxm, page, kDefaultMaxRingCapacity);
        return !r.has_value() && r.error() == EINVAL;
    };
    CHECK(bad(0, kMinRingCapacity, 1024, 1024, 4096));
    CHECK(bad(kMinRingCapacity, 3 * kMinRingCapacity, 1024, 1024, 4096));
    CHECK(bad(kMinRingCapacity, 2 * kDefaultMaxRingCapacity, 1024, 1024, 4096));
    CHECK(bad(kMinRingCapacity, kMinRingCapacity, 0, 1024, 4096));
    CHECK(bad(kMinRingCapacity, kMinRingCapacity, 1024, kMinRingCapacity, 4096)); // 超过容量的 1/16
    CHECK(bad(kMinRingCapacity, kMinRingCapacity, 1024, 1024, 0));
    CHECK(bad(kMinRingCapacity, kMinRingCapacity, 1024, 1024, 3000)); // 页大小不是 2 的幂
    CHECK(bad(kMinRingCapacity, kMinRingCapacity, 1024, 1024, 2048)); // 放不下控制块
    CHECK(bad(kMinRingCapacity, kMinRingCapacity, 1024, 1024, 2 * kMinRingCapacity));
}

void TestStructLayout()
{
    // 控制块里的每个字段独占缓存行；整个控制块放得下一页。
    CHECK(offsetof(RingControl, write_idx) % 64 == 0);
    CHECK(offsetof(RingControl, read_idx) % 64 == 0);
    CHECK(offsetof(RingControl, consumer_sleeping) % 64 == 0);
    CHECK(offsetof(RingControl, inflight_idx) % 64 == 0);
    CHECK(offsetof(RingControl, producer_stats) % 64 == 0);
    CHECK(offsetof(RingControl, consumer_stats) % 64 == 0);
    CHECK(offsetof(RingControl, read_idx) - offsetof(RingControl, write_idx) >= 64);
    CHECK(sizeof(RingControl) <= 4096);
    CHECK(sizeof(ChannelHeader) <= 4096);
    CHECK(sizeof(RecordHeader) == 16);
    CHECK(offsetof(ChannelHeader, app_epoch) % 64 == 0);
    CHECK(offsetof(ChannelHeader, agent_epoch) % 64 == 0);
}

void TestMemFdSeal()
{
    auto fd = CreateMemFd("shm-layout-test", 2 * 4096);
    CHECK(fd.has_value());

    std::uint64_t size = 0;
    CHECK(CheckSealedMemFd(fd->get(), &size) == EINVAL); // 还没封印

    CHECK(SealMemFd(fd->get()) == 0);
    CHECK(CheckSealedMemFd(fd->get(), &size) == 0);
    CHECK(size == 2 * 4096);

    // 封印之后大小定死：缩小、增大都失败；数据照常能写。
    CHECK(::ftruncate(fd->get(), 4096) != 0 && errno == EPERM);
    CHECK(::ftruncate(fd->get(), 4 * 4096) != 0 && errno == EPERM);
    const char msg[] = "hello";
    CHECK(::pwrite(fd->get(), msg, sizeof(msg), 100) == static_cast<ssize_t>(sizeof(msg)));
    CHECK(SealMemFd(fd->get()) != 0); // F_SEAL_SEAL：封印本身也不能再改

    // 不是 memfd 的 fd 一律不通过。
    int pipefd[2];
    CHECK(::pipe(pipefd) == 0);
    Fd p0(pipefd[0]), p1(pipefd[1]);
    CHECK(CheckSealedMemFd(p0.get(), &size) == EINVAL);
    Fd null_fd(::open("/dev/null", O_RDWR));
    CHECK(null_fd.valid());
    CHECK(CheckSealedMemFd(null_fd.get(), &size) == EINVAL);
    auto ev = CreateEventFd();
    CHECK(ev.has_value());
    CHECK(CheckSealedMemFd(ev->get(), &size) == EINVAL);
    // 普通文件（不是 memfd，不支持封印）：是普通文件、大小也合适，同样不能通过。
    char tmpl[] = "/tmp/shm_layout_test_XXXXXX";
    const int tmp = ::mkstemp(tmpl);
    CHECK(tmp >= 0);
    ::unlink(tmpl);
    Fd tmp_fd(tmp);
    CHECK(::ftruncate(tmp_fd.get(), 4096) == 0);
    CHECK(CheckSealedMemFd(tmp_fd.get(), &size) == EINVAL);

    // 只封了一部分的 memfd 也不通过。
    auto partial = CreateMemFd("shm-layout-partial", 4096);
    CHECK(partial.has_value());
    CHECK(::fcntl(partial->get(), F_ADD_SEALS, F_SEAL_SHRINK) == 0);
    CHECK(CheckSealedMemFd(partial->get(), &size) == EINVAL);
}

void TestEventFdCheck()
{
    auto ev = CreateEventFd();
    CHECK(ev.has_value());
    CHECK(CheckEventFd(ev->get()) == 0);

    Fd blocking(::eventfd(0, 0)); // 阻塞的 eventfd：write 可能卡住对端，不接受
    CHECK(blocking.valid());
    CHECK(CheckEventFd(blocking.get()) == EINVAL);

    int pipefd[2];
    CHECK(::pipe2(pipefd, O_NONBLOCK) == 0); // 非阻塞，但不是 eventfd
    Fd p0(pipefd[0]), p1(pipefd[1]);
    CHECK(CheckEventFd(p0.get()) == EINVAL);

    auto mem = CreateMemFd("shm-layout-ev", 4096);
    CHECK(mem.has_value());
    CHECK(CheckEventFd(mem->get()) == EINVAL);
    CHECK(CheckEventFd(-1) != 0);

    // /proc 不可用（读不到链接目标）时无法确认是 eventfd，按失败处理，而不是放行。
    CHECK(CheckEventFd(ev->get(), "/nonexistent-proc-dir") == EINVAL);
}

void TestMirrorMapping()
{
    constexpr std::uint64_t kCap = 64 * 1024;
    auto fd = CreateMemFd("shm-layout-mirror", kCap);
    CHECK(fd.has_value());
    auto m = MirrorMapping::Map(fd->get(), 0, kCap);
    CHECK(m.has_value());
    CHECK(m->capacity() == kCap);

    std::byte* p = m->data();
    // 后半与前半是同一份内存：两个方向都验证。
    p[0] = std::byte{0xAB};
    CHECK(p[kCap] == std::byte{0xAB});
    p[kCap + 7] = std::byte{0xCD};
    CHECK(p[7] == std::byte{0xCD});

    // 跨尾部的一段在虚拟地址上连续：写在 [cap - 8, cap + 8)，从另一侧读回。
    std::byte pattern[16];
    for (int i = 0; i < 16; ++i)
        pattern[i] = static_cast<std::byte>(i + 1);
    std::memcpy(p + kCap - 8, pattern, 16);
    CHECK(std::memcmp(p + kCap - 8, pattern, 16) == 0);
    CHECK(std::memcmp(p + 2 * kCap - 8, pattern, 8) == 0); // 前 8 个字节在尾部镜像的末尾
    CHECK(std::memcmp(p, pattern + 8, 8) == 0);            // 后 8 个字节回到开头

    // 非零文件偏移：同一个 memfd 的后一段，与前一段互不干扰。
    auto fd2 = CreateMemFd("shm-layout-mirror2", 2 * kCap);
    CHECK(fd2.has_value());
    auto a = MirrorMapping::Map(fd2->get(), 0, kCap);
    auto b = MirrorMapping::Map(fd2->get(), kCap, kCap);
    CHECK(a.has_value() && b.has_value());
    a->data()[5] = std::byte{1};
    b->data()[5] = std::byte{2};
    CHECK(a->data()[5] == std::byte{1} && b->data()[5] == std::byte{2});

    // 移动之后地址不变、旧对象不再持有。
    std::byte* before = m->data();
    MirrorMapping moved = std::move(*m);
    CHECK(moved.data() == before && m->data() == nullptr);

    // 偏移没对齐、fd 不对，都返回错误而不是崩溃。
    CHECK(!MirrorMapping::Map(fd->get(), 100, kCap).has_value());
    CHECK(!MirrorMapping::Map(-1, 0, kCap).has_value());
}

// 当前进程的虚拟内存大小（字节），读 /proc/self/statm。
std::uint64_t VirtualBytes()
{
    FILE* f = std::fopen("/proc/self/statm", "r");
    CHECK(f != nullptr);
    unsigned long pages = 0;
    CHECK(std::fscanf(f, "%lu", &pages) == 1);
    std::fclose(f);
    return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
}

// 映射失败时要把预留的 2 × capacity 地址空间还回去：失败路径反复走，泄漏会越积越大。
// 用管道的 fd（不能 mmap）触发失败，每次预留 128 MiB 虚拟地址；泄漏的话 200 次就是 25 GiB。
void TestMirrorMapFailureDoesNotLeak()
{
    int pipefd[2];
    CHECK(::pipe(pipefd) == 0);
    Fd r(pipefd[0]), w(pipefd[1]);
    constexpr std::uint64_t kCap = 64ULL << 20;
    const std::uint64_t before = VirtualBytes();
    for (int i = 0; i < 200; ++i)
        CHECK(!MirrorMapping::Map(r.get(), 0, kCap).has_value());
    const std::uint64_t after = VirtualBytes();
    CHECK(after < before + (1ULL << 30));
}

} // namespace

int main()
{
    Arm(60);
    TestRecordSize();
    TestCapacityRules();
    TestMaxPayloadRules();
    TestComputeLayout();
    TestStructLayout();
    TestMemFdSeal();
    TestEventFdCheck();
    TestMirrorMapping();
    TestMirrorMapFailureDoesNotLeak();
    std::puts("shm_layout_test: OK");
    return 0;
}
