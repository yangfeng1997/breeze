// Lua table ↔ protobuf wire。设计见 docs/Lua_pb库设计.md §5、§6、§7。
//
// 解码：按 tag 走一遍，字段名从 per-state 的 kit 表按 rawgeti 取（不做字符串哈希），写入 rawset；
//       隐式存在的标量缺失时逐字段填默认值；未知字段号与 wire type 不匹配的字段按协议跳过。
// 编码：先用 lua_next 扫一遍输入表（只对字符串键查 per-state 指针哈希 / 长名内容比较），把"存在哪些
//       字段"记成位图；再按字段号序输出（输出确定）。空 repeated / 空 map 不写；隐式标量等于默认值不写；
//       显式存在字段（optional / oneof / message）非 nil 即写。
//
// 严格面（自己产生的输入）：未知字段名、类型不符、整数越界、同一 oneof 多成员、repeated 有空洞 → 报错。
// 宽容面（网络来的字节）：未知字段号、wire type 不匹配 → 跳过。

#include "base/lua_pb/codec.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace breeze::lua_pb
{
namespace
{

// 字段名长度 ≤ 40 时 Lua 会驻留短字符串，可以按 TString* 指针查表。
constexpr std::size_t kInternedMaxLen = 40;

// 栈上位图容量：512 个字段。超过就退回"逐字段 rawget 检查"的兜底路径（真实消息远小于这个数）。
constexpr std::uint32_t kTrackedFields = 512;

// 解码建表时哈希部分的预分配上界（见 docs/Lua_pb库设计.md §21 的校准）。
// 可用 -DLUA_PB_HASH_PREALLOC=N 覆盖，只为基准校准用。
#ifndef LUA_PB_HASH_PREALLOC
#define LUA_PB_HASH_PREALLOC 32
#endif
constexpr std::uint32_t kHashPreallocCap = LUA_PB_HASH_PREALLOC;

// 出错的上下文：消息类型 + 字段下标，用来拼 Describe。
[[nodiscard]] Error Fail(ErrorCode code, TypeId type, std::uint32_t field = kNoIndex, std::uint32_t offset = 0) noexcept
{
    return MakeError(code, type, field, offset);
}

struct PresenceBits
{
    std::array<std::uint64_t, kTrackedFields / 64> words{};
    bool tracked = true;

    void Mark(std::uint32_t idx) noexcept
    {
        if (tracked)
            words[idx >> 6] |= (std::uint64_t{1} << (idx & 63u));
    }
    [[nodiscard]] bool Has(std::uint32_t idx) const noexcept
    {
        return tracked && ((words[idx >> 6] >> (idx & 63u)) & 1u) != 0;
    }
};

[[nodiscard]] constexpr bool IsScalarOp(Op op) noexcept
{
    return op != Op::kMessage && op != Op::kMap;
}

// 隐式存在 + 单值 + 标量：缺省时要填默认值，编码时等于默认值就不写。
[[nodiscard]] constexpr bool NeedsDefaultFill(const FieldInfo& f) noexcept
{
    return !f.repeated && IsScalarOp(f.op) && f.presence == Presence::kImplicit;
}

void PushDefault(lua_State* L, Op op)
{
    switch (op)
    {
        case Op::kBool:
            lua_pushboolean(L, 0);
            return;
        case Op::kString:
        case Op::kBytes:
            lua_pushliteral(L, "");
            return;
        default:
            lua_pushinteger(L, 0); // 数值与 enum
            return;
    }
}

// 栈上 idx 处的值是否等于该 Op 的默认值（编码时的"隐式标量不写"判定）。
// 浮点用 signbit 区分 -0.0：protobuf 里 -0.0 不是默认值。
bool IsDefaultOnStack(lua_State* L, int idx, Op op)
{
    switch (op)
    {
        case Op::kBool:
            return lua_toboolean(L, idx) == 0;
        case Op::kString:
        case Op::kBytes:
        {
            std::size_t len = 0;
            lua_tolstring(L, idx, &len);
            return len == 0;
        }
        case Op::kFloat:
        case Op::kDouble:
        {
            const double v = lua_tonumber(L, idx);
            return v == 0.0 && !std::signbit(v);
        }
        default:
            // 整数 / enum：**不能**用 lua_tointeger 判 0——它对没有整数表示的数（如 3.5）返回 0，
            // 会把 "3.5" 误判成默认值而静默跳过；那种情况要留给类型校验去报错。
            if (lua_isinteger(L, idx))
                return lua_tointeger(L, idx) == 0;
            return lua_tonumber(L, idx) == 0.0;
    }
}

// 读整数：接受 integer，也接受"整值且范围内"的浮点；其余（字符串 / 非整值 / 越界）都算类型不符。
bool ReadIntegerInRange(lua_State* L, int idx, std::int64_t lo, std::uint64_t hi, std::int64_t& out)
{
    if (lua_isinteger(L, idx))
    {
        const lua_Integer v = lua_tointeger(L, idx);
        if (v < lo)
            return false;
        if (v >= 0 && static_cast<std::uint64_t>(v) > hi)
            return false;
        out = static_cast<std::int64_t>(v);
        return true;
    }
    if (!lua_isnumber(L, idx))
        return false;
    const double d = lua_tonumber(L, idx);
    if (!std::isfinite(d) || std::trunc(d) != d)
        return false;
    if (d < static_cast<double>(lo) || d > 18446744073709551615.0) // 上界按最宽处理，再由下面精判
        return false;
    const auto v = static_cast<std::int64_t>(d);
    if (v < lo)
        return false;
    if (v >= 0 && static_cast<std::uint64_t>(v) > hi)
        return false;
    out = v;
    return true;
}

bool ReadBoolStrict(lua_State* L, int idx) { return lua_type(L, idx) == LUA_TBOOLEAN; }

// ---- 编码：写字段体（不含 tag）----
std::expected<void, Error> WriteScalarBody(lua_State* L, Writer& w, Op op, int idx, Error ctx)
{
    const auto bad = [&] { return std::unexpected(ctx); };
    switch (op)
    {
        case Op::kInt32:
        case Op::kSint32:
        case Op::kInt64:
        case Op::kSint64:
        case Op::kEnum:
        {
            std::int64_t v = 0;
            const std::int64_t lo = std::numeric_limits<std::int32_t>::min();
            const std::uint64_t hi = static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
            if (op == Op::kInt64 || op == Op::kSint64)
            {
                if (!ReadIntegerInRange(L, idx, std::numeric_limits<std::int64_t>::min(),
                                        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()), v))
                    return bad();
            }
            else if (!ReadIntegerInRange(L, idx, lo, hi, v))
            {
                return bad();
            }
            if (op == Op::kSint32)
                w.WriteVarint(ZigZagEncode(static_cast<std::int32_t>(v)));
            else if (op == Op::kSint64)
                w.WriteVarint(ZigZagEncode(v));
            else
                w.WriteVarint(static_cast<std::uint64_t>(v)); // 负数按 64 位补码展开成 10 字节 varint
            return {};
        }
        case Op::kUint32:
        {
            std::int64_t v = 0;
            if (!ReadIntegerInRange(L, idx, 0, std::numeric_limits<std::uint32_t>::max(), v))
                return bad();
            w.WriteVarint(static_cast<std::uint64_t>(v));
            return {};
        }
        case Op::kUint64:
        {
            std::int64_t v = 0;
            // uint64 ≥ 2^63 在 Lua 侧是回绕后的负数，这里按位模式原样写回
            if (!ReadIntegerInRange(L, idx, std::numeric_limits<std::int64_t>::min(),
                                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()), v))
                return bad();
            w.WriteVarint(static_cast<std::uint64_t>(v));
            return {};
        }
        case Op::kBool:
        {
            if (!ReadBoolStrict(L, idx))
                return bad();
            w.WriteVarint(lua_toboolean(L, idx) != 0 ? 1u : 0u);
            return {};
        }
        case Op::kFixed32:
        case Op::kSfixed32:
        {
            std::int64_t v = 0;
            const bool signed_ok = ReadIntegerInRange(L, idx, std::numeric_limits<std::int32_t>::min(),
                                                      static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()), v);
            if (op == Op::kFixed32)
            {
                if (!ReadIntegerInRange(L, idx, 0, std::numeric_limits<std::uint32_t>::max(), v))
                    return bad();
            }
            else if (!signed_ok)
            {
                return bad();
            }
            w.WriteFixed32(static_cast<std::uint32_t>(v));
            return {};
        }
        case Op::kFloat:
        {
            if (!lua_isnumber(L, idx))
                return bad();
            w.WriteFixed32(std::bit_cast<std::uint32_t>(static_cast<float>(lua_tonumber(L, idx))));
            return {};
        }
        case Op::kFixed64:
        case Op::kSfixed64:
        {
            std::int64_t v = 0;
            if (op == Op::kFixed64)
            {
                if (!ReadIntegerInRange(L, idx, 0, std::numeric_limits<std::uint64_t>::max(), v))
                    return bad();
            }
            else if (!ReadIntegerInRange(L, idx, std::numeric_limits<std::int64_t>::min(),
                                         static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()), v))
            {
                return bad();
            }
            w.WriteFixed64(static_cast<std::uint64_t>(v));
            return {};
        }
        case Op::kDouble:
        {
            if (!lua_isnumber(L, idx))
                return bad();
            w.WriteFixed64(std::bit_cast<std::uint64_t>(lua_tonumber(L, idx)));
            return {};
        }
        case Op::kString:
        case Op::kBytes:
        {
            if (lua_type(L, idx) != LUA_TSTRING)
                return bad();
            std::size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            w.WriteVarint(len);
            w.WriteRaw(std::span<const std::byte>{reinterpret_cast<const std::byte*>(s), len});
            return {};
        }
        case Op::kMessage:
        case Op::kMap:
            break; // 由调用方处理
    }
    return bad();
}

// ---- 解码：读字段体并压栈 ----
std::expected<void, Error> ReadScalar(lua_State* L, Reader& r, Op op, Error ctx)
{
    const auto bad = [&](ErrorCode code) { return std::unexpected(Fail(code, ctx.type_id, ctx.field_idx, 0)); };
    std::uint64_t v = 0;
    switch (op)
    {
        case Op::kInt32:
        case Op::kSint32:
        case Op::kEnum:
        {
            if (!r.ReadVarint(v))
                return bad(ErrorCode::kTruncated);
            const std::int32_t x = (op == Op::kSint32) ? static_cast<std::int32_t>(ZigZagDecode(static_cast<std::uint32_t>(v)))
                                                       : static_cast<std::int32_t>(static_cast<std::uint32_t>(v));
            lua_pushinteger(L, x);
            return {};
        }
        case Op::kUint32:
        {
            if (!r.ReadVarint(v))
                return bad(ErrorCode::kTruncated);
            lua_pushinteger(L, static_cast<std::uint32_t>(v));
            return {};
        }
        case Op::kInt64:
        case Op::kUint64:
        {
            if (!r.ReadVarint(v))
                return bad(ErrorCode::kTruncated);
            // uint64 ≥ 2^63 回绕成负数（位精确），编码侧按位模式写回
            lua_pushinteger(L, static_cast<lua_Integer>(v));
            return {};
        }
        case Op::kSint64:
        {
            if (!r.ReadVarint(v))
                return bad(ErrorCode::kTruncated);
            lua_pushinteger(L, static_cast<lua_Integer>(ZigZagDecode(v)));
            return {};
        }
        case Op::kBool:
        {
            if (!r.ReadVarint(v))
                return bad(ErrorCode::kTruncated);
            lua_pushboolean(L, v != 0);
            return {};
        }
        case Op::kFixed32:
        case Op::kSfixed32:
        case Op::kFloat:
        {
            std::uint32_t x = 0;
            if (!r.ReadFixed32(x))
                return bad(ErrorCode::kTruncated);
            if (op == Op::kFloat)
                lua_pushnumber(L, static_cast<lua_Number>(std::bit_cast<float>(x)));
            else if (op == Op::kSfixed32)
                lua_pushinteger(L, static_cast<std::int32_t>(x));
            else
                lua_pushinteger(L, x);
            return {};
        }
        case Op::kFixed64:
        case Op::kSfixed64:
        case Op::kDouble:
        {
            std::uint64_t x = 0;
            if (!r.ReadFixed64(x))
                return bad(ErrorCode::kTruncated);
            if (op == Op::kDouble)
                lua_pushnumber(L, static_cast<lua_Number>(std::bit_cast<double>(x)));
            else
                lua_pushinteger(L, static_cast<lua_Integer>(x));
            return {};
        }
        case Op::kString:
        case Op::kBytes:
        {
            std::span<const std::byte> s;
            if (!r.ReadLengthDelimited(s))
                return bad(ErrorCode::kLengthOverflow);
            lua_pushlstring(L, reinterpret_cast<const char*>(s.data()), s.size());
            return {};
        }
        case Op::kMessage:
        case Op::kMap:
            break;
    }
    return bad(ErrorCode::kTypeMismatch);
}

} // namespace

// ============================== Codec 生命周期 ==============================

Codec::Codec(lua_State* L, std::shared_ptr<const Schema> schema)
    : L_(L)
    , schema_(std::move(schema))
    , kits_(schema_->MessageCount())
{
}

Codec::~Codec()
{
    // Lua 状态必须先于 Codec 销毁（P3 的绑定负责这个顺序）；这里只清 registry 引用。
    if (L_ == nullptr)
        return;
    for (Kit& k : kits_)
        if (k.names_ref != 0)
            luaL_unref(L_, LUA_REGISTRYINDEX, k.names_ref);
}

Codec::Codec(Codec&& o) noexcept
    : L_(std::exchange(o.L_, nullptr))
    , schema_(std::move(o.schema_))
    , kits_(std::move(o.kits_))
    , writer_(std::move(o.writer_))
{
}

Codec& Codec::operator=(Codec&& o) noexcept
{
    if (this != &o)
    {
        L_ = std::exchange(o.L_, nullptr);
        schema_ = std::move(o.schema_);
        kits_ = std::move(o.kits_);
        writer_ = std::move(o.writer_);
    }
    return *this;
}

const Codec::Kit& Codec::GetKit(TypeId type)
{
    Kit& kit = kits_[type];
    if (kit.names_ref != 0)
        return kit;

    const MessageInfo& m = schema_->Message(type);
    lua_createtable(L_, 0, static_cast<int>(m.fields.size()));
    for (std::size_t i = 0; i < m.fields.size(); ++i)
    {
        const FieldInfo& f = m.fields[i];
        lua_pushlstring(L_, f.name.data(), f.name.size());
        const void* p = lua_topointer(L_, -1);
        if (f.name.size() <= kInternedMaxLen)
            kit.by_ptr.emplace(p, static_cast<std::uint32_t>(i));
        else
            kit.long_names.emplace_back(f.name, static_cast<std::uint32_t>(i));
        lua_rawseti(L_, -2, static_cast<lua_Integer>(i) + 1);
    }
    kit.names_ref = luaL_ref(L_, LUA_REGISTRYINDEX);
    return kit;
}

// ============================== 对外入口 ==============================

std::expected<void, Error> Codec::Decode(TypeId type, std::span<const std::byte> payload)
{
    if (type == kNoIndex || type >= schema_->MessageCount())
        return std::unexpected(MakeError(ErrorCode::kUnknownType));
    Reader r{payload};
    return DecodeMessage(type, r, 1);
}

std::expected<void, Error> Codec::Decode(std::string_view type_name, std::span<const std::byte> payload)
{
    const TypeId type = schema_->FindType(type_name);
    if (type == kNoIndex)
        return std::unexpected(MakeError(ErrorCode::kUnknownType));
    return Decode(type, payload);
}

std::expected<std::span<const std::byte>, Error> Codec::Encode(TypeId type, int index)
{
    if (type == kNoIndex || type >= schema_->MessageCount())
        return std::unexpected(MakeError(ErrorCode::kUnknownType));
    writer_.Clear();
    if (auto e = EncodeMessage(type, index, 1); !e)
        return std::unexpected(e.error());
    return writer_.Bytes();
}

std::expected<std::span<const std::byte>, Error> Codec::Encode(std::string_view type_name, int index)
{
    const TypeId type = schema_->FindType(type_name);
    if (type == kNoIndex)
        return std::unexpected(MakeError(ErrorCode::kUnknownType));
    return Encode(type, index);
}

// ============================== 解码 ==============================

std::expected<void, Error> Codec::DecodeMessage(TypeId type, Reader& r, unsigned depth)
{
    if (depth > kMaxDepth)
        return std::unexpected(MakeError(ErrorCode::kDepthExceeded, type));

    const int entry_top = lua_gettop(L_);
    const MessageInfo& m = schema_->Message(type);
    const Kit& kit = GetKit(type);

    // 每个字段最多压 3 个值（名字 / 值 / 数组），嵌套时逐层叠加；不显式扩栈会在 release 下静默写坏栈。
    if (lua_checkstack(L_, static_cast<int>(m.fields.size()) + 8) == 0)
        return std::unexpected(MakeError(ErrorCode::kDepthExceeded, type));

    // 字段名表留在栈上，按绝对下标取名字：每字段一次 rawgeti
    lua_rawgeti(L_, LUA_REGISTRYINDEX, kit.names_ref);
    const int names = lua_gettop(L_);

    lua_createtable(L_, 0,
                    static_cast<int>(m.fields.size() < kHashPreallocCap ? m.fields.size() : kHashPreallocCap));
    const int obj = lua_gettop(L_);

    PresenceBits present;
    present.tracked = m.fields.size() <= kTrackedFields;

    // oneof：记住每个 oneof 当前设置的是哪个字段，切换时把前一个成员从表里删掉
    std::array<std::uint32_t, kMaxOneofs> oneof_set{};
    for (std::uint32_t i = 0; i < m.oneof_count && i < kMaxOneofs; ++i)
        oneof_set[i] = kNoIndex;

    // 非 packed repeated：连续同一个字段时把数组表留在栈顶，避免每个元素都 rawget
    std::uint32_t cached_field = kNoIndex;
    int cache_top = 0; // 缓存时数组表所在的栈位（= 当时的栈高）

    const auto flush_cache = [&]
    {
        if (cached_field != kNoIndex)
        {
            lua_settop(L_, cache_top - 1); // 只弹掉缓存的数组表，别碰下面的结果表
            cached_field = kNoIndex;
        }
    };

    std::uint32_t tag = 0;
    while (!r.Empty())
    {
        const std::uint32_t tag_offset = static_cast<std::uint32_t>(r.Offset());
        if (!r.ReadTag(tag))
        {
            flush_cache();
            lua_settop(L_, entry_top);
            return std::unexpected(MakeError(ErrorCode::kTruncated, type, kNoIndex, tag_offset));
        }
        const std::uint32_t number = TagFieldNumber(tag);
        const WireType wt = TagWireType(tag);
        if (number == 0)
        {
            flush_cache();
            lua_settop(L_, entry_top);
            return std::unexpected(MakeError(ErrorCode::kFieldZero, type, kNoIndex, tag_offset));
        }

        const FieldInfo* f = Schema::FindField(m, number);
        const std::uint32_t fidx =
            (f != nullptr) ? static_cast<std::uint32_t>(f - m.fields.data()) : kNoIndex;

        // 未知字段，或 wire type 与 schema 不符：按协议跳过（前向兼容）
        const bool packed_form = (f != nullptr) && f->repeated && OpIsPackable(f->op) && wt == WireType::kLen;
        if (f == nullptr || (!packed_form && wt != OpWireType(f->op)))
        {
            flush_cache();
            if (!r.Skip(wt))
            {
                lua_settop(L_, entry_top);
                return std::unexpected(MakeError(ErrorCode::kTruncated, type, kNoIndex, tag_offset));
            }
            continue;
        }

        const Error ctx = Fail(ErrorCode::kTruncated, type, fidx, tag_offset);

        if (f->op == Op::kMap)
        {
            flush_cache();
            std::span<const std::byte> entry;
            if (!r.ReadLengthDelimited(entry))
            {
                lua_settop(L_, entry_top);
                return std::unexpected(MakeError(ErrorCode::kLengthOverflow, type, fidx, tag_offset));
            }
            // map 表：没有就建
            lua_rawgeti(L_, names, static_cast<lua_Integer>(fidx) + 1);
            lua_rawget(L_, obj);
            if (lua_isnil(L_, -1))
            {
                lua_pop(L_, 1);
                lua_createtable(L_, 0, 0);
                lua_pushvalue(L_, -1);
                lua_rawgeti(L_, names, static_cast<lua_Integer>(fidx) + 1);
                lua_insert(L_, -2);
                lua_rawset(L_, obj);
            }
            const int map_idx = lua_gettop(L_);

            // 占位：key / value
            lua_pushnil(L_);
            const int key_slot = lua_gettop(L_);
            lua_pushnil(L_);
            const int val_slot = lua_gettop(L_);

            Reader er{entry};
            std::uint32_t etag = 0;
            while (!er.Empty())
            {
                if (!er.ReadTag(etag))
                {
                    lua_settop(L_, entry_top);
                    return std::unexpected(MakeError(ErrorCode::kTruncated, type, fidx, tag_offset));
                }
                const std::uint32_t en = TagFieldNumber(etag);
                if (en == 1 && TagWireType(etag) == OpWireType(f->map_key_op))
                {
                    if (auto e = ReadScalar(L_, er, f->map_key_op, ctx); !e)
                    {
                        lua_settop(L_, entry_top);
                        return std::unexpected(e.error());
                    }
                    lua_replace(L_, key_slot);
                    continue;
                }
                if (en == 2 && (f->message_type != kNoIndex ? TagWireType(etag) == WireType::kLen
                                                            : TagWireType(etag) == OpWireType(f->map_value_op)))
                {
                    if (f->message_type != kNoIndex)
                    {
                        std::span<const std::byte> sub;
                        if (!er.ReadLengthDelimited(sub))
                        {
                            lua_settop(L_, entry_top);
                            return std::unexpected(MakeError(ErrorCode::kLengthOverflow, type, fidx, tag_offset));
                        }
                        Reader sr{sub};
                        if (auto e = DecodeMessage(f->message_type, sr, depth + 1); !e)
                        {
                            lua_settop(L_, entry_top);
                            return std::unexpected(e.error());
                        }
                    }
                    else if (auto e = ReadScalar(L_, er, f->map_value_op, ctx); !e)
                    {
                        lua_settop(L_, entry_top);
                        return std::unexpected(e.error());
                    }
                    lua_replace(L_, val_slot);
                    continue;
                }
                if (!er.Skip(TagWireType(etag)))
                {
                    lua_settop(L_, entry_top);
                    return std::unexpected(MakeError(ErrorCode::kTruncated, type, fidx, tag_offset));
                }
            }
            // 缺 key / value 时补默认值
            if (lua_isnil(L_, key_slot))
            {
                lua_pushnil(L_);
                lua_replace(L_, key_slot);
                PushDefault(L_, f->map_key_op);
                lua_replace(L_, key_slot);
            }
            if (lua_isnil(L_, val_slot))
            {
                if (f->message_type != kNoIndex)
                {
                    lua_createtable(L_, 0, 0); // 空子消息
                }
                else
                {
                    PushDefault(L_, f->map_value_op);
                }
                lua_replace(L_, val_slot);
            }
            lua_pushvalue(L_, key_slot);
            lua_pushvalue(L_, val_slot);
            lua_rawset(L_, map_idx);
            lua_settop(L_, map_idx); // 去掉占位与 map 表
            present.Mark(fidx);
            continue;
        }

        if (f->repeated && packed_form)
        {
            flush_cache();
            std::span<const std::byte> packed;
            if (!r.ReadLengthDelimited(packed))
            {
                lua_settop(L_, entry_top);
                return std::unexpected(MakeError(ErrorCode::kLengthOverflow, type, fidx, tag_offset));
            }
            Reader pr{packed};
            // 定长元素可以按字节数预分配数组部分；varint 用"每元素至少 1 字节"做上界
            std::size_t elem = 0;
            switch (OpWireType(f->op))
            {
                case WireType::kFixed32:
                    elem = 4;
                    break;
                case WireType::kFixed64:
                    elem = 8;
                    break;
                default:
                    elem = 1;
                    break;
            }
            lua_createtable(L_, static_cast<int>(packed.size() / elem), 0);
            lua_Integer n = 0;
            while (!pr.Empty())
            {
                if (auto e = ReadScalar(L_, pr, f->op, ctx); !e)
                {
                    lua_settop(L_, entry_top);
                    return std::unexpected(e.error());
                }
                lua_rawseti(L_, -2, ++n);
            }
            lua_rawgeti(L_, names, static_cast<lua_Integer>(fidx) + 1);
            lua_insert(L_, -2);
            lua_rawset(L_, obj);
            present.Mark(fidx);
            continue;
        }

        if (f->repeated)
        {
            // 非 packed：把数组表留在栈顶复用
            if (cached_field != fidx)
            {
                flush_cache();
                lua_rawgeti(L_, names, static_cast<lua_Integer>(fidx) + 1);
                lua_rawget(L_, obj);
                if (lua_isnil(L_, -1))
                {
                    lua_pop(L_, 1);
                    lua_createtable(L_, 0, 0);
                    lua_pushvalue(L_, -1);
                    lua_rawgeti(L_, names, static_cast<lua_Integer>(fidx) + 1);
                    lua_insert(L_, -2);
                    lua_rawset(L_, obj);
                }
                cached_field = fidx;
                cache_top = lua_gettop(L_); // 数组表在栈顶
            }
            if (f->op == Op::kMessage)
            {
                std::span<const std::byte> sub;
                if (!r.ReadLengthDelimited(sub))
                {
                    flush_cache();
                    lua_settop(L_, entry_top);
                    return std::unexpected(MakeError(ErrorCode::kLengthOverflow, type, fidx, tag_offset));
                }
                Reader sr{sub};
                if (auto e = DecodeMessage(f->message_type, sr, depth + 1); !e)
                {
                    flush_cache();
                    lua_settop(L_, entry_top);
                    return std::unexpected(e.error());
                }
            }
            else if (auto e = ReadScalar(L_, r, f->op, ctx); !e)
            {
                flush_cache();
                lua_settop(L_, entry_top);
                return std::unexpected(e.error());
            }
            lua_rawseti(L_, -2, lua_rawlen(L_, -2) + 1);
            present.Mark(fidx);
            continue;
        }

        // 单值
        flush_cache();
        if (f->op == Op::kMessage)
        {
            std::span<const std::byte> sub;
            if (!r.ReadLengthDelimited(sub))
            {
                lua_settop(L_, entry_top);
                return std::unexpected(MakeError(ErrorCode::kLengthOverflow, type, fidx, tag_offset));
            }
            Reader sr{sub};
            if (auto e = DecodeMessage(f->message_type, sr, depth + 1); !e)
            {
                lua_settop(L_, entry_top);
                return std::unexpected(e.error());
            }
        }
        else if (auto e = ReadScalar(L_, r, f->op, ctx); !e)
        {
            lua_settop(L_, entry_top);
            return std::unexpected(e.error());
        }

        // oneof：换成员时把前一个删掉（"只出现实际设置的成员"）
        if (f->oneof_index != kNoIndex && f->oneof_index < kMaxOneofs)
        {
            const std::uint32_t prev = oneof_set[f->oneof_index];
            if (prev != kNoIndex && prev != fidx)
            {
                lua_rawgeti(L_, names, static_cast<lua_Integer>(prev) + 1);
                lua_pushnil(L_);
                lua_rawset(L_, obj);
            }
            oneof_set[f->oneof_index] = fidx;
        }

        lua_rawgeti(L_, names, static_cast<lua_Integer>(fidx) + 1);
        lua_insert(L_, -2);
        lua_rawset(L_, obj);
        present.Mark(fidx);
    }

    flush_cache();

    // 隐式存在的标量：缺失时逐字段填默认值
    for (std::uint32_t i = 0; i < m.fields.size(); ++i)
    {
        const FieldInfo& f = m.fields[i];
        if (!NeedsDefaultFill(f))
            continue;
        if (present.tracked)
        {
            if (present.Has(i))
                continue;
        }
        else
        {
            // 兜底：字段数超过位图容量时逐字段查一次
            lua_rawgeti(L_, names, static_cast<lua_Integer>(i) + 1);
            lua_rawget(L_, obj);
            const bool is_nil = lua_isnil(L_, -1) != 0;
            lua_pop(L_, 1);
            if (!is_nil)
                continue;
        }
        lua_rawgeti(L_, names, static_cast<lua_Integer>(i) + 1);
        PushDefault(L_, f.op);
        lua_rawset(L_, obj);
    }

    // 收尾：留下 obj，去掉 names 与工作值
    lua_pushvalue(L_, obj);
    lua_replace(L_, names);
    lua_settop(L_, names);
    return {};
}

// ============================== 编码 ==============================

std::expected<void, Error> Codec::EncodeMessage(TypeId type, int index, unsigned depth)
{
    if (depth > kMaxDepth)
        return std::unexpected(MakeError(ErrorCode::kDepthExceeded, type));

    const int entry_top = lua_gettop(L_);
    index = lua_absindex(L_, index);
    if (lua_type(L_, index) != LUA_TTABLE)
        return std::unexpected(MakeError(ErrorCode::kTypeMismatch, type));

    const MessageInfo& m = schema_->Message(type);
    const Kit& kit = GetKit(type);

    if (lua_checkstack(L_, static_cast<int>(m.fields.size()) + 8) == 0)
        return std::unexpected(MakeError(ErrorCode::kDepthExceeded, type));

    lua_rawgeti(L_, LUA_REGISTRYINDEX, kit.names_ref);
    const int names = lua_gettop(L_);
    const unsigned base = static_cast<unsigned>(names);

    // ---- 1. 扫输入表：校验键、记位图 ----
    PresenceBits present;
    present.tracked = m.fields.size() <= kTrackedFields;
    std::array<std::uint32_t, kMaxOneofs> oneof_seen{};
    for (std::uint32_t i = 0; i < m.oneof_count && i < kMaxOneofs; ++i)
        oneof_seen[i] = kNoIndex;

    lua_pushnil(L_);
    while (lua_next(L_, index) != 0)
    {
        // key 在 -2，value 在 -1。**只对字符串键取指针**：数字键调 lua_tolstring 会原地改 key，破坏遍历。
        if (lua_type(L_, -2) != LUA_TSTRING)
        {
            lua_settop(L_, entry_top);
            return std::unexpected(MakeError(ErrorCode::kUnknownField, type));
        }
        const void* key_ptr = lua_topointer(L_, -2);
        std::size_t key_len = 0;
        const char* key_str = lua_tolstring(L_, -2, &key_len);

        std::uint32_t fidx = kNoIndex;
        if (const auto it = kit.by_ptr.find(key_ptr); it != kit.by_ptr.end())
        {
            fidx = it->second;
        }
        else if (key_len > kInternedMaxLen)
        {
            const std::string_view key{key_str, key_len};
            for (const auto& [name, idx] : kit.long_names)
            {
                if (name == key)
                {
                    fidx = idx;
                    break;
                }
            }
        }
        if (fidx == kNoIndex)
        {
            lua_settop(L_, entry_top);
            return std::unexpected(MakeError(ErrorCode::kUnknownField, type));
        }

        present.Mark(fidx);
        const FieldInfo& f = m.fields[fidx];
        if (f.oneof_index != kNoIndex && f.oneof_index < kMaxOneofs)
        {
            const std::uint32_t prev = oneof_seen[f.oneof_index];
            if (prev != kNoIndex && prev != fidx)
            {
                lua_settop(L_, entry_top);
                return std::unexpected(MakeError(ErrorCode::kOneofConflict, type, fidx));
            }
            oneof_seen[f.oneof_index] = fidx;
        }
        lua_pop(L_, 1); // 弹 value，留 key 给下一次 lua_next
    }

    // ---- 2. 按字段号序输出 ----
    for (std::uint32_t i = 0; i < m.fields.size(); ++i)
    {
        const FieldInfo& f = m.fields[i];
        if (present.tracked && !present.Has(i))
            continue;

        lua_rawgeti(L_, names, static_cast<lua_Integer>(i) + 1);
        lua_rawget(L_, index);
        if (lua_isnil(L_, -1))
        {
            lua_settop(L_, static_cast<int>(base));
            continue;
        }
        const int value = lua_gettop(L_);
        const Error ctx = Fail(ErrorCode::kTypeMismatch, type, i);

        if (f.repeated)
        {
            if (lua_type(L_, value) != LUA_TTABLE)
            {
                lua_settop(L_, entry_top);
                return std::unexpected(ctx);
            }
            if (f.op == Op::kMap)
            {
                // 空 map 不写。不能拿 rawlen 判空：map 是字符串键的表，长度为 0；用 lua_next 探一条。
                lua_pushnil(L_);
                if (lua_next(L_, value) == 0)
                {
                    lua_settop(L_, static_cast<int>(base));
                    continue;
                }
                for (;;)
                {
                    // 每条 entry 各自一个 LEN 字段：长度必须逐条回填，
                    // 否则会把多条 entry 包进同一个 LEN 里（单条 entry 时看不出来，两条就丢数据）。
                    writer_.WriteVarint(MakeTag(f.number, WireType::kLen));
                    const std::size_t at = writer_.ReserveLength();
                    const std::size_t before = writer_.Size();

                    // key 在 -2，value 在 -1
                    const int mv = lua_gettop(L_);
                    writer_.WriteVarint(MakeTag(1, OpWireType(f.map_key_op)));
                    if (auto e = WriteScalarBody(L_, writer_, f.map_key_op, mv - 1, ctx); !e)
                    {
                        lua_settop(L_, entry_top);
                        return std::unexpected(e.error());
                    }
                    writer_.WriteVarint(MakeTag(2, f.message_type != kNoIndex ? WireType::kLen
                                                                              : OpWireType(f.map_value_op)));
                    if (f.message_type != kNoIndex)
                    {
                        const std::size_t vat = writer_.ReserveLength();
                        const std::size_t vbefore = writer_.Size();
                        if (auto e = EncodeMessage(f.message_type, mv, depth + 1); !e)
                        {
                            lua_settop(L_, entry_top);
                            return std::unexpected(e.error());
                        }
                        writer_.BackfillLength(vat, writer_.Size() - vbefore);
                    }
                    else if (auto e = WriteScalarBody(L_, writer_, f.map_value_op, mv, ctx); !e)
                    {
                        lua_settop(L_, entry_top);
                        return std::unexpected(e.error());
                    }
                    writer_.BackfillLength(at, writer_.Size() - before);
                    lua_pop(L_, 1); // 弹 value，留 key
                    if (lua_next(L_, value) == 0)
                        break;
                }
                lua_settop(L_, static_cast<int>(base));
                continue;
            }
            const lua_Integer n = lua_rawlen(L_, value);
            if (n == 0)
            {
                lua_settop(L_, static_cast<int>(base)); // 空 repeated = 未设置，不写
                continue;
            }
            if (f.packed && OpIsPackable(f.op))
            {
                writer_.WriteVarint(MakeTag(f.number, WireType::kLen));
                const std::size_t at = writer_.ReserveLength();
                const std::size_t before = writer_.Size();
                for (lua_Integer k = 1; k <= n; ++k)
                {
                    lua_rawgeti(L_, value, k);
                    if (lua_isnil(L_, -1))
                    {
                        lua_settop(L_, entry_top);
                        return std::unexpected(ctx); // repeated 有空洞
                    }
                    if (auto e = WriteScalarBody(L_, writer_, f.op, lua_gettop(L_), ctx); !e)
                    {
                        lua_settop(L_, entry_top);
                        return std::unexpected(e.error());
                    }
                    lua_pop(L_, 1);
                }
                writer_.BackfillLength(at, writer_.Size() - before);
                lua_settop(L_, static_cast<int>(base));
                continue;
            }
            for (lua_Integer k = 1; k <= n; ++k)
            {
                lua_rawgeti(L_, value, k);
                if (lua_isnil(L_, -1))
                {
                    lua_settop(L_, entry_top);
                    return std::unexpected(ctx);
                }
                const int elem = lua_gettop(L_);
                if (f.op == Op::kMessage)
                {
                    writer_.WriteVarint(MakeTag(f.number, WireType::kLen));
                    const std::size_t at = writer_.ReserveLength();
                    const std::size_t before = writer_.Size();
                    if (auto e = EncodeMessage(f.message_type, elem, depth + 1); !e)
                    {
                        lua_settop(L_, entry_top);
                        return std::unexpected(e.error());
                    }
                    writer_.BackfillLength(at, writer_.Size() - before);
                }
                else
                {
                    writer_.WriteVarint(MakeTag(f.number, OpWireType(f.op)));
                    if (auto e = WriteScalarBody(L_, writer_, f.op, elem, ctx); !e)
                    {
                        lua_settop(L_, entry_top);
                        return std::unexpected(e.error());
                    }
                }
                lua_pop(L_, 1); // 只弹元素：value（数组表）必须留着给下一轮 lua_rawgeti 用
            }
            continue;
        }

        // 单值
        if (f.op == Op::kMessage)
        {
            if (lua_type(L_, value) != LUA_TTABLE)
            {
                lua_settop(L_, entry_top);
                return std::unexpected(ctx);
            }
            writer_.WriteVarint(MakeTag(f.number, WireType::kLen));
            const std::size_t at = writer_.ReserveLength();
            const std::size_t before = writer_.Size();
            if (auto e = EncodeMessage(f.message_type, value, depth + 1); !e)
            {
                lua_settop(L_, entry_top);
                return std::unexpected(e.error());
            }
            writer_.BackfillLength(at, writer_.Size() - before);
            lua_settop(L_, static_cast<int>(base));
            continue;
        }

        // 隐式标量等于默认值 → 不写。注意先看 Lua 类型：lua_tointeger('x') 也是 0，
        // 否则 {i32 = 'x'} 会被当成"等于默认值"静默跳过，而不是报类型不符。
        const bool type_ok = [&]
        {
            switch (f.op)
            {
                case Op::kBool:
                    return lua_type(L_, value) == LUA_TBOOLEAN;
                case Op::kString:
                case Op::kBytes:
                    return lua_type(L_, value) == LUA_TSTRING;
                default:
                    return lua_type(L_, value) == LUA_TNUMBER;
            }
        }();
        if (NeedsDefaultFill(f) && type_ok && IsDefaultOnStack(L_, value, f.op))
        {
            lua_settop(L_, static_cast<int>(base));
            continue;
        }

        writer_.WriteVarint(MakeTag(f.number, OpWireType(f.op)));
        if (auto e = WriteScalarBody(L_, writer_, f.op, value, ctx); !e)
        {
            lua_settop(L_, entry_top);
            return std::unexpected(e.error());
        }
        lua_settop(L_, static_cast<int>(base));
    }

    lua_settop(L_, entry_top);
    return {};
}

} // namespace breeze::lua_pb
