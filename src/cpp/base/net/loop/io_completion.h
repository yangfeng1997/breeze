// 完成模型内部契约：回调只在所属 loop 上执行；Detach 后不再回调，内核请求仍由引擎收尾。
#pragma once

#include <sys/socket.h>
#include <sys/uio.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace breeze
{
using IoRegistration = std::uint64_t; // 0 无效；引擎内单调分配，避免 fd/连接地址复用导致迟到完成项串线。

class IStreamCompletion
{
public:
    virtual ~IStreamCompletion() = default;
    // error 为正 errno；空 data 且 error=0 是 EOF。data 仅本次回调有效、可原地解帧。
    virtual void OnIoRead(std::span<std::byte> data, int error) = 0;
    // 返回本次提交实际写出的字节数；短写由调用方保序重提，不代表对端已经收到。
    virtual void OnIoWrite(std::size_t bytes, int error) = 0;
};

class IDatagramCompletion
{
public:
    virtual ~IDatagramCompletion() = default;
    virtual void OnIoDatagram(const sockaddr_storage& source, socklen_t length, std::span<const std::byte> data) = 0;
    virtual void OnIoDatagramError(int error) = 0;
    virtual void OnIoDatagramSent(std::size_t) {}
};
} // namespace breeze
