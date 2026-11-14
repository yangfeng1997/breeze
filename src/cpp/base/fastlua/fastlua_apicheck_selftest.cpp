// 证明测试构建链接的 Lua 真的开了 LUA_USE_APICHECK + LUAI_ASSERT。
// 在子进程里故意用非法栈索引调用 lua_pushvalue：带检查的 Lua 会断言 abort。
// 父进程期待子进程异常结束；子进程没 abort 则本测试返回非零（说明开关没生效）。

#include "base/fastlua/config.h"  // 按 C 链接引入 Lua 头

#if defined(__unix__)
#include <sys/wait.h>
#include <unistd.h>
#endif

int main() {
#if defined(__unix__)
    const pid_t pid = fork();
    if (pid < 0) {
        return 1;
    }
    if (pid == 0) {
        lua_State* L = luaL_newstate();
        if (L == nullptr) {
            ::_exit(2);
        }
        lua_pushvalue(L, 100);  // 非法索引：apicheck 必须在这里 abort
        lua_close(L);
        ::_exit(0);             // 没 abort，说明检查没开
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        return 1;
    }
    if (WIFSIGNALED(status)) {
        return 0;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 2) {
        return 1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) != 0 ? 0 : 1;
#else
    return 0;
#endif
}
