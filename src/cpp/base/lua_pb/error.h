// lua_pb 的错误模型：结构化、平凡可析构、不抛异常。
// 设计见 docs/Lua_pb库设计.md §8：C++ API 返回 std::expected<T, Error>；只有在最外层的 Lua 边界
// （lua_pb.cpp / codec.cpp）才用 Describe() 拼文本并转成一次 luaL_error。
//
// Error 用定长 POD：任何 Lua 栈帧里持有它都安全（沙箱 OOM 时 Lua 会 longjmp 到 panic，见 Lua绑定库设计.md）。

#pragma once

#include <cstdint>
#include <string_view>

namespace breeze::lua_pb
{

enum class ErrorCode : std::uint8_t
{
    kNone = 0,
    // 描述符 / schema
    kBadDescriptor,      // 描述符结构不对：字段缺号、长度越界、label 非法……
    kUnsupportedSyntax,  // 描述符里没有任何 proto3 文件（只有 proto2 / editions）
    kUnsupportedFeature, // group、extension 等本库不做的特性
    kTooManyOneofs,      // oneof 组数超过 kMaxOneofs
    // 编解码
    kUnknownType,     // 类型名不在 schema 里
    kTruncated,       // 数据在字段中途结束
    kInvalidVarint,   // varint 超过 10 字节或第 10 字节有高位
    kInvalidWireType, // tag 里的 wire type 是 6 / 7（不存在），或 group
    kLengthOverflow,  // LEN 前缀超过剩余字节
    kDepthExceeded,   // 嵌套超过 kMaxDepth
    kFieldZero,       // field number 0（协议禁止）
    kTypeMismatch,    // Lua 值的类型与字段类型不符，或整数越界
    kUnknownField,    // 编码时表里出现 schema 没有的字段名
    kOneofConflict,   // 编码时同一个 oneof 出现多个成员
    kLengthInvalid,   // 编码时字符串 / 子消息长度装不进预留的 varint
};

// 索引类字段的"无"值（type_id / field_idx 用）。
inline constexpr std::uint32_t kNoIndex = 0xFFFFFFFFu;

struct Error
{
    ErrorCode code = ErrorCode::kNone;
    std::uint32_t type_id = kNoIndex;   // 相关消息类型；无则 kNoIndex
    std::uint32_t field_idx = kNoIndex; // 该消息内字段下标；无则 kNoIndex
    std::uint32_t offset = 0;           // 出错处在输入里的字节偏移（解码时）
};

constexpr std::string_view ErrorCodeName(ErrorCode c) noexcept
{
    switch (c)
    {
        case ErrorCode::kNone:
            return "none";
        case ErrorCode::kBadDescriptor:
            return "bad descriptor";
        case ErrorCode::kUnsupportedSyntax:
            return "unsupported syntax";
        case ErrorCode::kUnsupportedFeature:
            return "unsupported feature";
        case ErrorCode::kTooManyOneofs:
            return "too many oneofs";
        case ErrorCode::kUnknownType:
            return "unknown type";
        case ErrorCode::kTruncated:
            return "truncated";
        case ErrorCode::kInvalidVarint:
            return "invalid varint";
        case ErrorCode::kInvalidWireType:
            return "invalid wire type";
        case ErrorCode::kLengthOverflow:
            return "length overflow";
        case ErrorCode::kDepthExceeded:
            return "depth exceeded";
        case ErrorCode::kFieldZero:
            return "field number 0";
        case ErrorCode::kTypeMismatch:
            return "type mismatch";
        case ErrorCode::kUnknownField:
            return "unknown field";
        case ErrorCode::kOneofConflict:
            return "oneof conflict";
        case ErrorCode::kLengthInvalid:
            return "length invalid";
    }
    return "unknown";
}

constexpr Error MakeError(ErrorCode c,
                          std::uint32_t type_id = kNoIndex,
                          std::uint32_t field_idx = kNoIndex,
                          std::uint32_t offset = 0) noexcept
{
    return Error{c, type_id, field_idx, offset};
}

} // namespace breeze::lua_pb
