#include "message_stats.h"

#include <algorithm>
#include <format>
#include <tuple>
#include <utility>

#include "logger_macros.h"

namespace breeze
{

namespace
{

// 一套键的报告段：标题带键数与合计，之后列前 top_n 项。
template <typename Entry, typename Key>
void AppendSection(
    std::string& out, std::string_view title, const std::vector<Entry>& entries, std::size_t top_n, Key key)
{
    if (entries.empty())
        return;
    std::uint64_t msgs = 0, bytes = 0;
    for (const auto& e : entries)
    {
        msgs += e.total.count;
        bytes += e.total.bytes;
    }
    out += std::format("\n  {} ({} keys, {} msgs, {} bytes):", title, entries.size(), msgs, bytes);
    const std::size_t shown = top_n == 0 ? entries.size() : std::min(top_n, entries.size());
    for (std::size_t i = 0; i < shown; ++i)
        out +=
            std::format("\n    {}  {} msgs  {} bytes", key(entries[i]), entries[i].total.count, entries[i].total.bytes);
    if (shown < entries.size())
        out += std::format("\n    ... {} more", entries.size() - shown);
}

} // namespace

MessageStats::MessageStats(std::string label, MessageStatsOptions options)
    : label_(std::move(label))
    , options_(options)
{
}

void MessageStats::Disable()
{
    enabled_ = false;
    window_start_ms_.reset();
    Reset();
}

void MessageStats::Reset()
{
    by_id_.clear();
    by_name_.clear();
    dropped_ = {};
}

MessageStats::Snapshot MessageStats::GetSnapshot() const
{
    Snapshot s;
    s.by_id.reserve(by_id_.size());
    for (const auto& [id, c] : by_id_)
        s.by_id.push_back({id, c});
    s.by_name.reserve(by_name_.size());
    for (const auto& [name, c] : by_name_)
        s.by_name.push_back({name, c});
    // 先 count、再 bytes 都是降序，最后按键升序：同样的数据永远同样的顺序
    std::ranges::sort(
        s.by_id,
        [](const Snapshot::ById& a, const Snapshot::ById& b)
        { return std::tie(b.total.count, b.total.bytes, a.id) < std::tie(a.total.count, a.total.bytes, b.id); });
    std::ranges::sort(
        s.by_name,
        [](const Snapshot::ByName& a, const Snapshot::ByName& b)
        { return std::tie(b.total.count, b.total.bytes, a.name) < std::tie(a.total.count, a.total.bytes, b.name); });
    s.dropped = dropped_;
    return s;
}

std::string MessageStats::Format(std::size_t top_n, std::int64_t window_ms) const
{
    const Snapshot s = GetSnapshot();
    std::string out = std::format("msgstats[{}]", label_);
    if (window_ms >= 0)
        out += std::format(" window={}ms", window_ms);
    AppendSection(out, "by id", s.by_id, top_n, [](const Snapshot::ById& e) { return e.id; });
    AppendSection(
        out, "by name", s.by_name, top_n, [](const Snapshot::ByName& e) -> const std::string& { return e.name; });
    if (s.dropped.count != 0)
        out += std::format(
            "\n  dropped (max_keys={}): {} msgs  {} bytes", options_.max_keys, s.dropped.count, s.dropped.bytes);
    return out;
}

bool MessageStats::Tick(std::int64_t now_ms, const std::function<void(std::string_view)>& sink)
{
    if (!enabled_)
        return false;
    if (!window_start_ms_)
    {
        window_start_ms_ = now_ms;
        return false;
    }
    const std::int64_t elapsed = now_ms - *window_start_ms_;
    if (options_.interval_ms > 0 && elapsed < options_.interval_ms)
        return false;
    window_start_ms_ = now_ms;
    if (by_id_.empty() && by_name_.empty() && dropped_.count == 0)
        return false;
    std::string report = Format(options_.report_top_n, elapsed);
    Reset();
    sink(report);
    return true;
}

bool MessageStats::Tick(std::int64_t now_ms)
{
    return Tick(now_ms, [](std::string_view report) { LOG_INFO("{}", report); });
}

} // namespace breeze
