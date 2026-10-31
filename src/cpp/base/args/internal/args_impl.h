// ArgsParser::Impl 的定义，含 CLI11 头文件。
//
// **只许 base/args 内部包含**：CMake 里本目录是 PRIVATE 头文件路径，使用方 include 不到。
// 一旦挪进公开目录（base/args/），CLI11 会泄漏给所有链接 breeze_args 的目标。

#pragma once

#include <CLI/CLI.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

#include "base/args/args_parser.h"

namespace breeze
{

struct ArgsParser::Impl
{
    explicit Impl(std::string description)
        : cli(std::move(description))
    {
    }
    CLI::App cli;

    template <class T>
    void Add(std::string_view name, T& out, std::string_view desc, bool required)
    {
        try
        {
            auto* opt = cli.add_option(std::string(name), out, std::string(desc));
            opt->capture_default_str();
            if (required)
                opt->required();
        }
        catch (const CLI::Error& e)
        {
            throw std::logic_error(e.what());
        }
    }
};

} // namespace breeze
