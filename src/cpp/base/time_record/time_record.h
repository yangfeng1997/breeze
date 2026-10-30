// 耗时记录：一个单调时钟的秒表，可选地在析构时把耗时写进日志。
//
//   {
//       TimeRecord t("LoadPlayer");          // 作用域结束时记一条 Info：timerecord: name:LoadPlayer elapsed 1532us
//       LoadPlayer(id);
//   }
//   TIME_RECORD("Settle");                   // 同上，变量名自动生成
//
//   TimeRecord slow("RunSql", 50ms);         // 只在超过 50ms 时记一条 Warn（慢操作日志），平时不出声
//
//   TimeRecord sw;                           // 不带名字：纯秒表，不记日志
//   ...; auto us = sw.ElapsedUs();
//
// 用的是 steady_clock，不受系统时间调整与 Now 的 GM 模拟影响。日志走默认 logger（没有就写 stderr），
// 位置记的是**创建 TimeRecord 的那一行**（source_location），不是析构的地方。
// 不可拷贝、不可移动（析构时才记录，拷出去会记两次）。

#pragma once

#include <chrono>
#include <cstdint>
#include <source_location>
#include <string>

namespace breeze
{

class TimeRecord
{
public:
    using Clock = std::chrono::steady_clock;

    // 纯秒表：不记日志（之后 SetName 可以让它记）。
    TimeRecord() noexcept
        : start_(Clock::now())
    {
    }

    // 带名字：析构时记日志。slow_threshold 为 0（默认）= 总是记一条 Info；大于 0 = 只在耗时 >= 阈值时记一条 Warn。
    explicit TimeRecord(std::string name,
                        std::chrono::microseconds slow_threshold = std::chrono::microseconds::zero(),
                        std::source_location loc = std::source_location::current());

    TimeRecord(const TimeRecord&) = delete;
    TimeRecord& operator=(const TimeRecord&) = delete;

    ~TimeRecord();

    // 重新开始计时（日志里记的是最后一次 Restart 以来的耗时）。
    void Restart() noexcept { start_ = Clock::now(); }

    std::int64_t ElapsedNs() const noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_).count();
    }
    std::int64_t ElapsedUs() const noexcept
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start_).count();
    }
    std::int64_t ElapsedMs() const noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start_).count();
    }

    // 给它起名字，并让它在析构时记日志（原版只改名字、不开日志，起了名字也不会记）。
    void SetName(std::string name)
    {
        name_ = std::move(name);
        enabled_ = true;
    }
    // 析构时不再记日志（比如发现这次不值得记）。
    void Cancel() noexcept { enabled_ = false; }

private:
    Clock::time_point start_;
    std::string name_;
    std::chrono::microseconds slow_threshold_ = std::chrono::microseconds::zero();
    std::source_location loc_;
    bool enabled_ = false;
};

} // namespace breeze

#define BREEZE_TIME_RECORD_CAT_(a, b) a##b
#define BREEZE_TIME_RECORD_NAME_(n) BREEZE_TIME_RECORD_CAT_(breeze_time_record_, n)
// 在当前作用域里创建一个带名字的 TimeRecord，作用域结束时记日志。
#define TIME_RECORD(...) ::breeze::TimeRecord BREEZE_TIME_RECORD_NAME_(__COUNTER__)(__VA_ARGS__)
