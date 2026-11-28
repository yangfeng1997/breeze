#include "base/net/loop/io_engine.h"

#include <sys/epoll.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "base/logger/logger.h"
#include "base/logger/logger_macros.h"
#include "base/net/socket/unique_fd.h"
#if BREEZE_HAS_IO_URING
#include "base/net/loop/uring_engine.h"
#endif

namespace breeze
{
namespace
{
class EpollIoEngine final : public IoEngine
{
public:
    int Init() override
    {
        fd_.reset(::epoll_create1(EPOLL_CLOEXEC));
        return fd_.valid() ? 0 : errno;
    }
    IoBackend backend() const noexcept override { return IoBackend::Epoll; }
    int Add(IPollable* target, int fd, std::uint32_t events) override
    {
        epoll_event ev{};
        ev.events = events;
        ev.data.ptr = target;
        return ::epoll_ctl(fd_.get(), EPOLL_CTL_ADD, fd, &ev) == 0 ? 0 : errno;
    }
    void Mod(IPollable* target, int fd, std::uint32_t events) override
    {
        epoll_event ev{};
        ev.events = events;
        ev.data.ptr = target;
        if (::epoll_ctl(fd_.get(), EPOLL_CTL_MOD, fd, &ev) != 0)
            LOG_FATAL("epoll_ctl(MOD) 失败 fd={} errno={}", fd, errno);
    }
    void Del(int fd) override
    {
        if (::epoll_ctl(fd_.get(), EPOLL_CTL_DEL, fd, nullptr) != 0)
            LOG_FATAL("epoll_ctl(DEL) 失败 fd={} errno={}", fd, errno);
    }
    int Wait(std::span<IoEvent> events, int timeout_ms) override
    {
        if (events.empty())
            return 0;
        std::array<epoll_event, 1024> native;
        const int n = ::epoll_wait(
            fd_.get(), native.data(), static_cast<int>(std::min(native.size(), events.size())), timeout_ms);
        if (n < 0)
            return errno == EINTR ? 0 : -errno;
        for (int i = 0; i < n; ++i)
            events[i] = {static_cast<IPollable*>(native[i].data.ptr), native[i].events};
        return n;
    }

private:
    UniqueFd fd_;
};
} // namespace

IoRegistration IoEngine::AttachStream(int, IStreamCompletion&, int& error)
{
    error = ENOTSUP;
    return 0;
}
void IoEngine::ReadStream(IoRegistration, bool) {}
int IoEngine::WriteStream(IoRegistration, std::span<const iovec>) { return ENOTSUP; }
int IoEngine::WriteOwnedStream(IoRegistration, SharedBuffer) { return ENOTSUP; }
int IoEngine::WriteOwnedStreamV(IoRegistration, std::span<const std::byte>, SharedBuffer) { return ENOTSUP; }
void IoEngine::Detach(IoRegistration) {}
IoRegistration IoEngine::AttachDatagram(int, IDatagramCompletion&, int& error)
{
    error = ENOTSUP;
    return 0;
}
int IoEngine::SendDatagram(
    IoRegistration, std::span<const std::byte>, std::span<const std::byte>, const sockaddr_storage*, socklen_t)
{
    return ENOTSUP;
}

std::unique_ptr<IoEngine> MakeIoEngine(IoBackend backend, int& error, UringOptions options)
{
    error = 0;
    (void)options; // 禁用uring编译时不使用后端专有选项。
    if (const char* receive = std::getenv("BREEZE_TEST_URING_RECV"))
    {
        if (std::strcmp(receive, "multishot") == 0)
            options.receive_mode = UringReceiveMode::Multishot;
        else if (std::strcmp(receive, "hybrid") == 0)
            options.receive_mode = UringReceiveMode::Hybrid;
        else
        {
            error = EINVAL;
            return nullptr;
        }
    }
    // 测试/排障可强制后端；生产默认 Epoll，显式 Auto 才做运行时探测。
    if (const char* value = std::getenv("BREEZE_IO_BACKEND"))
    {
        if (std::strcmp(value, "epoll") == 0)
            backend = IoBackend::Epoll;
        else if (std::strcmp(value, "uring") == 0 || std::strcmp(value, "io_uring") == 0)
            backend = IoBackend::IoUring;
        else if (std::strcmp(value, "auto") == 0)
            backend = IoBackend::Auto;
        else
        {
            error = EINVAL;
            return nullptr;
        }
    }
    if (backend == IoBackend::Auto)
    {
#if BREEZE_HAS_IO_URING
        auto candidate = MakeUringEngine(options.receive_mode);
        if (const int e = candidate->Init(); e == 0)
            return candidate;
        else
            LOG_WARN("io_uring 初始化失败，Auto 回退 epoll errno={}", e);
#endif
        backend = IoBackend::Epoll;
    }
    std::unique_ptr<IoEngine> engine;
    switch (backend)
    {
        case IoBackend::Epoll:
            engine = std::make_unique<EpollIoEngine>();
            break;
        case IoBackend::IoUring:
#if BREEZE_HAS_IO_URING
            engine = MakeUringEngine(options.receive_mode);
            break;
#else
            error = ENOTSUP;
            return nullptr;
#endif
        default:
            error = EINVAL;
            return nullptr;
    }
    if (const int e = engine->Init(); e != 0)
    {
        error = e;
        return nullptr;
    }
    return engine;
}
} // namespace breeze
