// 无测试框架依赖：失败即 abort。并发用例靠 tsan 预设抓数据竞争（cmake --preset tsan）。

#include "base/buffer/shared_buffer.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

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

namespace
{

std::vector<std::byte> Bytes(std::initializer_list<int> v)
{
    std::vector<std::byte> out;
    for (const int x : v)
        out.push_back(static_cast<std::byte>(x));
    return out;
}

bool Equal(std::span<const std::byte> a, std::span<const std::byte> b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

void TestEmpty()
{
    const SharedBuffer b;
    CHECK(b.empty());
    CHECK(b.size() == 0);
    CHECK(b.data() == nullptr);
    CHECK(b.UseCount() == 0);
    CHECK(SharedBuffer::Create(0).UseCount() == 0); // 0 字节不分配
    CHECK(SharedBuffer::CopyFrom({}).empty());
}

void TestCreateAndWrite()
{
    SharedBuffer b = SharedBuffer::Create(4);
    CHECK(b.size() == 4);
    CHECK(b.UseCount() == 1);
    const auto src = Bytes({1, 2, 3, 4});
    std::memcpy(b.MutableBytes().data(), src.data(), 4); // 序列化直接写进去
    CHECK(Equal(b.bytes(), src));
    CHECK(reinterpret_cast<std::uintptr_t>(b.data()) % 16 == 0);
}

void TestCopyFrom()
{
    const auto src = Bytes({9, 8, 7});
    const SharedBuffer b = SharedBuffer::CopyFrom(src);
    CHECK(Equal(b.bytes(), src));
}

// 同一块 payload 发给多条连接：只加引用计数，不拷贝数据。
void TestShareDoesNotCopy()
{
    const SharedBuffer a = SharedBuffer::CopyFrom(Bytes({1, 2, 3}));
    std::vector<SharedBuffer> conns(100, a);
    CHECK(a.UseCount() == 101);
    for (const auto& c : conns)
        CHECK(c.data() == a.data()); // 指向同一块内存
    conns.clear();
    CHECK(a.UseCount() == 1);
}

void TestSlice()
{
    SharedBuffer whole = SharedBuffer::CopyFrom(Bytes({10, 11, 12, 13, 14, 15}));
    SharedBuffer mid = whole.Slice(2, 3);
    CHECK(Equal(mid.bytes(), Bytes({12, 13, 14})));
    CHECK(mid.data() == whole.data() + 2);
    CHECK(whole.UseCount() == 2);

    SharedBuffer inner = mid.Slice(1, 1); // 切片的切片
    CHECK(Equal(inner.bytes(), Bytes({13})));
    CHECK(whole.UseCount() == 3);

    CHECK(whole.Slice(0, 0).empty());
    CHECK(whole.Slice(6, 0).empty()); // 恰好落在末尾

    // 切片可以比原对象活得久
    whole = SharedBuffer();
    CHECK(Equal(inner.bytes(), Bytes({13})));
    CHECK(inner.UseCount() == 2); // mid + inner
}

void TestMove()
{
    SharedBuffer a = SharedBuffer::CopyFrom(Bytes({1, 2}));
    const std::byte* p = a.data();
    SharedBuffer b = std::move(a);
    CHECK(a.empty() && a.UseCount() == 0); // NOLINT：被移走的对象回到空
    CHECK(b.data() == p && b.UseCount() == 1);

    SharedBuffer c;
    c = std::move(b);
    CHECK(b.empty()); // NOLINT
    CHECK(c.data() == p);

    const SharedBuffer& alias = c; // 经引用赋值：直接写 c = c 会触发编译器的自赋值警告
    c = alias;                     // 自赋值不崩、不丢数据
    CHECK(c.UseCount() == 1 && c.size() == 2);
}

// 生产线程分配、其它线程持有并释放：最后一个释放者销毁块（tsan 下检查引用计数与数据的同步）。
void TestCrossThreadRelease()
{
    constexpr int kThreads = 4;
    constexpr int kRounds = 2000;
    for (int r = 0; r < kRounds; ++r)
    {
        SharedBuffer a = SharedBuffer::Create(64);
        std::memset(a.MutableBytes().data(), r & 0xff, 64);
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; ++t)
        {
            ts.emplace_back(
                [copy = a, r]
                {
                    for (const std::byte b : copy.bytes())
                        CHECK(b == static_cast<std::byte>(r & 0xff));
                });
        }
        a = SharedBuffer(); // 主线程先放手，块由最后结束的线程释放
        for (auto& t : ts)
            t.join();
    }
}

} // namespace

int main()
{
    TestEmpty();
    TestCreateAndWrite();
    TestCopyFrom();
    TestShareDoesNotCopy();
    TestSlice();
    TestMove();
    TestCrossThreadRelease();
    std::puts("shared_buffer_test: ok");
    return 0;
}
