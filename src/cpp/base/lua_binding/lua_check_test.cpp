// 断言/日志测试：ENSURE 每处只记一次；CHECK 在任何构建都致命（Linux 用 fork 验证）。

#include "base/lua_binding/check.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#if defined(__unix__)
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace breeze::lua_binding;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                                \
        }                                                                                \
    } while (0)

namespace {

std::atomic<int> g_logs{0};

void CountingSink(std::string_view) noexcept {
    g_logs.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

int main() {
    SetLogSink(&CountingSink);

    // 同一调用点只记一次：宏里的 static flag 按展开位置共享。
    for (int i = 0; i < 2; ++i) {
        BREEZE_LUA_ENSURE(false, "ensure fires once per call site");
    }
    CHECK(g_logs.load(std::memory_order_relaxed) == 1);

#if defined(__unix__)
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        SetLogSink(nullptr);
        BREEZE_LUA_CHECK(false, "fatal check must abort");
        ::_exit(0);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0));
#endif

    std::printf("lua_check_test: ok\n");
    return 0;
}
