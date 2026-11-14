#include "base/fastlua/check.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace breeze::fastlua {
namespace {

std::mutex g_mutex;
LogSink g_log_sink = nullptr;
FatalHandler g_fatal_handler = nullptr;

}  // namespace

void SetLogSink(LogSink sink) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_log_sink = sink;
}

void SetFatalHandler(FatalHandler handler) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_fatal_handler = handler;
}

void LogMessage(std::string_view message) noexcept {
    LogSink sink;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        sink = g_log_sink;
    }
    if (sink != nullptr) {
        sink(message);
        return;
    }
    std::fwrite(message.data(), 1, message.size(), stderr);
    std::fputc('\n', stderr);
}

[[noreturn]] void Fatal(std::string_view message) noexcept {
    LogMessage(message);
    FatalHandler handler;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        handler = g_fatal_handler;
    }
    if (handler != nullptr) {
        handler(message);
    }
    std::abort();
}

}  // namespace breeze::fastlua
