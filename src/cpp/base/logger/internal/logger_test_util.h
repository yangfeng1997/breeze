// 仅测试用的小工具。无测试框架依赖：CHECK 失败即 abort。
// 放在 internal/：PRIVATE 头文件路径，只有 breeze_logger 自己的测试看得到。

#pragma once

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

namespace breeze::testutil
{

inline std::filesystem::path TempDir(const char* tag)
{
    auto p = std::filesystem::temp_directory_path() / std::format("breeze_logger_test_{}_{}", ::getpid(), tag);
    std::filesystem::remove_all(p);
    return p;
}

inline std::string ReadAll(const std::filesystem::path& p)
{
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline bool Has(const std::string& s, std::string_view needle) { return s.find(needle) != std::string::npos; }

} // namespace breeze::testutil
