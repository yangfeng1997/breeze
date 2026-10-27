// 时区与夏令时：滚动按**本地**小时。TZ 环境变量只能在没有其它线程时改，所以单独一个进程，
// 只测 sink，不起任何 Logger 线程。无测试框架依赖：失败即 abort。

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>

#include "hourly_file_sink.h"
#include "logger_test_util.h"

using namespace breeze;
using namespace breeze::testutil;
namespace fs = std::filesystem;

namespace
{

// 设 TZ，析构时还原。
class TzGuard
{
public:
    explicit TzGuard(const char* tz)
    {
        if (const char* old = std::getenv("TZ"))
            old_ = old, had_ = true;
        ::setenv("TZ", tz, 1);
        ::tzset();
    }
    ~TzGuard()
    {
        if (had_)
            ::setenv("TZ", old_.c_str(), 1);
        else
            ::unsetenv("TZ");
        ::tzset();
    }

private:
    std::string old_;
    bool had_ = false;
};

// 以 UTC 给出时刻，与 TZ 无关。
spdlog::log_clock::time_point Utc(int y, int mo, int d, int h, int mi)
{
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    return spdlog::log_clock::from_time_t(::timegm(&tm));
}

void Feed(HourlyFileSinkMt& sink, spdlog::log_clock::time_point t, std::string_view text)
{
    spdlog::details::log_msg msg(spdlog::source_loc{}, "x", spdlog::level::info, spdlog::string_view_t(text));
    msg.time = t;
    sink.log(msg);
    sink.flush();
}

// 美东 2026-03-08 02:00 EST 直接跳到 03:00 EDT：本地没有 02 点，不该有 _02 文件，也不该丢日志。
void TestSpringForward()
{
    TzGuard tz("America/New_York");
    const auto dir = TempDir("tz_spring");
    fs::create_directories(dir);
    {
        HourlyFileSinkMt sink(dir, "t", 0);
        Feed(sink, Utc(2026, 3, 8, 6, 30), "a"); // 01:30 EST
        Feed(sink, Utc(2026, 3, 8, 6, 59), "b"); // 01:59 EST
        Feed(sink, Utc(2026, 3, 8, 7, 0), "c");  // 03:00 EDT
        Feed(sink, Utc(2026, 3, 8, 7, 30), "d"); // 03:30 EDT
    }
    CHECK(Has(ReadAll(dir / "t.2026-03-08_01.log"), "a"));
    CHECK(Has(ReadAll(dir / "t.2026-03-08_01.log"), "b"));
    CHECK(!fs::exists(dir / "t.2026-03-08_02.log"));
    CHECK(Has(ReadAll(dir / "t.2026-03-08_03.log"), "c"));
    CHECK(Has(ReadAll(dir / "t.2026-03-08_03.log"), "d"));
    CHECK(fs::read_symlink(dir / "t.log") == "t.2026-03-08_03.log");
    fs::remove_all(dir);
}

// 美东 2026-11-01 02:00 EDT 回拨到 01:00 EST：01 点出现两次。两段日志都进 _01 文件（追加），不丢。
void TestFallBack()
{
    TzGuard tz("America/New_York");
    const auto dir = TempDir("tz_fall");
    fs::create_directories(dir);
    {
        HourlyFileSinkMt sink(dir, "t", 0);
        Feed(sink, Utc(2026, 11, 1, 5, 30), "first");  // 01:30 EDT
        Feed(sink, Utc(2026, 11, 1, 6, 30), "second"); // 01:30 EST（第二次 01 点）
        Feed(sink, Utc(2026, 11, 1, 7, 0), "third");   // 02:00 EST
    }
    const auto h01 = ReadAll(dir / "t.2026-11-01_01.log");
    CHECK(Has(h01, "first"));
    CHECK(Has(h01, "second")); // 同名文件追加，没有覆盖
    CHECK(Has(ReadAll(dir / "t.2026-11-01_02.log"), "third"));
    CHECK(fs::read_symlink(dir / "t.log") == "t.2026-11-01_02.log");
    fs::remove_all(dir);
}

// 偏移 +5:30 的时区：按本地整点滚动，不是按 UTC 整点。
void TestHalfHourOffsetZone()
{
    TzGuard tz("Asia/Kolkata");
    const auto dir = TempDir("tz_india");
    fs::create_directories(dir);
    {
        HourlyFileSinkMt sink(dir, "t", 0);
        Feed(sink, Utc(2026, 10, 26, 18, 29), "before"); // 23:59 IST
        Feed(sink, Utc(2026, 10, 26, 18, 30), "after");  // 次日 00:00 IST
    }
    CHECK(Has(ReadAll(dir / "t.2026-10-26_23.log"), "before"));
    CHECK(Has(ReadAll(dir / "t.2026-10-27_00.log"), "after"));
    fs::remove_all(dir);
}

// 清理的截止时刻也按本地时间换算，跨夏令时切换不误删、不漏删。
void TestRetentionAcrossDst()
{
    TzGuard tz("America/New_York");
    const auto dir = TempDir("tz_retain");
    fs::create_directories(dir);
    for (const char* f : {"t.2026-03-07_00.log", "t.2026-03-07_23.log", "t.2026-03-08_12.log"})
        std::ofstream(dir / f) << "x";
    {
        HourlyFileSinkMt sink(dir, "t", 24);       // 保留 24 小时
        Feed(sink, Utc(2026, 3, 8, 18, 0), "now"); // 14:00 EDT，往前 24 小时 = 03-07 14:00 EST
    }
    CHECK(!fs::exists(dir / "t.2026-03-07_00.log")); // 早于截止
    CHECK(fs::exists(dir / "t.2026-03-07_23.log"));  // 晚于截止
    CHECK(fs::exists(dir / "t.2026-03-08_12.log"));
    fs::remove_all(dir);
}

} // namespace

int main()
{
    if (!fs::exists("/usr/share/zoneinfo/America/New_York"))
    {
        std::puts("logger_tz_test: skipped（没有 tzdata）");
        return 0;
    }
    TestSpringForward();
    TestFallBack();
    TestHalfHourOffsetZone();
    TestRetentionAcrossDst();
    std::puts("logger_tz_test: ok");
}
