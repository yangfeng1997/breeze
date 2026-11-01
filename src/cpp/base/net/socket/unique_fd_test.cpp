// UniqueFd：只能移动、析构时关闭。无测试框架依赖：失败即 abort。

#include "base/net/socket/unique_fd.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <utility>

using namespace breeze;

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

void TestUniqueFdRaii()
{
    int fds[2];
    CHECK(::pipe(fds) == 0);
    {
        UniqueFd a(fds[0]);
        UniqueFd b = std::move(a);
        CHECK(!a.valid() && b.valid()); // NOLINT：被移走的对象不再拥有 fd
        CHECK(b.get() == fds[0]);
    } // b 析构关闭 fds[0]
    CHECK(::fcntl(fds[0], F_GETFD) == -1 && errno == EBADF);
    ::close(fds[1]);
}

} // namespace

int main()
{
    TestUniqueFdRaii();
    std::puts("unique_fd_test: ok");
    return 0;
}
