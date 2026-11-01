#include "base/net/socket/unique_fd.h"

#include <unistd.h>

namespace breeze
{

void UniqueFd::reset(int fd) noexcept
{
    // close 失败（EINTR / EIO）也无可挽回：Linux 上 fd 此时已经释放，重试反而可能关掉别人的 fd。
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = fd;
}

} // namespace breeze
