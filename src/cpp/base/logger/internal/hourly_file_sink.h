// 按小时滚动的文件 sink，同时维护 <name>.log 软链。
//
// 滚动与清理一律以**消息自带的时间**为准，不读时钟 —— 单测可以直接喂未来的时间点。
// 文件在第一条消息（或 Open）到来时才创建，不是构造时。
//
// 文件名 <name>.<YYYY-MM-DD_HH>.log。时间串里只用 '-' 和 '_'：name 可含点号（gatesvr.res），
// 时间串再有点号就分不出哪段是名字；冒号在 Windows 上不能做文件名。
// 定长零填充，字符串比较即时间比较。
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
    HourlyFileSink(std::filesystem::path dir, std::string name, std::uint32_t retainHours)
        : dir_(std::move(dir))
        , name_(std::move(name))
        , retainHours_(retainHours)
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
        spdlog::memory_buf_t buf;
        this->formatter_->format(msg, buf);
        file_.write(buf);
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

    void Rotate(spdlog::log_clock::time_point tp)
    {
        const std::string file = std::format("{}.{}.log", name_, HourKey(tp));
        file_.open((dir_ / file).string(), false); // 追加：同小时重启不覆盖

        std::tm tm = spdlog::details::os::localtime(spdlog::log_clock::to_time_t(tp));
        tm.tm_min = 0;
        tm.tm_sec = 0;
        start_ = spdlog::log_clock::from_time_t(std::mktime(&tm));
        end_ = start_ + std::chrono::hours(1);

        UpdateSymlink(file);
        if (retainHours_ > 0)
            Cleanup(tp);
    }

    // 先建临时软链再 rename 覆盖：tail -F 之类的读者看不到"软链不存在"的空窗。
    void UpdateSymlink(const std::string& file)
    {
        const auto link = dir_ / (name_ + ".log");
        auto tmp = link;
        tmp += ".tmp";
        std::error_code ec;
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

    // 文件名里的小时串定长且零填充，字符串比较即时间比较。
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
            if (fn.size() != prefix.size() + kKeyLen + kSuffix.size() || !fn.starts_with(prefix) ||
                !fn.ends_with(kSuffix))
                continue;
            const std::string key = fn.substr(prefix.size(), kKeyLen);
            if (!IsHourKey(key))
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
    spdlog::details::file_helper file_;
    spdlog::log_clock::time_point start_{};
    spdlog::log_clock::time_point end_{};
    bool warned_ = false;
};

using HourlyFileSinkMt = HourlyFileSink<std::mutex>;

} // namespace breeze
