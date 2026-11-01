#include "base/net/shm/shm_protocol.h"

#include <cerrno>
#include <cstring>

namespace breeze::shm::wire
{

namespace
{

constexpr bool IsBusIdChar(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == ':' || c == '@' || c == '-';
}

} // namespace

bool IsValidBusId(std::string_view name) noexcept
{
    if (name.empty() || name.size() >= kBusIdSize)
        return false;
    for (const char c : name)
    {
        if (!IsBusIdChar(c))
            return false;
    }
    return true;
}

bool SetBusId(Hello& hello, std::string_view name) noexcept
{
    if (!IsValidBusId(name))
        return false;
    std::memset(hello.busid, 0, sizeof(hello.busid));
    std::memcpy(hello.busid, name.data(), name.size());
    return true;
}

std::optional<std::string_view> GetBusId(const Hello& hello) noexcept
{
    const void* end = std::memchr(hello.busid, '\0', sizeof(hello.busid));
    if (end == nullptr)
        return std::nullopt; // 没有结尾：不假定它以 '\0' 结尾
    const std::string_view name(hello.busid, static_cast<const char*>(end) - hello.busid);
    if (!IsValidBusId(name))
        return std::nullopt;
    return name;
}

int CheckHello(const Hello& hello) noexcept
{
    if (hello.hdr.magic != kMagic || hello.hdr.version != kVersion ||
        hello.hdr.type != static_cast<std::uint16_t>(MsgType::kHello))
        return EPROTO;
    const bool reattach = hello.attach_type == static_cast<std::uint32_t>(AttachType::kReattach);
    if (!reattach && hello.attach_type != static_cast<std::uint32_t>(AttachType::kAttach))
        return EINVAL;
    if ((hello.flags & ~kFlagDiscardRetained) != 0)
        return EINVAL;
    if (hello.transport < static_cast<std::uint32_t>(Transport::kUdsOnly) ||
        hello.transport > static_cast<std::uint32_t>(Transport::kShmRequired))
        return EINVAL;
    if (reattach)
    {
        // Reattach 恢复的是 App 手里已有的那块内存：没有"要多大"，没有可丢弃的队列，也不会退化成仅 UDS。
        if (hello.flags != 0 || hello.transport == static_cast<std::uint32_t>(Transport::kUdsOnly) ||
            hello.tx_capacity != 0 || hello.rx_capacity != 0 || hello.tx_max_payload != 0 || hello.rx_max_payload != 0)
            return EINVAL;
    }
    if (!GetBusId(hello).has_value())
        return EINVAL;
    return 0;
}

Header MakeHeader(MsgType type) noexcept { return Header{kMagic, kVersion, static_cast<std::uint16_t>(type)}; }

int CheckReplyHeader(const Reply& reply) noexcept
{
    if (reply.hdr.magic != kMagic || reply.hdr.version != kVersion ||
        reply.hdr.type != static_cast<std::uint16_t>(MsgType::kReply))
        return EPROTO;
    return 0;
}

int CheckControl(const Control& c) noexcept
{
    if (c.hdr.magic != kMagic || c.hdr.version != kVersion ||
        c.hdr.type != static_cast<std::uint16_t>(MsgType::kControl))
        return EPROTO;
    switch (static_cast<ControlOp>(c.op))
    {
        case ControlOp::kPrepareRestart:
            return c.arg == 0 ? EINVAL : 0; // 0 毫秒没有意义：要不保留就发 Bye
        case ControlOp::kBye:
            return c.arg == 0 ? 0 : EINVAL;
    }
    return EINVAL;
}

Control MakeControl(ControlOp op, std::uint32_t arg) noexcept
{
    return Control{MakeHeader(MsgType::kControl), static_cast<std::uint32_t>(op), arg};
}

} // namespace breeze::shm::wire
