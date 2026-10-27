// 命令行解析：CLI11 的薄封装。
//
// CLI11 只出现在 args_parser.cpp，本头不含它 —— 换库只改那一个文件，
// 业务代码和链接它的目标都不受影响。
//
// 只覆盖 "--key value" 型选项与布尔开关。要子命令、位置参数等，先评估是否值得扩这层，
// 否则直接在该进程的 main 里用 CLI11。

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace breeze
{

inline constexpr bool kRequired = true;

// proceed 为 false 时调用方应直接 `return exitCode`：
// --help 已打印帮助、exitCode 为 0；参数错误已向 stderr 打印原因、exitCode 非 0。
struct ParseResult
{
    bool proceed;
    int exitCode;
};

class ArgsParser
{
public:
    explicit ArgsParser(std::string description);
    ~ArgsParser();
    ArgsParser(const ArgsParser&) = delete;
    ArgsParser& operator=(const ArgsParser&) = delete;

    // name 形如 "--config" 或 "-c,--config"。out 的初值即默认值，且必须活过 Parse。
    // 重名抛 std::logic_error（编程错误，与 App::RegisterModule 同）。
    void Add(std::string_view name, std::string& out, std::string_view desc, bool required = false);
    void Add(std::string_view name, int& out, std::string_view desc, bool required = false);
    void Add(std::string_view name, std::int64_t& out, std::string_view desc, bool required = false);
    void Add(std::string_view name, std::uint32_t& out, std::string_view desc, bool required = false);
    void Add(std::string_view name, double& out, std::string_view desc, bool required = false);

    // 出现即置 true；不出现保持 out 原值。
    void AddFlag(std::string_view name, bool& out, std::string_view desc);

    // 不抛异常：解析失败、--help 都经 ParseResult 返回。
    ParseResult Parse(int argc, const char* const* argv);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace breeze
