// FileDescriptorSet → Schema。设计见 docs/Lua_pb库设计.md §4。
//
// 用本库自己的 wire reader 解析（不链 libprotobuf）：只读需要的字段，其余按 wire type 跳过。
// 已知字段的 wire type 不符时按坏描述符处理（描述符是 protoc 产出的，出问题是构建错误，不该被容忍）。
//
// proto3-only 的取舍：
//   - 没有任何 proto3 文件（只有 proto2 / editions）→ kUnsupportedSyntax；
//   - 导入进来的 proto2 文件（如 google/protobuf/descriptor.proto）整份跳过：自定义 option 会把它拉进
//     --include_imports 的描述符里，但它的消息不会被我们的 IDL 当字段类型引用；真引用了就报引用失败；
//   - required（proto2）/ group / extension 一律 kUnsupportedFeature。

#include "base/lua_pb/schema.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace breeze::lua_pb
{
namespace
{

// ---- descriptor.proto 里我们要用的字段号 ----
// FileDescriptorSet
constexpr std::uint32_t kSetFile = 1;
// FileDescriptorProto
constexpr std::uint32_t kFile_Name = 1;
constexpr std::uint32_t kFile_Package = 2;
constexpr std::uint32_t kFile_MessageType = 4;
constexpr std::uint32_t kFile_EnumType = 5;
constexpr std::uint32_t kFile_Syntax = 12;
constexpr std::uint32_t kFile_Edition = 14;
// DescriptorProto
constexpr std::uint32_t kMsg_Name = 1;
constexpr std::uint32_t kMsg_Field = 2;
constexpr std::uint32_t kMsg_NestedType = 3;
constexpr std::uint32_t kMsg_EnumType = 4;
constexpr std::uint32_t kMsg_Options = 7;
// DescriptorProto.Options
constexpr std::uint32_t kMsgOptions_MapEntry = 7;
// FieldDescriptorProto
constexpr std::uint32_t kField_Name = 1;
constexpr std::uint32_t kField_Number = 3;
constexpr std::uint32_t kField_Label = 4;
constexpr std::uint32_t kField_Type = 5;
constexpr std::uint32_t kField_TypeName = 6;
constexpr std::uint32_t kField_OneofIndex = 9;
constexpr std::uint32_t kField_Proto3Optional = 17;
// EnumDescriptorProto
constexpr std::uint32_t kEnum_Name = 1;

constexpr std::uint32_t kLabelOptional = 1;
constexpr std::uint32_t kLabelRequired = 2;
constexpr std::uint32_t kLabelRepeated = 3;

// FieldDescriptorProto.Type
constexpr std::uint32_t kTDouble = 1;
constexpr std::uint32_t kTFloat = 2;
constexpr std::uint32_t kTInt64 = 3;
constexpr std::uint32_t kTUint64 = 4;
constexpr std::uint32_t kTInt32 = 5;
constexpr std::uint32_t kTFixed64 = 6;
constexpr std::uint32_t kTFixed32 = 7;
constexpr std::uint32_t kTBool = 8;
constexpr std::uint32_t kTString = 9;
constexpr std::uint32_t kTGroup = 10;
constexpr std::uint32_t kTMessage = 11;
constexpr std::uint32_t kTBytes = 12;
constexpr std::uint32_t kTUint32 = 13;
constexpr std::uint32_t kTEnum = 14;
constexpr std::uint32_t kTSfixed32 = 15;
constexpr std::uint32_t kTSfixed64 = 16;
constexpr std::uint32_t kTSint32 = 17;
constexpr std::uint32_t kTSint64 = 18;

struct RawField
{
    std::string name;
    std::string type_name;
    std::uint32_t number = 0;
    std::uint32_t label = 0;
    std::uint32_t type = 0;
    std::uint32_t oneof_index = 0;
    bool has_oneof_index = false;
    bool proto3_optional = false;
};

struct RawMessage
{
    std::string name;
    std::vector<RawField> fields;
    std::vector<RawMessage> nested;
    std::vector<std::string> enums;
    bool map_entry = false;
};

struct RawFile
{
    std::string name;
    std::string package;
    std::string syntax;
    bool has_edition = false;
    std::vector<RawMessage> messages;
    std::vector<std::string> enums;
};

// 读取一个 LEN 字段到 string；wire type 不是 LEN 视为坏描述符。
std::expected<void, Error> ReadString(Reader& r, std::string& out)
{
    std::span<const std::byte> v;
    if (!r.ReadLengthDelimited(v))
        return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                         static_cast<std::uint32_t>(r.Offset())));
    out.assign(reinterpret_cast<const char*>(v.data()), v.size());
    return {};
}

std::expected<std::uint64_t, Error> ReadUint64(Reader& r)
{
    std::uint64_t v = 0;
    if (!r.ReadVarint(v))
        return std::unexpected(
            MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex, static_cast<std::uint32_t>(r.Offset())));
    return v;
}

std::expected<bool, Error> ReadBool(Reader& r)
{
    auto v = ReadUint64(r);
    if (!v)
        return std::unexpected(v.error());
    return *v != 0;
}

std::expected<void, Error> ParseField(Reader r, RawField& out)
{
    std::uint32_t tag = 0;
    while (!r.Empty())
    {
        if (!r.ReadTag(tag))
            return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                             static_cast<std::uint32_t>(r.Offset())));
        const std::uint32_t number = TagFieldNumber(tag);
        const WireType wt = TagWireType(tag);
        switch (number)
        {
            case kField_Name:
                if (wt != WireType::kLen)
                    break;
                if (auto e = ReadString(r, out.name); !e)
                    return e;
                continue;
            case kField_TypeName:
                if (wt != WireType::kLen)
                    break;
                if (auto e = ReadString(r, out.type_name); !e)
                    return e;
                continue;
            case kField_Number:
            case kField_Label:
            case kField_Type:
            case kField_OneofIndex:
            {
                if (wt != WireType::kVarint)
                    break;
                auto v = ReadUint64(r);
                if (!v)
                    return std::unexpected(v.error());
                if (number == kField_Number)
                    out.number = static_cast<std::uint32_t>(*v);
                else if (number == kField_Label)
                    out.label = static_cast<std::uint32_t>(*v);
                else if (number == kField_Type)
                    out.type = static_cast<std::uint32_t>(*v);
                else
                {
                    out.oneof_index = static_cast<std::uint32_t>(*v);
                    out.has_oneof_index = true;
                }
                continue;
            }
            case kField_Proto3Optional:
            {
                if (wt != WireType::kVarint)
                    break;
                auto v = ReadBool(r);
                if (!v)
                    return std::unexpected(v.error());
                out.proto3_optional = *v;
                continue;
            }
            default:
                break;
        }
        if (!r.Skip(wt))
            return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                             static_cast<std::uint32_t>(r.Offset())));
    }
    return {};
}

std::expected<void, Error> ParseMessage(Reader r, RawMessage& out, unsigned depth)
{
    if (depth > kMaxDepth)
        return std::unexpected(MakeError(ErrorCode::kBadDescriptor));
    std::uint32_t tag = 0;
    while (!r.Empty())
    {
        if (!r.ReadTag(tag))
            return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                             static_cast<std::uint32_t>(r.Offset())));
        const std::uint32_t number = TagFieldNumber(tag);
        const WireType wt = TagWireType(tag);
        switch (number)
        {
            case kMsg_Name:
                if (wt != WireType::kLen)
                    break;
                if (auto e = ReadString(r, out.name); !e)
                    return e;
                continue;
            case kMsg_Field:
            {
                if (wt != WireType::kLen)
                    break;
                std::span<const std::byte> body;
                if (!r.ReadLengthDelimited(body))
                    return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                                     static_cast<std::uint32_t>(r.Offset())));
                out.fields.emplace_back();
                if (auto e = ParseField(Reader{body}, out.fields.back()); !e)
                    return e;
                continue;
            }
            case kMsg_NestedType:
            {
                if (wt != WireType::kLen)
                    break;
                std::span<const std::byte> body;
                if (!r.ReadLengthDelimited(body))
                    return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                                     static_cast<std::uint32_t>(r.Offset())));
                out.nested.emplace_back();
                if (auto e = ParseMessage(Reader{body}, out.nested.back(), depth + 1); !e)
                    return e;
                continue;
            }
            case kMsg_EnumType:
            {
                if (wt != WireType::kLen)
                    break;
                std::span<const std::byte> body;
                if (!r.ReadLengthDelimited(body))
                    return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                                     static_cast<std::uint32_t>(r.Offset())));
                std::string enum_name;
                Reader er{body};
                std::uint32_t etag = 0;
                while (!er.Empty())
                {
                    if (!er.ReadTag(etag))
                        return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                                         static_cast<std::uint32_t>(er.Offset())));
                    const WireType ewt = TagWireType(etag);
                    if (TagFieldNumber(etag) == kEnum_Name && ewt == WireType::kLen)
                    {
                        if (auto e = ReadString(er, enum_name); !e)
                            return e;
                        continue;
                    }
                    if (!er.Skip(ewt))
                        return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                                         static_cast<std::uint32_t>(er.Offset())));
                }
                if (!enum_name.empty())
                    out.enums.push_back(std::move(enum_name));
                continue;
            }
            case kMsg_Options:
            {
                if (wt != WireType::kLen)
                    break;
                std::span<const std::byte> body;
                if (!r.ReadLengthDelimited(body))
                    return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                                     static_cast<std::uint32_t>(r.Offset())));
                Reader o{body};
                std::uint32_t otag = 0;
                while (!o.Empty())
                {
                    if (!o.ReadTag(otag))
                        return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                                         static_cast<std::uint32_t>(o.Offset())));
                    const WireType owt = TagWireType(otag);
                    if (TagFieldNumber(otag) == kMsgOptions_MapEntry && owt == WireType::kVarint)
                    {
                        auto v = ReadBool(o);
                        if (!v)
                            return std::unexpected(v.error());
                        out.map_entry = *v;
                        continue;
                    }
                    if (!o.Skip(owt))
                        return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                                         static_cast<std::uint32_t>(o.Offset())));
                }
                continue;
            }
            default:
                break;
        }
        if (!r.Skip(wt))
            return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                             static_cast<std::uint32_t>(r.Offset())));
    }
    return {};
}

std::expected<void, Error> ParseFile(Reader r, RawFile& out)
{
    std::uint32_t tag = 0;
    while (!r.Empty())
    {
        if (!r.ReadTag(tag))
            return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                             static_cast<std::uint32_t>(r.Offset())));
        const std::uint32_t number = TagFieldNumber(tag);
        const WireType wt = TagWireType(tag);
        switch (number)
        {
            case kFile_Name:
                if (wt != WireType::kLen)
                    break;
                if (auto e = ReadString(r, out.name); !e)
                    return e;
                continue;
            case kFile_Package:
                if (wt != WireType::kLen)
                    break;
                if (auto e = ReadString(r, out.package); !e)
                    return e;
                continue;
            case kFile_Syntax:
                if (wt != WireType::kLen)
                    break;
                if (auto e = ReadString(r, out.syntax); !e)
                    return e;
                continue;
            case kFile_MessageType:
            {
                if (wt != WireType::kLen)
                    break;
                std::span<const std::byte> body;
                if (!r.ReadLengthDelimited(body))
                    return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                                     static_cast<std::uint32_t>(r.Offset())));
                out.messages.emplace_back();
                if (auto e = ParseMessage(Reader{body}, out.messages.back(), 1); !e)
                    return e;
                continue;
            }
            case kFile_EnumType:
            {
                if (wt != WireType::kLen)
                    break;
                std::span<const std::byte> body;
                if (!r.ReadLengthDelimited(body))
                    return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                                     static_cast<std::uint32_t>(r.Offset())));
                std::string enum_name;
                Reader er{body};
                std::uint32_t etag = 0;
                while (!er.Empty())
                {
                    if (!er.ReadTag(etag))
                        return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                                         static_cast<std::uint32_t>(er.Offset())));
                    const WireType ewt = TagWireType(etag);
                    if (TagFieldNumber(etag) == kEnum_Name && ewt == WireType::kLen)
                    {
                        if (auto e = ReadString(er, enum_name); !e)
                            return e;
                        continue;
                    }
                    if (!er.Skip(ewt))
                        return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                                         static_cast<std::uint32_t>(er.Offset())));
                }
                if (!enum_name.empty())
                    out.enums.push_back(std::move(enum_name));
                continue;
            }
            case kFile_Edition:
                if (wt == WireType::kVarint)
                {
                    auto v = ReadUint64(r);
                    if (!v)
                        return std::unexpected(v.error());
                    out.has_edition = true;
                    continue;
                }
                break;
            default:
                break;
        }
        if (!r.Skip(wt))
            return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                             static_cast<std::uint32_t>(r.Offset())));
    }
    return {};
}

// 全限定名：package + 嵌套路径。空 package 时不要前导点。
std::string JoinName(std::string_view prefix, std::string_view name)
{
    if (prefix.empty())
        return std::string(name);
    std::string s(prefix);
    s.push_back('.');
    s.append(name);
    return s;
}

// descriptor 的 type 枚举 → Op；非标量（message / enum / group）返回 std::nullopt。
std::optional<Op> ScalarOp(std::uint32_t t) noexcept
{
    switch (t)
    {
        case kTInt32:
            return Op::kInt32;
        case kTUint32:
            return Op::kUint32;
        case kTInt64:
            return Op::kInt64;
        case kTUint64:
            return Op::kUint64;
        case kTSint32:
            return Op::kSint32;
        case kTSint64:
            return Op::kSint64;
        case kTBool:
            return Op::kBool;
        case kTEnum:
            return Op::kEnum;
        case kTFixed32:
            return Op::kFixed32;
        case kTSfixed32:
            return Op::kSfixed32;
        case kTFloat:
            return Op::kFloat;
        case kTFixed64:
            return Op::kFixed64;
        case kTSfixed64:
            return Op::kSfixed64;
        case kTDouble:
            return Op::kDouble;
        case kTString:
            return Op::kString;
        case kTBytes:
            return Op::kBytes;
        default:
            return std::nullopt;
    }
}

// 编译期收集：一个文件里的所有消息（递归），带全限定名与所属文件。
struct FlatMessage
{
    const RawMessage* raw = nullptr;
    std::string full_name;
};

void FlattenMessages(const RawMessage& m, std::string_view prefix, std::vector<FlatMessage>& out)
{
    const std::string full = JoinName(prefix, m.name);
    out.push_back(FlatMessage{&m, full});
    for (const auto& n : m.nested)
        FlattenMessages(n, full, out);
}

void CollectEnums(const RawMessage& m, std::string_view prefix, std::vector<std::string>& out)
{
    const std::string full = JoinName(prefix, m.name);
    for (const auto& e : m.enums)
        out.push_back(JoinName(full, e));
    for (const auto& n : m.nested)
        CollectEnums(n, full, out);
}

} // namespace

std::expected<Schema, Error> Schema::Load(std::span<const std::byte> descriptor_set)
{
    // ---- 1. 解析描述符 ----
    std::vector<RawFile> files;
    {
        Reader r{descriptor_set};
        std::uint32_t tag = 0;
        while (!r.Empty())
        {
            if (!r.ReadTag(tag))
                return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                                 static_cast<std::uint32_t>(r.Offset())));
            const WireType wt = TagWireType(tag);
            if (TagFieldNumber(tag) == kSetFile && wt == WireType::kLen)
            {
                std::span<const std::byte> body;
                if (!r.ReadLengthDelimited(body))
                    return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                                     static_cast<std::uint32_t>(r.Offset())));
                files.emplace_back();
                if (auto e = ParseFile(Reader{body}, files.back()); !e)
                    return std::unexpected(e.error());
                continue;
            }
            if (!r.Skip(wt))
                return std::unexpected(MakeError(ErrorCode::kBadDescriptor, kNoIndex, kNoIndex,
                                                 static_cast<std::uint32_t>(r.Offset())));
        }
    }

    // ---- 2. 选 proto3 文件；同名文件去重（多个 .pb 拼接时会出现重复） ----
    std::vector<const RawFile*> proto3;
    {
        std::unordered_set<std::string> seen;
        for (const auto& f : files)
        {
            if (f.syntax != "proto3" || f.has_edition)
                continue; // proto2 / editions：整份跳过（见文件头注释）
            if (!seen.insert(f.name).second)
                continue;
            proto3.push_back(&f);
        }
    }
    if (proto3.empty())
        return std::unexpected(MakeError(ErrorCode::kUnsupportedSyntax));

    Schema s;

    // ---- 3. 展平消息、登记类型名（map entry 不登记为类型） ----
    std::vector<FlatMessage> flat;
    std::vector<std::string> enum_names;
    for (const RawFile* f : proto3)
    {
        for (const auto& m : f->messages)
            FlattenMessages(m, f->package, flat);
        for (const auto& e : f->enums)
            enum_names.push_back(JoinName(f->package, e));
        for (const auto& m : f->messages)
            CollectEnums(m, f->package, enum_names);
    }

    // 名字池：deque，push_back 之后已有元素的引用不失效，FieldInfo::name 才敢存 string_view。
    std::vector<const FlatMessage*> types; // 非 map entry 的消息，按登记顺序 = TypeId
    // 含 map entry 的全名表：map 字段的 type_name 指向 entry 消息（entry 不登记为类型）。
    // key 指向 flat 里的 std::string，flat 在 Load 期间不再改动。
    std::unordered_map<std::string_view, const FlatMessage*> all_by_name;
    for (const auto& fm : flat)
        all_by_name.emplace(std::string_view{fm.full_name}, &fm);

    for (const auto& fm : flat)
    {
        if (fm.raw->map_entry)
            continue;
        s.names_.push_back(fm.full_name);
        const std::string_view view{s.names_.back()};
        if (!s.by_full_name_.emplace(view, static_cast<TypeId>(types.size())).second)
            return std::unexpected(MakeError(ErrorCode::kBadDescriptor)); // 重名类型
        types.push_back(&fm);
    }

    std::unordered_set<std::string_view> enum_set;
    for (const auto& n : enum_names)
    {
        s.names_.push_back(n);
        enum_set.insert(std::string_view{s.names_.back()});
    }

    const auto strip_dot = [](std::string_view name) noexcept
    {
        // protoc 给的是全限定名，带前导点
        if (!name.empty() && name.front() == '.')
            name.remove_prefix(1);
        return name;
    };
    const auto find_type = [&](std::string_view name) -> TypeId
    {
        const auto it = s.by_full_name_.find(strip_dot(name));
        return it == s.by_full_name_.end() ? kNoIndex : it->second;
    };
    const auto find_flat = [&](std::string_view name) -> const FlatMessage*
    {
        const auto it = all_by_name.find(strip_dot(name));
        return it == all_by_name.end() ? nullptr : it->second;
    };

    // ---- 4. 逐消息建字段 ----
    std::vector<std::vector<FieldInfo>> tmp_fields(types.size());
    for (std::size_t i = 0; i < types.size(); ++i)
    {
        const RawMessage& raw = *types[i]->raw;
        std::vector<FieldInfo>& out = tmp_fields[i];
        out.reserve(raw.fields.size());

        for (const RawField& rf : raw.fields)
        {
            if (rf.number == 0 || rf.number > kMaxFieldNumber || rf.name.empty())
                return std::unexpected(MakeError(ErrorCode::kBadDescriptor, static_cast<TypeId>(i)));
            if (rf.label == kLabelRequired)
                return std::unexpected(MakeError(ErrorCode::kUnsupportedFeature, static_cast<TypeId>(i)));
            if (rf.label != kLabelOptional && rf.label != kLabelRepeated)
                return std::unexpected(MakeError(ErrorCode::kBadDescriptor, static_cast<TypeId>(i)));
            if (rf.type == kTGroup)
                return std::unexpected(MakeError(ErrorCode::kUnsupportedFeature, static_cast<TypeId>(i)));

            FieldInfo fi;
            fi.number = rf.number;
            fi.repeated = rf.label == kLabelRepeated;

            if (rf.type == kTMessage)
            {
                // 注意：map 字段的 type_name 指向 entry 消息，而 entry 不登记为类型，所以先用含 entry 的表查。
                const FlatMessage* target = find_flat(rf.type_name);
                if (target == nullptr)
                    return std::unexpected(MakeError(ErrorCode::kBadDescriptor, static_cast<TypeId>(i)));
                if (fi.repeated && target->raw->map_entry)
                {
                    const RawMessage& entry = *target->raw;
                    const RawField* key = nullptr;
                    const RawField* val = nullptr;
                    for (const RawField& ef : entry.fields)
                    {
                        if (ef.number == 1)
                            key = &ef;
                        else if (ef.number == 2)
                            val = &ef;
                    }
                    if (key == nullptr || val == nullptr)
                        return std::unexpected(MakeError(ErrorCode::kBadDescriptor, static_cast<TypeId>(i)));
                    const auto key_op = ScalarOp(key->type);
                    if (!key_op || !OpIsValidMapKey(*key_op))
                        return std::unexpected(MakeError(ErrorCode::kBadDescriptor, static_cast<TypeId>(i)));
                    if (val->type == kTMessage)
                    {
                        const TypeId vt = find_type(val->type_name);
                        if (vt == kNoIndex)
                            return std::unexpected(MakeError(ErrorCode::kBadDescriptor, static_cast<TypeId>(i)));
                        fi.op = Op::kMap;
                        fi.map_key_op = *key_op;
                        fi.message_type = vt;
                    }
                    else
                    {
                        const auto val_op = ScalarOp(val->type);
                        if (!val_op)
                            return std::unexpected(
                                MakeError(ErrorCode::kBadDescriptor, static_cast<TypeId>(i)));
                        fi.op = Op::kMap;
                        fi.map_key_op = *key_op;
                        fi.map_value_op = *val_op;
                        fi.message_type = kNoIndex;
                    }
                    s.names_.push_back(rf.name);
                    fi.name = s.names_.back();
                    out.push_back(fi);
                    continue;
                }
                // 普通 message 字段：entry 只能出现在 map 位置，这里必须落到已登记的类型上
                const TypeId tid = find_type(rf.type_name);
                if (tid == kNoIndex)
                    return std::unexpected(MakeError(ErrorCode::kBadDescriptor, static_cast<TypeId>(i)));
                fi.op = Op::kMessage;
                fi.message_type = tid;
            }
            else if (rf.type == kTEnum)
            {
                const std::string_view full = [&]
                {
                    std::string_view n{rf.type_name};
                    if (!n.empty() && n.front() == '.')
                        n.remove_prefix(1);
                    return n;
                }();
                if (!enum_set.contains(full))
                    return std::unexpected(MakeError(ErrorCode::kBadDescriptor, static_cast<TypeId>(i)));
                fi.op = Op::kEnum;
            }
            else
            {
                const auto op = ScalarOp(rf.type);
                if (!op)
                    return std::unexpected(MakeError(ErrorCode::kBadDescriptor, static_cast<TypeId>(i)));
                fi.op = *op;
            }

            if (fi.repeated && OpIsPackable(fi.op))
                fi.packed = true;

            // presence：repeated 不参与；proto3 optional / 真 oneof / message / map 是显式存在
            if (!fi.repeated)
            {
                const bool synthetic_oneof = rf.proto3_optional;
                if (synthetic_oneof || fi.op == Op::kMessage || fi.op == Op::kMap)
                    fi.presence = Presence::kExplicit;
                else if (rf.has_oneof_index)
                    fi.presence = Presence::kExplicit;
            }
            if (!fi.repeated && rf.has_oneof_index && !rf.proto3_optional)
            {
                if (rf.oneof_index >= kMaxOneofs)
                    return std::unexpected(MakeError(ErrorCode::kTooManyOneofs, static_cast<TypeId>(i)));
                fi.oneof_index = rf.oneof_index;
            }

            s.names_.push_back(rf.name);
            fi.name = s.names_.back();
            out.push_back(fi);
        }

        std::sort(out.begin(), out.end(), [](const FieldInfo& a, const FieldInfo& b) { return a.number < b.number; });
        for (std::size_t k = 1; k < out.size(); ++k)
        {
            if (out[k].number == out[k - 1].number)
                return std::unexpected(MakeError(ErrorCode::kBadDescriptor, static_cast<TypeId>(i)));
        }
    }

    // ---- 5. 落进 Schema 的存储：先算总量并 reserve，再取 span（否则 vector 扩容会让 span 悬空） ----
    std::size_t total_fields = 0;
    std::size_t total_dense = 0;
    std::size_t total_names = 0;
    for (std::size_t i = 0; i < types.size(); ++i)
    {
        const auto& f = tmp_fields[i];
        total_fields += f.size();
        total_names += f.size();
        if (!f.empty())
        {
            const std::uint32_t max_number = f.back().number;
            if (max_number < 4 * static_cast<std::uint32_t>(f.size()))
                total_dense += max_number + 1;
        }
    }
    s.field_storage_.reserve(total_fields);
    s.dense_storage_.reserve(total_dense);
    s.by_name_storage_.reserve(total_names);
    s.messages_.reserve(types.size());

    for (std::size_t i = 0; i < types.size(); ++i)
    {
        MessageInfo mi;
        mi.full_name = s.names_[i]; // 前 types.size() 个名字就是消息全名
        const std::vector<FieldInfo>& f = tmp_fields[i];

        const std::size_t field_begin = s.field_storage_.size();
        s.field_storage_.insert(s.field_storage_.end(), f.begin(), f.end());
        mi.fields = std::span<const FieldInfo>{s.field_storage_.data() + field_begin, f.size()};

        // 字段号 → 下标：稠密用数组直取，稀疏用二分（fields 已按 number 升序）
        if (!f.empty())
        {
            const std::uint32_t max_number = f.back().number;
            if (max_number < 4 * static_cast<std::uint32_t>(f.size()))
            {
                const std::size_t begin = s.dense_storage_.size();
                s.dense_storage_.resize(begin + max_number + 1, kNoIndex);
                for (std::size_t k = 0; k < f.size(); ++k)
                    s.dense_storage_[begin + f[k].number] = static_cast<std::uint32_t>(k);
                mi.dense = std::span<const std::uint32_t>{s.dense_storage_.data() + begin, max_number + 1};
            }
        }

        // 名字 → 下标：按 name 升序，二分
        {
            const std::size_t begin = s.by_name_storage_.size();
            for (std::size_t k = 0; k < f.size(); ++k)
                s.by_name_storage_.push_back(static_cast<std::uint32_t>(k));
            std::sort(s.by_name_storage_.begin() + static_cast<std::ptrdiff_t>(begin), s.by_name_storage_.end(),
                      [&](std::uint32_t a, std::uint32_t b) { return f[a].name < f[b].name; });
            mi.by_name = std::span<const std::uint32_t>{s.by_name_storage_.data() + begin, f.size()};
        }

        for (const FieldInfo& fi : mi.fields)
        {
            if (fi.oneof_index != kNoIndex && fi.oneof_index + 1 > mi.oneof_count)
                mi.oneof_count = fi.oneof_index + 1;
        }

        s.messages_.push_back(mi);
    }

    return s;
}

TypeId Schema::FindType(std::string_view full_name) const noexcept
{
    if (!full_name.empty() && full_name.front() == '.')
        full_name.remove_prefix(1);
    const auto it = by_full_name_.find(full_name);
    return it == by_full_name_.end() ? kNoIndex : it->second;
}

const FieldInfo* Schema::FindField(const MessageInfo& m, std::uint32_t number) noexcept
{
    if (!m.dense.empty())
    {
        if (number >= m.dense.size())
            return nullptr;
        const std::uint32_t idx = m.dense[number];
        return idx == kNoIndex ? nullptr : &m.fields[idx];
    }
    const auto it = std::lower_bound(m.fields.begin(), m.fields.end(), number,
                                     [](const FieldInfo& f, std::uint32_t n) { return f.number < n; });
    return (it != m.fields.end() && it->number == number) ? &*it : nullptr;
}

const FieldInfo* Schema::FindField(const MessageInfo& m, std::string_view name) noexcept
{
    const auto it = std::lower_bound(m.by_name.begin(), m.by_name.end(), name,
                                     [&](std::uint32_t idx, std::string_view n) { return m.fields[idx].name < n; });
    if (it == m.by_name.end())
        return nullptr;
    const FieldInfo& f = m.fields[*it];
    return f.name == name ? &f : nullptr;
}

std::string Schema::Describe(const Error& e) const
{
    std::string out;
    if (e.type_id != kNoIndex)
        out.append(MessageName(e.type_id));
    if (e.type_id != kNoIndex && e.field_idx != kNoIndex && e.type_id < messages_.size() &&
        e.field_idx < messages_[e.type_id].fields.size())
    {
        out.push_back('.');
        out.append(messages_[e.type_id].fields[e.field_idx].name);
    }
    if (e.offset != 0)
    {
        out.append(" @");
        out.append(std::to_string(e.offset));
    }
    if (!out.empty())
        out.append(": ");
    out.append(ErrorCodeName(e.code));
    return out;
}

} // namespace breeze::lua_pb
