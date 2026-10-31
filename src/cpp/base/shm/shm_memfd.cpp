#include "shm_memfd.h"

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace breeze::shm
{

namespace
{
constexpr int kAllSeals = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;
}

std::expected<Fd, int> CreateMemFd(const char* name, std::uint64_t size)
{
    Fd fd(::memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (!fd.valid())
        return std::unexpected(errno);
    if (::ftruncate(fd.get(), static_cast<off_t>(size)) != 0)
        return std::unexpected(errno);
    return fd;
}

int SealMemFd(int fd) { return ::fcntl(fd, F_ADD_SEALS, kAllSeals) == 0 ? 0 : errno; }

int CheckSealedMemFd(int fd, std::uint64_t* size_out)
{
    const int seals = ::fcntl(fd, F_GET_SEALS);
    if (seals < 0)
        return EINVAL; // 不是 memfd，或者文件系统不支持封印
    if ((seals & kAllSeals) != kAllSeals)
        return EINVAL;
    struct stat st{};
    if (::fstat(fd, &st) != 0)
        return errno;
    if (!S_ISREG(st.st_mode) || st.st_size < 0)
        return EINVAL;
    *size_out = static_cast<std::uint64_t>(st.st_size);
    return 0;
}

std::expected<Fd, int> CreateEventFd()
{
    Fd fd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (!fd.valid())
        return std::unexpected(errno);
    return fd;
}

int CheckEventFd(int fd, const char* proc_fd_dir)
{
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0)
        return errno;
    if ((flags & O_NONBLOCK) == 0)
        return EINVAL;
    char path[128];
    std::snprintf(path, sizeof(path), "%s/%d", proc_fd_dir, fd);
    char target[64];
    const ssize_t n = ::readlink(path, target, sizeof(target) - 1);
    if (n < 0)
        return EINVAL;
    target[n] = '\0';
    return std::strcmp(target, "anon_inode:[eventfd]") == 0 ? 0 : EINVAL;
}

} // namespace breeze::shm
