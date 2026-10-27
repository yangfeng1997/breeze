// 按小时滚动、小时内按大小分段的文件 sink，同时维护 <name>.log 软链。
//
// 滚动与清理一律以**消息自带的时间**为准，不读时钟 —— 单测可以直接喂未来的时间点。
// 文件在第一条消息（或 Open）到来时才创建，不是构造时。
//
// 文件名 <name>.<YYYY-MM-DD_HH>.log。时间串里只用 '-' 和 '_'：name 可含点号（gatesvr.res），
// 时间串再有点号就分不出哪段是名字；冒号在 Windows 上不能做文件名。
// 定长零填充，字符串比较即时间比较。
//
// 大小上限（maxFileBytes > 0 时）：一个小时的第 0 段是 <name>.<hour>.log，写满后顺延为
// <name>.<hour>.1.log、.2.log …，最多 maxFilesPerHour 段（0 不限）。段数用完后该小时剩余日志**丢弃**，
// 并在最后一段留一条说明，下个小时恢复，并在新文件里记上丢了多少条。软链始终指向当前段。
// 总量上界 = 保留小时数 × maxFilesPerHour × maxFileBytes（外加几行说明）。
// 同一小时内重启：从第 0 段起找第一个还放得下的段，不覆盖。
//
// **只许 base/logger 内部包含**（含 spdlog 头）。

#pragma once

#include <spdlog/details/file_helper.h>
#include <spdlog/details/os.h>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>

namespace breeze
{

template <class Mutex>
class HourlyFileSink final : public spdlog::sinks::base_sink<Mutex>
{
public:
    // maxFileBytes / maxFilesPerHour 为 0 表示不限。
    HourlyFileSink(std::filesystem::path dir,
                   std::string name,
                   std::uint32_t retainHours,
                   std::uint64_t maxFileBytes = 0,
                   std::uint32_t maxFilesPerHour = 0)
        : dir_(std::move(dir))
        , name_(std::move(name))
        , retainHours_(retainHours)
        , maxFileBytes_(maxFileBytes)
        , maxFilesPerHour_(maxFilesPerHour)
    {
    }

    // 提前打开 now 所在小时的文件：目录不可写之类的错误在 Create 里同步抛出，
    // 而不是等到异步线程里被 error handler 吞成一行 stderr。
    void Open(spdlog::log_clock::time_point now)
    {
        std::lock_guard<Mutex> lk(this->mutex_);
        Rotate(now);
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        // 时间倒退（校时）也重开：否则会把早一小时的日志写进当前文件。
        if (msg.time >= end_ || msg.time < start_)
            Rotate(msg.time);
        if (capped_)
        {
            ++dropped_;
            return;
        }

        spdlog::memory_buf_t buf;
        this->formatter_->format(msg, buf);
        while (!Fits(buf.size()))
        {
            if (!NextSegment())
            {
                Cap();
                ++dropped_;
                return;
            }
        }
        file_.write(buf);
        bytes_ += buf.size();
    }

    void flush_() override { file_.flush(); }

private:
    static std::string HourKey(spdlog::log_clock::time_point tp)
    {
        const std::tm tm = spdlog::details::os::localtime(spdlog::log_clock::to_time_t(tp));
        return std::format("{:04}-{:02}-{:02}_{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour);
    }

    // 严格匹配 HourKey 的输出：YYYY-MM-DD_HH。旧格式（纯数字）与其它写法都不认，宁可漏删也不误删。
    static bool IsHourKey(std::string_view k)
    {
        if (k.size() != 13)
            return false;
        for (std::size_t i = 0; i < k.size(); ++i)
        {
            const bool sep = i == 4 || i == 7 || i == 10;
            const char want = i == 10 ? '_' : '-';
            if (sep ? k[i] != want : (k[i] < '0' || k[i] > '9'))
                return false;
        }
        return true;
    }

    // 段号后缀：".<1 位以上数字>"。其它（".x"、".1.2"、"."）都不是，宁可漏删也不误删。
    static bool IsSegmentSuffix(std::string_view t)
    {
        if (t.size() < 2 || t[0] != '.')
            return false;
        return std::all_of(t.begin() + 1, t.end(), [](char c) { return c >= '0' && c <= '9'; });
    }

    std::string SegmentFile() const
    {
        return segment_ == 0 ? std::format("{}.{}.log", name_, hourKey_)
                             : std::format("{}.{}.{}.log", name_, hourKey_, segment_);
    }

    // 打开当前段（追加：同小时重启不覆盖），记下已有大小，软链指过去。
    void OpenSegment()
    {
        const std::string file = SegmentFile();
        file_.open((dir_ / file).string(), false);
        bytes_ = file_.size();
        UpdateSymlink(file);
    }

    // 空文件不拒绝：单条日志本身超过上限也得写进去，否则会死循环换段。
    bool Fits(std::size_t n) const { return maxFileBytes_ == 0 || bytes_ == 0 || bytes_ + n <= maxFileBytes_; }

    bool NextSegment()
    {
        if (maxFilesPerHour_ > 0 && segment_ + 1 >= maxFilesPerHour_)
            return false;
        ++segment_;
        OpenSegment();
        return true;
    }

    // 说明行直接写，不经格式化器：它是 sink 自己的话，不是某个 logger 的日志。
    void WriteNote(const std::string& text)
    {
        const std::string line = "[logger] " + text + "\n";
        spdlog::memory_buf_t b;
        b.append(line.data(), line.data() + line.size());
        file_.write(b);
        bytes_ += line.size();
    }

    void Cap()
    {
        capped_ = true;
        WriteNote(std::format("本小时日志量超限（单文件 {} 字节 x {} 个），后续日志被丢弃，下个小时恢复",
                              maxFileBytes_,
                              maxFilesPerHour_));
    }

    void Rotate(spdlog::log_clock::time_point tp)
    {
        const auto droppedPrev = dropped_;
        capped_ = false;
        dropped_ = 0;
        hourKey_ = HourKey(tp);
        segment_ = 0;
        OpenSegment();

        std::tm tm = spdlog::details::os::localtime(spdlog::log_clock::to_time_t(tp));
        tm.tm_min = 0;
        tm.tm_sec = 0;
        start_ = spdlog::log_clock::from_time_t(std::mktime(&tm));
        end_ = start_ + std::chrono::hours(1);

        if (droppedPrev > 0)
            WriteNote(std::format("上一个小时因超限共丢弃 {} 条日志", droppedPrev));
        if (retainHours_ > 0)
            Cleanup(tp);
    }

    // 先建临时软链再 rename 覆盖：tail -F 之类的读者看不到"软链不存在"的空窗。
    //
    // <name>.log 已存在时按类型处理：
    //   软链（有效或悬空） 直接被覆盖。
    //   普通文件           旧版本遗留的同名日志：改名 .old 保留。rename 会直接覆盖它，等于悄悄删掉。
    //   目录               没法替换，只告警，小时文件照写，没有软链。
    void UpdateSymlink(const std::string& file)
    {
        const auto link = dir_ / (name_ + ".log");
        auto tmp = link;
        tmp += ".tmp";
        std::error_code ec;

        const auto type = std::filesystem::symlink_status(link, ec).type(); // 不依赖 ec：不存在时各实现不一
        if (type == std::filesystem::file_type::directory)
        {
            std::fprintf(stderr, "[logger] %s 是目录，无法建软链，小时文件照常写入\n", link.string().c_str());
            return;
        }
        if (type != std::filesystem::file_type::not_found && type != std::filesystem::file_type::symlink)
        {
            const auto old = link.string() + ".old";
            std::filesystem::remove(old, ec);
            std::filesystem::rename(link, old, ec);
            if (ec)
            {
                std::fprintf(stderr, "[logger] 旧日志 %s 改名失败: %s\n", link.string().c_str(), ec.message().c_str());
                return;
            }
            std::fprintf(stderr, "[logger] %s 原是普通文件，已改名为 %s\n", link.string().c_str(), old.c_str());
        }

        std::filesystem::remove(tmp, ec);
        std::filesystem::create_symlink(file, tmp, ec); // 相对目标：整个目录搬走软链仍有效
        if (!ec)
            std::filesystem::rename(tmp, link, ec);
        if (ec && !warned_)
        {
            warned_ = true;
            std::fprintf(stderr, "[logger] 更新软链失败 %s: %s\n", link.string().c_str(), ec.message().c_str());
        }
    }

    // 只认 <name>.<YYYY-MM-DD_HH>.log 与 <name>.<YYYY-MM-DD_HH>.<段号>.log，早于截止小时的删掉
    // （含它的所有段）。小时串定长且零填充，字符串比较即时间比较。
    void Cleanup(spdlog::log_clock::time_point now)
    {
        const std::string cutoff = HourKey(now - std::chrono::hours(retainHours_));
        const std::string prefix = name_ + ".";
        constexpr std::string_view kSuffix = ".log";
        constexpr std::size_t kKeyLen = 13; // YYYY-MM-DD_HH

        std::error_code ec;
        for (std::filesystem::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string fn = it->path().filename().string();
            if (!fn.starts_with(prefix) || !fn.ends_with(kSuffix))
                continue;
            const std::string mid = fn.substr(prefix.size(), fn.size() - prefix.size() - kSuffix.size());
            if (mid.size() < kKeyLen)
                continue;
            const std::string key = mid.substr(0, kKeyLen);
            const std::string tail = mid.substr(kKeyLen);
            if (!IsHourKey(key) || (!tail.empty() && !IsSegmentSuffix(tail)))
                continue;
            if (key < cutoff)
            {
                std::error_code rmEc;
                std::filesystem::remove(it->path(), rmEc);
            }
        }
    }

    std::filesystem::path dir_;
    std::string name_;
    std::uint32_t retainHours_;
    std::uint64_t maxFileBytes_;
    std::uint32_t maxFilesPerHour_;

    spdlog::details::file_helper file_;
    spdlog::log_clock::time_point start_{};
    spdlog::log_clock::time_point end_{};
    std::string hourKey_;
    std::uint32_t segment_ = 0;
    std::uint64_t bytes_ = 0;
    bool capped_ = false;
    std::uint64_t dropped_ = 0;
    bool warned_ = false;
};

using HourlyFileSinkMt = HourlyFileSink<std::mutex>;

} // namespace breeze
