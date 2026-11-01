// 鲁棒性：队列溢出的两种策略、多后台线程、多线程压力、Shutdown 与生产者并发、滚动并发。
// 压力只跑到"几秒内"；长时间运行留给线上验证。无测试框架依赖：失败即 abort。

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "base/logger/internal/hourly_file_sink.h"
#include "base/logger/internal/logger_test_util.h"
#include "base/logger/logger.h"
#include "base/logger/logger_macros.h"

using namespace breeze;
using namespace breeze::testutil;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace
{

LoggerConfig FileCfg(const fs::path& dir, bool async = true)
{
    LoggerConfig c;
    c.file.dir = dir.string();
    c.name = "svc";
    c.level = Level::Info;
    c.async.enabled = async;
    return c;
}

std::vector<std::string> Lines(const std::string& text)
{
    std::vector<std::string> v;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);)
        v.push_back(std::move(l));
    return v;
}

// 日志行的最后一个空格分隔的词。
std::string LastToken(const std::string& line) { return line.substr(line.rfind(' ') + 1); }

// 一行完整的日志：以 [ 开头（时间戳），最后一个词以 prefix 开头。被截断或交错写入的行过不了。
bool Complete(const std::string& line, std::string_view prefix)
{
    return line.size() > 20 && line[0] == '[' && LastToken(line).starts_with(prefix);
}

// 把 stdout 接到一根暂时没人读的管道：后台线程写满 64KB 后阻塞，能稳定制造"队列堵住"。
class StdoutPipe
{
public:
    StdoutPipe()
    {
        std::fflush(stdout);
        int fds[2];
        CHECK(::pipe(fds) == 0);
        rd_ = fds[0];
        saved_ = ::dup(STDOUT_FILENO);
        ::dup2(fds[1], STDOUT_FILENO);
        ::close(fds[1]);
    }
    ~StdoutPipe()
    {
        Restore();
        ::close(rd_);
    }
    // 恢复 stdout，同时关掉管道写端，读者才会读到 EOF。必须在 Logger 销毁（排空）之后调。
    void Restore()
    {
        if (saved_ < 0)
            return;
        std::fflush(stdout);
        ::dup2(saved_, STDOUT_FILENO);
        ::close(saved_);
        saved_ = -1;
    }
    std::string ReadToEof()
    {
        std::string out;
        char buf[8192];
        for (ssize_t n; (n = ::read(rd_, buf, sizeof buf)) > 0;)
            out.append(buf, static_cast<std::size_t>(n));
        return out;
    }

private:
    int rd_ = -1;
    int saved_ = -1;
};

LoggerConfig PipeCfg(Overflow overflow)
{
    LoggerConfig c;
    c.output = Output::Stdout;
    c.name = "pipe";
    c.level = Level::Info;
    c.async.enabled = true;
    c.async.queueSize = 16;
    c.async.overflow = overflow;
    c.flush.level = Level::Info; // 每条都刷：缓冲不会替管道挡住背压
    c.flush.intervalSec = 0;
    return c;
}

constexpr int kPipeN = 5000;

// 发 kPipeN 条约 150 字节的日志，足以把 64KB 管道撑满。
void ProduceToPipe(Logger& lg)
{
    const std::string pad(80, 'x');
    for (int i = 0; i < kPipeN; ++i)
        BREEZE_LOG_INFO(lg, "msg-{} {}", i, pad);
}

std::size_t Count(const std::string& text, std::string_view needle)
{
    std::size_t n = 0;
    for (auto pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size()))
        ++n;
    return n;
}

// Block：队列满了生产者被堵住（不丢），读者开始读之后全部到齐。
void TestOverflowBlockStallsButLosesNothing()
{
    StdoutPipe pipe;
    std::string err;
    auto lg = Logger::Create(PipeCfg(Overflow::Block), err);
    CHECK(lg);

    auto producer = std::async(std::launch::async, [&] { ProduceToPipe(*lg); });
    CHECK(producer.wait_for(300ms) == std::future_status::timeout); // 被堵住了

    std::string out;
    std::thread reader([&] { out = pipe.ReadToEof(); });
    CHECK(producer.wait_for(20s) == std::future_status::ready);
    producer.get();
    lg.reset(); // 排空
    pipe.Restore();
    reader.join();

    CHECK(Count(out, "msg-") == kPipeN);
    CHECK(Has(out, "msg-4999 "));
}

// DropOldest：生产者永不被堵住，代价是丢旧的；最新的保住。
void TestOverflowDropOldestNeverBlocksAndKeepsNewest()
{
    StdoutPipe pipe;
    std::string err;
    auto lg = Logger::Create(PipeCfg(Overflow::DropOldest), err);
    CHECK(lg);

    auto producer = std::async(std::launch::async, [&] { ProduceToPipe(*lg); });
    CHECK(producer.wait_for(10s) == std::future_status::ready); // 后台线程被管道卡着，生产者照样发完
    producer.get();

    std::string out;
    std::thread reader([&] { out = pipe.ReadToEof(); });
    lg.reset();
    pipe.Restore();
    reader.join();

    const auto n = Count(out, "msg-");
    CHECK(n > 0);
    CHECK(n < static_cast<std::size_t>(kPipeN)); // 确实丢了
    CHECK(Has(out, "msg-4999 "));                // 最新的在
}

// threads > 1：多个后台线程并发写同一个 logger，每条完整、不丢、不重。
void TestMultipleWorkerThreads()
{
    const auto dir = TempDir("workers");
    auto c = FileCfg(dir);
    c.async.threads = 4;
    c.async.queueSize = 1 << 16;
    std::string err;
    auto lg = Logger::Create(c, err);
    CHECK(lg);

    constexpr int kProducers = 4, kPer = 5000;
    std::vector<std::thread> ts;
    for (int t = 0; t < kProducers; ++t)
        ts.emplace_back(
            [&, t]
            {
                for (int i = 0; i < kPer; ++i)
                    BREEZE_LOG_INFO(lg, "p{}-{}", t, i);
            });
    for (auto& th : ts)
        th.join();
    lg.reset();

    std::unordered_set<std::string> seen;
    for (const auto& l : Lines(ReadAll(dir / "svc.log")))
    {
        CHECK(Complete(l, "p"));
        seen.insert(LastToken(l));
    }
    CHECK(seen.size() == static_cast<std::size_t>(kProducers * kPer)); // 不丢不重（多线程下不保证顺序，不检查）
    fs::remove_all(dir);
}

// 压力：多生产者同时写，另一个线程不停改级别、改 flush 级别、Flush。
// 每个生产者写两种日志：WARN（Info / Warn 两档下都不会被过滤，所以必须一条不少）和 INFO（随级别来回切，
// 丢是预期的）。注意被过滤的调用只要零点几纳秒，只写 INFO 的话生产者几微秒就跑完，什么都没测到。
void TestConcurrentStress()
{
    const auto dir = TempDir("stress");
    auto c = FileCfg(dir);
    c.async.threads = 2;
    c.async.queueSize = 1 << 17;
    std::string err;
    auto lg = Logger::Create(c, err);
    CHECK(lg);

    constexpr int kProducers = 8, kPer = 20000;
    std::atomic<bool> done{false};
    std::thread chaos(
        [&]
        {
            for (int i = 0; !done.load(); ++i)
            {
                lg->SetLevel(i % 2 ? Level::Info : Level::Warn);
                lg->SetFlushLevel(i % 3 ? Level::Warn : Level::Info);
                lg->Flush();
                std::this_thread::sleep_for(50us);
            }
        });
    std::vector<std::thread> ts;
    for (int t = 0; t < kProducers; ++t)
        ts.emplace_back(
            [&, t]
            {
                for (int i = 0; i < kPer; ++i)
                {
                    BREEZE_LOG_WARN(lg, "w-{}-{}", t, i);
                    BREEZE_LOG_INFO(lg, "i-{}-{}", t, i);
                }
            });
    for (auto& th : ts)
        th.join();
    done.store(true);
    chaos.join();
    lg.reset();

    std::unordered_set<std::string> warns;
    std::size_t infos = 0;
    for (const auto& l : Lines(ReadAll(dir / "svc.log")))
    {
        if (Complete(l, "w-"))
            warns.insert(LastToken(l));
        else if (Complete(l, "i-"))
            ++infos;
        else
            CHECK(false); // 撕裂 / 交错的行
    }
    CHECK(warns.size() == static_cast<std::size_t>(kProducers * kPer)); // WARN 一条不少、不重
    CHECK(infos <= static_cast<std::size_t>(kProducers * kPer));
    fs::remove_all(dir);
}

// Shutdown 与生产者并发：晚到的日志转 stderr，不崩、不死锁、文件里没有撕裂行。
void TestShutdownWhileProducersRunning()
{
    const auto dir = TempDir("shutdown_race");
    fs::create_directories(dir);
    std::string err;
    auto lg = Logger::Create(FileCfg(dir / "logs"), err);
    CHECK(lg);

    // Shutdown 窗口里 spdlog 的 error handler 可能往 stderr 打几行，收进文件免得刷屏。
    int savedErr = ::dup(STDERR_FILENO);
    int fd = ::open((dir / "err.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ::dup2(fd, STDERR_FILENO);
    ::close(fd);

    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < 6; ++t)
        ts.emplace_back(
            [&, t]
            {
                for (int i = 0; !stop.load(); ++i)
                    BREEZE_LOG_INFO(lg, "r-{}-{}", t, i);
            });
    std::this_thread::sleep_for(30ms);
    lg->Shutdown();
    std::this_thread::sleep_for(30ms);
    stop.store(true);
    for (auto& th : ts)
        th.join();
    lg.reset();

    std::fflush(stderr);
    ::dup2(savedErr, STDERR_FILENO);
    ::close(savedErr);

    const auto lines = Lines(ReadAll(dir / "logs" / "svc.log"));
    CHECK(!lines.empty());
    for (const auto& l : lines)
        CHECK(Complete(l, "r-"));
    CHECK(Has(ReadAll(dir / "err.txt"), "r-")); // 关闭之后的日志转到了 stderr
    fs::remove_all(dir);
}

// 滚动并发：多个线程喂跨越五百多个小时的时间戳，每条恰好落进某个小时文件一次，没有撕裂。
void TestSinkConcurrentRotation()
{
    const auto dir = TempDir("rotation");
    fs::create_directories(dir);
    HourlyFileSinkMt sink(dir, "r", 0);

    std::tm tm{};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 9;
    tm.tm_mday = 1;
    tm.tm_isdst = -1;
    const auto base = spdlog::log_clock::from_time_t(std::mktime(&tm));

    constexpr int kThreads = 4, kPer = 5000;
    std::atomic<int> seq{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t)
        ts.emplace_back(
            [&]
            {
                for (int i = 0; i < kPer; ++i)
                {
                    const int k = seq.fetch_add(1);
                    const std::string payload = std::format("m-{}", k);
                    spdlog::details::log_msg msg(
                        spdlog::source_loc{}, "x", spdlog::level::info, spdlog::string_view_t(payload));
                    msg.time = base + std::chrono::seconds(90) * k; // 2 万条 × 90 秒 ≈ 500 小时
                    sink.log(msg);
                }
            });
    for (auto& th : ts)
        th.join();
    sink.flush();

    std::unordered_set<std::string> seen;
    std::size_t files = 0, total = 0;
    for (const auto& e : fs::directory_iterator(dir))
    {
        const auto name = e.path().filename().string();
        if (name == "r.log" || !name.starts_with("r.") || e.is_symlink())
            continue;
        ++files;
        for (const auto& l : Lines(ReadAll(e.path())))
        {
            ++total;
            CHECK(Complete(l, "m-")); // 默认格式 "[时间] [name] [级别] 消息"，最后一个词是消息
            seen.insert(LastToken(l));
        }
    }
    CHECK(total == static_cast<std::size_t>(kThreads * kPer)); // 一条不多不少
    CHECK(seen.size() == total);                               // 没有重复
    CHECK(files > 100);                                        // 确实滚了很多次
    fs::remove_all(dir);
}

} // namespace

int main()
{
    TestOverflowBlockStallsButLosesNothing();
    TestOverflowDropOldestNeverBlocksAndKeepsNewest();
    TestMultipleWorkerThreads();
    TestConcurrentStress();
    TestShutdownWhileProducersRunning();
    TestSinkConcurrentRotation();
    std::puts("logger_robust_test: ok");
}
