// 无测试框架依赖：失败即 abort。

#include "base/buffer/buffer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

std::span<const std::byte> View(const std::string& s)
{
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::string Str(std::span<const std::byte> v) { return {reinterpret_cast<const char*>(v.data()), v.size()}; }

void TestAppendConsume()
{
    Buffer b;
    CHECK(b.ReadableBytes() == 0);
    CHECK(b.Capacity() == 0);
    b.Append(View("hello"));
    b.Append(View(" world"));
    CHECK(Str(b.Readable()) == "hello world");
    b.Consume(6);
    CHECK(Str(b.Readable()) == "world");
    b.Consume(5);
    CHECK(b.ReadableBytes() == 0);
}

// 读空后下标回到起点：下一次写又能用满整块，不会越用越靠后。
void TestConsumeAllResets()
{
    Buffer b(16);
    b.Append(View("0123456789"));
    b.Consume(10);
    CHECK(b.Writable().size() == 16);
}

void TestAppendEmptyIsNoop()
{
    Buffer b;
    b.Append({});
    CHECK(b.Capacity() == 0 && b.ReadableBytes() == 0);
}

// readv 的用法：PrepareWrite → 往 Writable() 里写 → Commit。
void TestPrepareWriteCommit()
{
    Buffer b;
    b.PrepareWrite(100);
    CHECK(b.Writable().size() >= 100);
    std::memset(b.Writable().data(), 'x', 100);
    b.Commit(100);
    CHECK(b.ReadableBytes() == 100);
    CHECK(Str(b.Readable()) == std::string(100, 'x'));
}

// 已读部分加尾部空闲够用时挤到起点，不分配。
void TestCompactsBeforeGrowing()
{
    Buffer b(16);
    b.Append(View("abcdefghijkl")); // 12
    b.Consume(8);                   // 剩 "ijkl"，尾部只剩 4 字节空闲
    b.PrepareWrite(10);             // 需要 10：4 + 前面已读的 8 = 12 够用
    CHECK(b.Capacity() == 16);
    CHECK(Str(b.Readable()) == "ijkl");
    CHECK(b.Writable().size() >= 10);
}

void TestGrowsAndKeepsContent()
{
    Buffer b(8);
    std::string want;
    for (int i = 0; i < 200; ++i)
    {
        const std::string piece = "chunk" + std::to_string(i) + ";";
        want += piece;
        b.Append(View(piece));
    }
    CHECK(Str(b.Readable()) == want);
    CHECK(b.Capacity() >= want.size());
}

void TestGrowsAtLeastDoubling()
{
    Buffer b(8);
    b.Append(View("123456789")); // 9 > 8，必须扩容
    CHECK(b.Capacity() >= 16);
}

void TestShrinkIfEmpty()
{
    Buffer b(4096);
    b.Append(View("data"));
    b.ShrinkIfEmpty(0);
    CHECK(b.Capacity() == 4096); // 非空时不动
    CHECK(Str(b.Readable()) == "data");

    b.Consume(4);
    b.ShrinkIfEmpty(64);
    CHECK(b.Capacity() == 64);
    b.ShrinkIfEmpty(64); // 已经不大于 keep：不动
    CHECK(b.Capacity() == 64);
    b.ShrinkIfEmpty(0);
    CHECK(b.Capacity() == 0);

    b.Append(View("again")); // 收缩后照常可用
    CHECK(Str(b.Readable()) == "again");
}

// 被移走的对象回到空状态且仍可用（默认移动会留下 cap > 0 而内存为空的悬空状态）。
void TestMove()
{
    Buffer a(32);
    a.Append(View("abc"));
    Buffer b = std::move(a);
    CHECK(Str(b.Readable()) == "abc");
    CHECK(a.Capacity() == 0 && a.ReadableBytes() == 0); // NOLINT
    a.Append(View("xyz"));                              // NOLINT：移走后重新使用
    CHECK(Str(a.Readable()) == "xyz");

    Buffer c;
    c = std::move(b);
    CHECK(Str(c.Readable()) == "abc");
    CHECK(b.Capacity() == 0 && b.ReadableBytes() == 0); // NOLINT
}

} // namespace

int main()
{
    TestAppendConsume();
    TestConsumeAllResets();
    TestAppendEmptyIsNoop();
    TestPrepareWriteCommit();
    TestCompactsBeforeGrowing();
    TestGrowsAndKeepsContent();
    TestGrowsAtLeastDoubling();
    TestShrinkIfEmpty();
    TestMove();
    std::puts("buffer_test: ok");
    return 0;
}
