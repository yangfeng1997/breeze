// 给 TlsLayer 用的自定义 BIO：不碰 fd、不回调连接，只在两个缓冲之间搬字节。属于 tls 的内部实现。
//
//   读：从 TlsBioState::in（本次 Unwrap 交进来的线上字节的视图）里取；取空了置 retry-read（SSL_ERROR_WANT_READ）。
//       **不拷贝线上字节到 BIO 自己的缓冲**：视图直接给 SSL 读，省一次拷贝。视图只在 Unwrap 期间有效，Unwrap 返回前清空。
//   写：追加进 TlsBioState::out（层的发送缓冲）并返回全长，永远不会 WANT_WRITE：发送缓冲的上界由连接按预算控制
//       （一次最多 Wrap 一个预算，见 LayeredStreamConnection::kWireBudget），不是由 BIO 反压。

#pragma once

#include <cstddef>
#include <span>

#include "base/buffer/buffer.h"

struct bio_st;

namespace breeze::tls
{

struct TlsBioState
{
    std::span<const std::byte> in;
    Buffer* out = nullptr;
};

// 创建一个绑定到 state 的 BIO；失败返回空。state 必须比 BIO 活得久（TlsLayer 里 state 是成员，SSL_free 在它之前）。
bio_st* NewBio(TlsBioState* state);

} // namespace breeze::tls
