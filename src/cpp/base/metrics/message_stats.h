// 按消息统计条数与字节数的诊断器：找出"哪种消息最话痨"。
// 逐条 Add，周期性 Tick 把报告写进日志（或交给自定义 sink）并清零。
//
//   MessageStats stats("gate-recv");        // 构造时是关的
//   stats.Enable();                         // GM / 运维命令打开；关着时 Add 只是一次分支
//   stats.Add(msg_id, payload.size());      // 按 id 统计；也可以按名字：stats.Add("PlayerMove", n)
//   stats.Tick(Now::SteadyMs());            // 主循环 / 定时器里周期调用，到点写一条 Info 日志并清零
//
// **非线程安全**：一个实例只给一个线程用（业务主循环一个、每个 IO 线程各一个），不要跨线程共享。
// 需要汇总多个线程时在上层各自 Snapshot 再合并；这里不加锁，热路径只有一次哈希查找。
//
// id 与名字是两套独立的键（id 5 与名字 "5" 不是同一项），报告里分开列。
// 每套键最多记 max_keys 个：之后出现的新键不再单独计，合计进 dropped；已记下的键照常累加。
// 消息 id / 名字若来自对端，这是对内存的上限保护；报告也只列前 report_top_n 项，不会一次刷出几千行日志。
//
// Tick 的 now_ms 用**单调时钟** Now::SteadyMs()：GM 拨时间不会让报告提前或迟迟不出。
// 统计窗口从第一次 Tick 起算（Enable 之后、或上一次上报之后），所以窗口长度是近似的，误差不超过两次 Tick 的间隔。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "flat_hash_map.h"

namespace breeze
{

struct MessageStatsOptions
{
    std::int64_t interval_ms = 60'000; // Tick 的上报周期；<= 0 表示每次 Tick 都上报
    std::size_t max_keys = 4096;       // 每套键（id / 名字）最多记多少项
    std::size_t report_top_n = 50;     // Tick 的报告里每套键列前几项；0 = 全列
};

class MessageStats
{
public:
    struct Counter
    {
        std::uint64_t count = 0;
        std::uint64_t bytes = 0;
    };

    // 排序：count 降序，再 bytes 降序，再键升序，所以同样的数据总是同样的顺序。
    struct Snapshot
    {
        struct ById
        {
            std::uint32_t id;
            Counter total;
        };
        struct ByName
        {
            std::string name;
            Counter total;
        };
        std::vector<ById> by_id;
        std::vector<ByName> by_name;
        Counter dropped; // 超过 max_keys 之后出现的新键的合计（id 与名字合在一起）
    };

    explicit MessageStats(std::string label, MessageStatsOptions options = {});

    // 构造时是关的。Disable 会清空数据。
    void Enable() noexcept { enabled_ = true; }
    void Disable();
    bool enabled() const noexcept { return enabled_; }

    // 记一条消息。统计关着时什么也不做。
    void Add(std::uint32_t id, std::size_t bytes)
    {
        if (!enabled_)
            return;
        auto it = by_id_.find(id);
        if (it == by_id_.end())
        {
            if (by_id_.size() >= options_.max_keys)
                return Drop(bytes);
            it = by_id_.try_emplace(id).first;
        }
        Bump(it->second, bytes);
    }
    void Add(std::string_view name, std::size_t bytes)
    {
        if (!enabled_)
            return;
        auto it = by_name_.find(name);
        if (it == by_name_.end())
        {
            if (by_name_.size() >= options_.max_keys)
                return Drop(bytes);
            it = by_name_.try_emplace(std::string(name)).first;
        }
        Bump(it->second, bytes);
    }

    // 清空数据（不改开关、不动计时窗口）。
    void Reset();

    Snapshot GetSnapshot() const;

    // 文本报告。top_n 限制每套键列几项（0 = 全列），多出来的折成一行 "... N more"；window_ms >= 0 时标题里带窗口长度。
    std::string Format(std::size_t top_n = 0, std::int64_t window_ms = -1) const;

    // 距上次上报满 interval_ms 就上报并清零，返回是否上报了。统计关着、或这一窗口里没有数据时不上报
    // （没数据的窗口也算过去了，下一窗口从现在起算）。sink 在清零之后才调用，里面再 Add 不会被冲掉。
    bool Tick(std::int64_t now_ms, const std::function<void(std::string_view)>& sink);
    // 报告写一条 Info 日志（默认 logger，没有就 stderr）。
    bool Tick(std::int64_t now_ms);

    const std::string& label() const noexcept { return label_; }

private:
    struct NameHash
    {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    };

    static void Bump(Counter& c, std::size_t bytes) noexcept
    {
        ++c.count;
        c.bytes += bytes;
    }
    void Drop(std::size_t bytes) noexcept { Bump(dropped_, bytes); }

    std::string label_;
    MessageStatsOptions options_;
    bool enabled_ = false;
    std::optional<std::int64_t> window_start_ms_; // 空 = 下一次 Tick 起算
    FlatHashMap<std::uint32_t, Counter> by_id_;
    FlatHashMap<std::string, Counter, NameHash, std::equal_to<>> by_name_;
    Counter dropped_;
};

} // namespace breeze
