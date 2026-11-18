// schema 层：FileDescriptorSet → Schema。描述符由构建期 protoc 生成，路径从编译宏进来。
// 无测试框架，失败即 abort。

#include "base/lua_pb/schema.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace breeze::lua_pb;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

#ifndef LUA_PB_TEST_DESCRIPTOR
#error "需要 -DLUA_PB_TEST_DESCRIPTOR=<path>"
#endif
#ifndef LUA_PB_TEST_EXTRA_DESCRIPTOR
#error "需要 -DLUA_PB_TEST_EXTRA_DESCRIPTOR=<path>"
#endif
#ifndef LUA_PB_TEST_REJECT_DESCRIPTOR
#error "需要 -DLUA_PB_TEST_REJECT_DESCRIPTOR=<path>"
#endif

namespace
{

std::vector<std::byte> ReadFile(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "打不开描述符：%s\n", path);
        std::abort();
    }
    std::vector<std::byte> data;
    char buf[4096];
    while (in.read(buf, sizeof(buf)) || in.gcount() > 0)
        for (std::streamsize i = 0; i < in.gcount(); ++i)
            data.push_back(static_cast<std::byte>(static_cast<unsigned char>(buf[i])));
    return data;
}

Schema MustLoad(std::span<const std::byte> bytes)
{
    auto s = Schema::Load(bytes);
    if (!s)
    {
        const Error& e = s.error();
        std::fprintf(stderr, "Schema::Load 失败：%s type=%u field=%u offset=%u\n",
                     std::string(ErrorCodeName(e.code)).c_str(), e.type_id, e.field_idx, e.offset);
        std::abort();
    }
    return std::move(*s);
}

void TestTypesAndNames()
{
    const auto bytes = ReadFile(LUA_PB_TEST_DESCRIPTOR);
    Schema s = MustLoad(bytes);

    const TypeId scalars = s.FindType("lua_pb_test.Scalars");
    const TypeId inner = s.FindType("lua_pb_test.Inner");
    CHECK(scalars != kNoIndex);
    CHECK(inner != kNoIndex);
    CHECK(s.FindType(".lua_pb_test.Inner") == inner);  // 前导点也认
    CHECK(s.FindType("lua_pb_test.Nope") == kNoIndex);
    CHECK(s.MessageName(inner) == "lua_pb_test.Inner");

    // 每种标量类型的 Op
    const MessageInfo& m = s.Message(scalars);
    const auto op_of = [&](std::string_view name)
    {
        const FieldInfo* f = Schema::FindField(m, name);
        CHECK(f != nullptr);
        return f->op;
    };
    CHECK(op_of("i32") == Op::kInt32);
    CHECK(op_of("i64") == Op::kInt64);
    CHECK(op_of("u32") == Op::kUint32);
    CHECK(op_of("u64") == Op::kUint64);
    CHECK(op_of("s32") == Op::kSint32);
    CHECK(op_of("s64") == Op::kSint64);
    CHECK(op_of("fx32") == Op::kFixed32);
    CHECK(op_of("fx64") == Op::kFixed64);
    CHECK(op_of("sfx32") == Op::kSfixed32);
    CHECK(op_of("sfx64") == Op::kSfixed64);
    CHECK(op_of("fl") == Op::kFloat);
    CHECK(op_of("db") == Op::kDouble);
    CHECK(op_of("ok") == Op::kBool);
    CHECK(op_of("name") == Op::kString);
    CHECK(op_of("blob") == Op::kBytes);
    CHECK(op_of("color") == Op::kEnum);

    // 超长字段名（>40 字节）也要能查到
    CHECK(Schema::FindField(m, "field_with_a_really_long_name_exceeding_forty_bytes") != nullptr);
    CHECK(Schema::FindField(m, "no_such_field") == nullptr);

    // 按字段号查（Schemas 字段号 1..17 稠密，走 dense 数组）
    CHECK(!m.dense.empty());
    for (std::uint32_t n = 1; n <= 17; ++n)
        CHECK(Schema::FindField(m, n) != nullptr);
    CHECK(Schema::FindField(m, 18) == nullptr);
    CHECK(Schema::FindField(m, 0) == nullptr);
}

void TestNestedAndRepeated()
{
    const auto bytes = ReadFile(LUA_PB_TEST_DESCRIPTOR);
    Schema s = MustLoad(bytes);

    const TypeId inner = s.FindType("lua_pb_test.Inner");
    const TypeId nested = s.FindType("lua_pb_test.Nested");
    const MessageInfo& n = s.Message(nested);

    const FieldInfo* one = Schema::FindField(n, "inner");
    CHECK(one != nullptr);
    CHECK(one->op == Op::kMessage);
    CHECK(one->message_type == inner);
    CHECK(!one->repeated);
    CHECK(one->presence == Presence::kExplicit); // message 字段是显式存在

    const TypeId repeated = s.FindType("lua_pb_test.Repeated");
    const MessageInfo& r = s.Message(repeated);
    const FieldInfo* nums = Schema::FindField(r, "nums");
    CHECK(nums != nullptr && nums->repeated && nums->packed && nums->op == Op::kInt32);
    const FieldInfo* names = Schema::FindField(r, "names");
    CHECK(names != nullptr && names->repeated && !names->packed && names->op == Op::kString);
    const FieldInfo* flags = Schema::FindField(r, "flags");
    CHECK(flags != nullptr && flags->repeated && flags->packed && flags->op == Op::kBool);
    const FieldInfo* items = Schema::FindField(r, "items");
    CHECK(items != nullptr && items->repeated && !items->packed && items->op == Op::kMessage);
    CHECK(items->message_type == inner);
}

void TestMaps()
{
    const auto bytes = ReadFile(LUA_PB_TEST_DESCRIPTOR);
    Schema s = MustLoad(bytes);

    const MessageInfo& m = s.Message(s.FindType("lua_pb_test.Maps"));

    const FieldInfo* counts = Schema::FindField(m, "counts");
    CHECK(counts != nullptr);
    CHECK(counts->op == Op::kMap);
    CHECK(counts->map_key_op == Op::kString);
    CHECK(counts->map_value_op == Op::kInt32);
    CHECK(counts->message_type == kNoIndex);
    CHECK(counts->repeated); // map 在描述符里是 repeated 的 entry

    const FieldInfo* objs = Schema::FindField(m, "objs");
    CHECK(objs != nullptr && objs->op == Op::kMap);
    CHECK(objs->map_key_op == Op::kInt32);
    CHECK(objs->message_type == s.FindType("lua_pb_test.Inner")); // value 是消息

    const FieldInfo* labels = Schema::FindField(m, "labels");
    CHECK(labels != nullptr && labels->op == Op::kMap);
    CHECK(labels->map_key_op == Op::kInt64);
    CHECK(labels->map_value_op == Op::kString);

    // map entry 本身不该作为类型被登记
    CHECK(s.FindType("lua_pb_test.Maps.CountsEntry") == kNoIndex);
}

void TestOneofAndOptional()
{
    const auto bytes = ReadFile(LUA_PB_TEST_DESCRIPTOR);
    Schema s = MustLoad(bytes);

    const MessageInfo& o = s.Message(s.FindType("lua_pb_test.Oneofs"));
    CHECK(o.oneof_count == 1);
    for (std::string_view name : {"a", "b", "c"})
    {
        const FieldInfo* f = Schema::FindField(o, name);
        CHECK(f != nullptr);
        CHECK(f->oneof_index == 0);
        CHECK(f->presence == Presence::kExplicit);
    }
    const FieldInfo* outside = Schema::FindField(o, "outside");
    CHECK(outside != nullptr);
    CHECK(outside->oneof_index == kNoIndex);
    CHECK(outside->presence == Presence::kImplicit);

    // proto3 optional：显式存在，但合成 oneof 不算 oneof
    const MessageInfo& opt = s.Message(s.FindType("lua_pb_test.Optionals"));
    CHECK(opt.oneof_count == 0);
    for (std::string_view name : {"maybe", "maybe_str", "maybe_color"})
    {
        const FieldInfo* f = Schema::FindField(opt, name);
        CHECK(f != nullptr);
        CHECK(f->presence == Presence::kExplicit);
        CHECK(f->oneof_index == kNoIndex);
    }
}

void TestRecursive()
{
    const auto bytes = ReadFile(LUA_PB_TEST_DESCRIPTOR);
    Schema s = MustLoad(bytes);
    const TypeId deep = s.FindType("lua_pb_test.Deep");
    const MessageInfo& d = s.Message(deep);
    const FieldInfo* next = Schema::FindField(d, "next");
    CHECK(next != nullptr);
    CHECK(next->message_type == deep); // 自引用
}

void TestConcatenatedDescriptorsAndDedupe()
{
    auto a = ReadFile(LUA_PB_TEST_DESCRIPTOR);
    auto b = ReadFile(LUA_PB_TEST_EXTRA_DESCRIPTOR);

    Schema only_a = MustLoad(a);
    const std::size_t count_a = only_a.MessageCount();

    // a + b：两个文件都能用
    std::vector<std::byte> ab = a;
    ab.insert(ab.end(), b.begin(), b.end());
    Schema both = MustLoad(ab);
    CHECK(both.FindType("lua_pb_test.Scalars") != kNoIndex);
    CHECK(both.FindType("lua_pb_test.extra.Extra") != kNoIndex);
    CHECK(both.MessageCount() == count_a + 1);

    // a + b + b：同名文件去重，不重复登记
    std::vector<std::byte> abb = ab;
    abb.insert(abb.end(), b.begin(), b.end());
    Schema dedup = MustLoad(abb);
    CHECK(dedup.MessageCount() == count_a + 1);

    // 与 only_a 相同的内容加载两次，类型 id 保持一致（可复现）
    Schema again = MustLoad(a);
    CHECK(again.FindType("lua_pb_test.Scalars") == only_a.FindType("lua_pb_test.Scalars"));
}

void TestRejects()
{
    // 只有 proto2 文件 → kUnsupportedSyntax
    {
        auto bytes = ReadFile(LUA_PB_TEST_REJECT_DESCRIPTOR);
        auto s = Schema::Load(bytes);
        CHECK(!s);
        CHECK(s.error().code == ErrorCode::kUnsupportedSyntax);
    }
    // 空输入 → 没有 proto3 文件
    {
        auto s = Schema::Load({});
        CHECK(!s);
        CHECK(s.error().code == ErrorCode::kUnsupportedSyntax);
    }
    // 垃圾字节 → 坏描述符
    {
        const std::byte junk[2] = {std::byte{0xFF}, std::byte{0xFF}};
        auto s = Schema::Load(std::span<const std::byte>{junk, 2});
        CHECK(!s);
        CHECK(s.error().code == ErrorCode::kBadDescriptor);
    }
    // 截断在文件长度为前缀中间 → 坏描述符
    {
        auto bytes = ReadFile(LUA_PB_TEST_DESCRIPTOR);
        CHECK(bytes.size() > 3);
        auto s = Schema::Load(std::span<const std::byte>{bytes.data(), bytes.size() - 1});
        CHECK(!s);
        CHECK(s.error().code == ErrorCode::kBadDescriptor);
    }
}

void TestDescribe()
{
    const auto bytes = ReadFile(LUA_PB_TEST_DESCRIPTOR);
    Schema s = MustLoad(bytes);
    const MessageInfo& m = s.Message(s.FindType("lua_pb_test.Scalars"));
    const FieldInfo* f = Schema::FindField(m, "name");
    CHECK(f != nullptr);
    const std::uint32_t idx = static_cast<std::uint32_t>(f - m.fields.data());

    const std::string text = s.Describe(MakeError(ErrorCode::kInvalidVarint, s.FindType("lua_pb_test.Scalars"), idx, 12));
    CHECK(text == "lua_pb_test.Scalars.name @12: invalid varint");

    const std::string bare = s.Describe(MakeError(ErrorCode::kUnsupportedSyntax));
    CHECK(bare == "unsupported syntax");
}

} // namespace

int main()
{
    TestTypesAndNames();
    TestNestedAndRepeated();
    TestMaps();
    TestOneofAndOptional();
    TestRecursive();
    TestConcatenatedDescriptorsAndDedupe();
    TestRejects();
    TestDescribe();
    std::printf("lua_pb_schema_test: ok\n");
    return 0;
}
