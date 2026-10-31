// TimeRecord：秒表语义、析构时记日志（名字、耗时、创建位置）、慢操作阈值、Cancel / SetName、宏。
// 日志写到 stderr（没有默认 logger 时的兜底），用重定向 fd 2 的办法读回来。无测试框架依赖：失败即 abort。

#include "base/time_record/time_record.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

using namespace breeze;
using namespace std::chrono_literals;

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

// 在当前进程里跑 fn，期间 fd 2 指向临时文件，返回这段时间写到 stderr 的内容。
std::string CaptureStderr(const std::function<void()>& fn)
{
    std::fflush(stderr);
    char path[] = "/tmp/breeze_time_record_test_XXXXXX";
    const int tmp = ::mkstemp(path);
    CHECK(tmp >= 0);
    const int saved = ::dup(STDERR_FILENO);
    ::dup2(tmp, STDERR_FILENO);
    fn();
    std::fflush(stderr);
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
    ::lseek(tmp, 0, SEEK_SET);
    std::string out;
    char buf[4096];
    for (ssize_t n; (n = ::read(tmp, buf, sizeof(buf))) > 0;)
        out.append(buf, static_cast<std::size_t>(n));
    ::close(tmp);
    ::unlink(path);
    return out;
}

bool Contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

void TestStopwatch()
{
    TimeRecord t;
    CHECK(t.ElapsedNs() >= 0 && t.ElapsedUs() >= 0 && t.ElapsedMs() >= 0);
    std::this_thread::sleep_for(30ms);
    CHECK(t.ElapsedMs() >= 25 && t.ElapsedMs() < 2000);
    CHECK(t.ElapsedUs() >= 25000 && t.ElapsedNs() >= 25000000);
    CHECK(t.ElapsedUs() / 1000 >= t.ElapsedMs() - 1); // 各单位一致
    t.Restart();
    CHECK(t.ElapsedMs() < 25); // 重新开始
    std::this_thread::sleep_for(10ms);
    CHECK(t.ElapsedMs() >= 8);
}

// 不带名字：纯秒表，析构不记日志
void TestUnnamedIsSilent()
{
    const std::string out = CaptureStderr(
        []
        {
            TimeRecord t;
            std::this_thread::sleep_for(5ms);
            (void)t.ElapsedMs();
        });
    CHECK(out.empty());
}

void TestNamedLogsOnDestruction()
{
    const std::string out = CaptureStderr(
        []
        {
            TimeRecord t("LoadPlayer");
            std::this_thread::sleep_for(20ms);
        });
    CHECK(Contains(out, "timerecord: name:LoadPlayer elapsed "));
    CHECK(Contains(out, "us"));
    CHECK(Contains(out, "[info]"));
    CHECK(Contains(out, "time_record_test.cpp")); // 位置是创建 TimeRecord 的那一行，不是库里的
    // 耗时数值合理：>= 20000us
    const auto pos = out.find("elapsed ");
    CHECK(pos != std::string::npos);
    CHECK(std::atoll(out.c_str() + pos + 8) >= 18000);
}

// 慢操作阈值：没超过不记；超过记 Warn
void TestSlowThreshold()
{
    const std::string fast = CaptureStderr(
        []
        {
            TimeRecord t("Fast", 500ms);
            std::this_thread::sleep_for(5ms);
        });
    CHECK(fast.empty());

    const std::string slow = CaptureStderr(
        []
        {
            TimeRecord t("Slow", 10ms);
            std::this_thread::sleep_for(30ms);
        });
    CHECK(Contains(slow, "timerecord: name:Slow elapsed "));
    CHECK(Contains(slow, "[warning]"));
}

void TestCancelAndSetName()
{
    const std::string cancelled = CaptureStderr(
        []
        {
            TimeRecord t("NotWorthIt");
            t.Cancel();
        });
    CHECK(cancelled.empty());

    // 原版的 SetName 只改名字、不开日志；现在起了名字就会记
    const std::string named_later = CaptureStderr(
        []
        {
            TimeRecord t;
            t.SetName("Late");
        });
    CHECK(Contains(named_later, "timerecord: name:Late elapsed "));

    // 取消之后再起名字：又开了
    const std::string again = CaptureStderr(
        []
        {
            TimeRecord t("A");
            t.Cancel();
            t.SetName("B");
        });
    CHECK(Contains(again, "name:B") && !Contains(again, "name:A"));
}

void TestRestartAffectsLoggedValue()
{
    const std::string out = CaptureStderr(
        []
        {
            TimeRecord t("Restarted");
            std::this_thread::sleep_for(60ms);
            t.Restart();
        });
    const auto pos = out.find("elapsed ");
    CHECK(pos != std::string::npos);
    CHECK(std::atoll(out.c_str() + pos + 8) < 30000); // 记的是 Restart 之后的耗时
}

void TestMacro()
{
    const std::string out = CaptureStderr(
        []
        {
            TIME_RECORD("FromMacro");
            TIME_RECORD("Second"); // 同一作用域里两个，变量名不重名
            TIME_RECORD("Slow", 1000s);
        });
    CHECK(Contains(out, "name:FromMacro") && Contains(out, "name:Second"));
    CHECK(!Contains(out, "name:Slow")); // 没到阈值
}

// 不可拷贝、不可移动
static_assert(!std::is_copy_constructible_v<TimeRecord> && !std::is_move_constructible_v<TimeRecord>);
static_assert(!std::is_copy_assignable_v<TimeRecord> && !std::is_move_assignable_v<TimeRecord>);

} // namespace

int main()
{
    TestStopwatch();
    TestUnnamedIsSilent();
    TestNamedLogsOnDestruction();
    TestSlowThreshold();
    TestCancelAndSetName();
    TestRestartAffectsLoggedValue();
    TestMacro();
    std::puts("time_record_test: ok");
    return 0;
}
