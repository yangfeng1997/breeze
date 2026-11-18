// lua_pb 的 schema 层：把 protoc 产出的 FileDescriptorSet 编译成不可变的表驱动结构。
// 设计见 docs/Lua_pb库设计.md §4。
//
// 编译后只读：可以跨线程共享（编解码各线程用自己的 Codec，Schema 本身没有可变状态）。
// 不依赖 libprotobuf：描述符用本库自己的 wire reader 解析（只解析需要的字段，其余跳过）。
// 只支持 proto3：描述符里没有 proto3 文件时返回 kUnsupportedSyntax；导入进来的 proto2 文件
// （如 google/protobuf/descriptor.proto）整份跳过，只有 proto3 文件引用了它定义的消息时才报引用失败。

#pragma once

#include <cstdint>
#include <deque>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "base/lua_pb/error.h"
#include "base/lua_pb/wire.h"

namespace breeze::lua_pb
{

using TypeId = std::uint32_t;

// 解码 / 编码的嵌套上限：与 protobuf C++ 的 kDefaultRecursionLimit、upb 的默认深度一致。
inline constexpr unsigned kMaxDepth = 100;

// oneof 组数上限：只是健全性上界（quanta 的 luapb 写死 16 且不越界检查，我们不做那种事）。
inline constexpr std::uint32_t kMaxOneofs = 64;

// 字段的编解码操作：决定用哪条路径处理，switch 分派，不用虚函数。
// 有符号 / 无符号分开：int32 的负数在线上是 10 字节 varint，解码要按 int32 解释；uint32 / uint64 的
// 范围检查也不同，合并成一个 Op 会出错。
enum class Op : std::uint8_t
{
    kInt32,
    kUint32,
    kInt64,
    kUint64,
    kSint32,   // zigzag
    kSint64,   // zigzag
    kBool,
    kEnum,     // 看着像 int32，独立出来是为了范围检查与默认值
    kFixed32,
    kSfixed32,
    kFloat,
    kFixed64,
    kSfixed64,
    kDouble,
    kString,
    kBytes,
    kMessage,
    kMap,
};

// 该字段的 wire type（map 与 message 是 LEN）。
constexpr WireType OpWireType(Op op) noexcept
{
    switch (op)
    {
        case Op::kInt32:
        case Op::kUint32:
        case Op::kInt64:
        case Op::kUint64:
        case Op::kSint32:
        case Op::kSint64:
        case Op::kBool:
        case Op::kEnum:
            return WireType::kVarint;
        case Op::kFixed32:
        case Op::kSfixed32:
        case Op::kFloat:
            return WireType::kFixed32;
        case Op::kFixed64:
        case Op::kSfixed64:
        case Op::kDouble:
            return WireType::kFixed64;
        case Op::kString:
        case Op::kBytes:
        case Op::kMessage:
        case Op::kMap:
            return WireType::kLen;
    }
    return WireType::kVarint;
}

// 是否是可打包的标量（repeated 时按 packed 编码；解码侧两种形式都收）。
constexpr bool OpIsPackable(Op op) noexcept
{
    return OpWireType(op) != WireType::kLen;
}

// 是否可以做 map 的 key：proto3 只允许整数类型、bool、string（float / double / bytes / enum / message 都不行）。
// 注意这与"可打包"不是一回事：string 可做 key 但不可打包。
constexpr bool OpIsValidMapKey(Op op) noexcept
{
    switch (op)
    {
        case Op::kInt32:
        case Op::kUint32:
        case Op::kInt64:
        case Op::kUint64:
        case Op::kSint32:
        case Op::kSint64:
        case Op::kBool:
        case Op::kFixed32:
        case Op::kSfixed32:
        case Op::kFixed64:
        case Op::kSfixed64:
        case Op::kString:
            return true;
        case Op::kEnum:
        case Op::kFloat:
        case Op::kDouble:
        case Op::kBytes:
        case Op::kMessage:
        case Op::kMap:
            return false;
    }
    return false;
}

// proto3 的存在性模型：隐式存在（标量缺失即默认值）与显式存在（optional / oneof 成员 / message）。
enum class Presence : std::uint8_t
{
    kImplicit,
    kExplicit,
};

struct FieldInfo
{
    std::string_view name;               // proto 字段名（snake_case），指向 Schema 的名称池
    std::uint32_t number = 0;
    Op op = Op::kInt32;
    Presence presence = Presence::kImplicit;
    bool repeated = false;
    bool packed = false;                 // repeated 且可打包：编码按 packed 写
    TypeId message_type = kNoIndex;      // kMessage：子消息类型；kMap：value 是消息时是它的类型
    Op map_key_op = Op::kInt32;       // kMap：key 的操作
    Op map_value_op = Op::kInt32;     // kMap：value 是标量时的操作（value 是消息时看 message_type）
    std::uint32_t oneof_index = kNoIndex; // kNoIndex = 不属于任何 oneof（proto3 optional 的合成 oneof 不算）
};

struct MessageInfo
{
    std::string_view full_name;
    std::span<const FieldInfo> fields;          // 按 field number 升序
    std::span<const std::uint32_t> dense;       // field number 稠密时：number → fields 下标（kNoIndex = 无）；否则为空
    std::span<const std::uint32_t> by_name;     // fields 下标，按 name 升序
    std::uint32_t oneof_count = 0;
};

class Schema
{
public:
    // 描述符字节（可以是多个 .pb 直接拼接：protobuf 的 repeated 语义允许）。
    // 失败返回 Error（kBadDescriptor / kUnsupportedSyntax / kUnsupportedFeature / kTooManyOneofs）。
    static std::expected<Schema, Error> Load(std::span<const std::byte> descriptor_set);

    Schema(Schema&&) noexcept = default;
    Schema& operator=(Schema&&) noexcept = default;
    Schema(const Schema&) = delete;
    Schema& operator=(const Schema&) = delete;
    ~Schema() = default;

    std::span<const MessageInfo> Messages() const noexcept { return messages_; }
    std::size_t MessageCount() const noexcept { return messages_.size(); }

    const MessageInfo& Message(TypeId id) const noexcept { return messages_[id]; }
    std::string_view MessageName(TypeId id) const noexcept
    {
        return id < messages_.size() ? messages_[id].full_name : std::string_view{};
    }

    // 全限定名（"pkg.Msg"，可带前导 '.'）；找不到返回 kNoIndex。
    TypeId FindType(std::string_view full_name) const noexcept;

    // 按 field number / 名字找字段；找不到返回 nullptr。名字查找用 by_name（二分）。
    static const FieldInfo* FindField(const MessageInfo& m, std::uint32_t number) noexcept;
    static const FieldInfo* FindField(const MessageInfo& m, std::string_view name) noexcept;

    // "decode pkg.Msg.field @12: invalid varint"，给最外层转 Lua 错误用。
    std::string Describe(const Error& e) const;

private:
    Schema() = default;

    // 名字池：deque 保证 push_back 之后已有元素的引用不失效，FieldInfo::name 才敢存 string_view。
    std::deque<std::string> names_;
    std::vector<MessageInfo> messages_;
    std::vector<std::uint32_t> dense_storage_; // messages_ 的 dense 都指向这里（各消息一段）
    std::vector<std::uint32_t> by_name_storage_;
    std::vector<FieldInfo> field_storage_;
    std::unordered_map<std::string_view, TypeId> by_full_name_;
};

} // namespace breeze::lua_pb
