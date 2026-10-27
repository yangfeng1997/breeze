// 编译期剥离：本 TU 把 ACTIVE_LEVEL 设为 WARN（3），低于 WARN 的宏连参数一起消失。
// 无测试框架依赖：失败即 abort。

#define BREEZE_LOG_ACTIVE_LEVEL 3

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>

#include "logger.h"
#include "logger_macros.h"

using namespace breeze;
namespace fs = std::filesystem;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

namespace
{
int g_evals = 0;
int Eval() { return ++g_evals; }
} // namespace

int main()
{
    const auto dir = fs::temp_directory_path() / std::format("breeze_strip_test_{}", ::getpid());
    fs::remove_all(dir);
    LoggerConfig cfg;
    cfg.file.dir = dir.string();
    cfg.name = "s";
    cfg.async.enabled = false;
    cfg.level = Level::Trace; // 运行时全开：证明缺的是编译期剥离，不是运行时过滤
    std::string err;
    auto lg = Logger::Create(cfg, err);
    CHECK(lg);

    BREEZE_LOG_TRACE(lg, "trace-{}", Eval());
    BREEZE_LOG_DEBUG(lg, "debug-{}", Eval());
    BREEZE_LOG_INFO(lg, "info-{}", Eval());
    CHECK(g_evals == 0); // 被剥离的宏，参数连求值都没有

    BREEZE_LOG_WARN(lg, "warn-{}", Eval());
    BREEZE_LOG_ERROR(lg, "error-{}", Eval());
    CHECK(g_evals == 2);
    lg->Flush();

    std::ifstream in(dir / "s.log");
    std::stringstream ss;
    ss << in.rdbuf();
    const auto text = ss.str();
    CHECK(text.find("trace-") == std::string::npos);
    CHECK(text.find("debug-") == std::string::npos);
    CHECK(text.find("info-") == std::string::npos);
    CHECK(text.find("warn-1") != std::string::npos);
    CHECK(text.find("error-2") != std::string::npos);

    lg.reset();
    fs::remove_all(dir);
    std::puts("logger_strip_test: ok");
}
