# Lua protobuf 库设计（`base/lua_pb`）

**状态**：设计阶段，尚无代码。本文是 `base/lua_pb` 的设计基线；实现分 P1–P4 推进（见 §13）。

相关文档：

- RPC/IDL 的运行时分层与 C++/Lua 混合 codec：[RPC与IDL设计.md](RPC与IDL设计.md)
- 绑定库的 API / 错误 / 线程规则：[Lua绑定库设计.md](Lua绑定库设计.md)
- Lua 运行时与 worker：[Lua脚本框架设计.md](Lua脚本框架设计.md)

---

## 0. 结论速查

1. **只做库**：描述符 → 编译后的 `Schema` → Lua table ↔ wire。不含 msg_id / 信封 / RPC / 路由 / 文件 IO。
2. **只支持 proto3**、**只支持 Lua 5.5.1**、**C++26**。
3. **三层**：`wire`（纯字节）→ `schema`（纯 C++，不碰 Lua）→ `binding`（Lua）。
4. **Schema 编译后不可变，跨线程只读共享**；每个 `lua_State` 一个 `Codec`（一线程一 state）。
5. **热路径只用 raw Lua API**：编解码期间不触发元方法、不跑用户代码。
6. **核心无 C++ 异常、无 `longjmp`**：`std::expected<T, Error>`；只在最外层转一次 Lua 错误。
7. **编码遍历用混合方案**：`lua_next` + per-state 指针哈希 + 位图，按字段号序输出（确定）。
8. **交付方式 b**：构建期产出 `run/proto/*.pb`，运行期由框架读一次交给 `Schema::Load(span)`；本库不碰文件系统。
9. **严格 / 宽容分界**：自己产生的输入（Lua 表 → 编码）严格；网络来的字节（解码）宽容。
10. **零配置**：没有 Options，行为固定。

## 1. 位置与命名

- 目录：`src/cpp/base/lua_pb/`，与 `base/lua_binding` 同级。
- 命名空间：`breeze::lua_pb`；文件 snake_case；`#pragma once`；注释用简体中文，只记约束、代价与失败模式。
- 不加入 `breeze_lua_binding`，也不进 `lua_binding.h`：codec 是独立包（`base/lua_binding/lua_binding.h:12` 的边界）。新库可以依赖 `breeze_lua_binding`。
- Lua 模块名：`pb`（`require("pb")`），与 starwing 的 `pb` 同名；C++ 侧经 `package.loaded["pb"]` 注册。

## 2. 交付与 proto 布局（方案 b）

```text
构建期： proto/*.proto  --protoc-->  run/proto/descriptor.pb   （/run/ 已忽略）
运行期： 框架读一次文件 → Schema::Load(span) → OpenPb(L, schema) → require("pb")
```

- 仓库顶层 `proto/` 只放 `.proto` 源（提交）；配置 schema 仍在 `config/schema/`。
- CMake 函数 `breeze_proto_descriptor`（`src/cpp/CMakeLists.txt`）：`proto/*.proto` → `run/proto/descriptor.pb`，`--include_imports`，protoc 按 `-DBREEZE_PROTOC` > PATH 查找；目录为空则不建目标。
- **读文件在框架层**（`RpcModule` 或启动代码），不在库内：库的唯一入口是 `Schema::Load(span)`。
- 描述符路径从 breeze 配置（`config/*.yaml`）读，不用环境变量。
- **失败即启动失败**：描述符缺失 / 空 / 不可解析时带原因退出；v1 不做版本号校验（描述符与二进制同一次构建、同批部署），只校验非空、可解析、大小上限。
- 不热更：改 IDL 走重启。
- **protoc 版本**：测试 proto 用了 proto3 optional，需要 **protoc ≥ 3.15**；更老的版本（如 Ubuntu 22.04 apt 的 3.12）要显式 `--experimental_allow_proto3_optional`，CMake 里已按 `protoc --version` 自动判断加不加这个旗标。
- **Windows 上的坑**：protoc 用 ANSI 解析 argv，绝对路径里的非 ASCII（本仓库在"源代码"目录下）会变成乱码，报 "directory does not exist"。所以 `breeze_proto_descriptor` 一律**在 `proto_dir` 下用相对路径**调用 protoc（quanta 的 `proto/build.bat` 也是靠"切到 proto 目录 + 相对文件名"规避同一个问题）。

## 3. 架构与文件

| 文件 | 内容 |
|---|---|
| `wire.h` | varint（≤10 字节）、zigzag、fixed32/64（小端，`static_assert`）、带上界的 `Reader`、可回填长度的 `Writer`（1.5–2× 扩容）。仓库内无现成实现，自写；不复用 `base/buffer`（游标 / 不可变切片，不支持长度回填）。`Writer` 用裸分配而不是 `std::vector`：**`vector::resize` 会把新增元素清零**，用 `resize` 当 Commit 会覆盖 "Reserve 后写指针" 写进去的字节 |
| `error.h` | `Error`（平凡可析构 POD）+ `Describe(Error, Schema)` |
| `schema.h/.cpp` | `Schema`（不可变）、`Schema::Load(span)`：自带 wire reader 读 `FileDescriptorSet`，不依赖 libprotobuf（参考 `tools/configgen/configgen.py:95-250`） |
| `codec.h/.cpp` | `Codec`：绑定 `lua_State` + 共享 `Schema`；`Decode(TypeId, span)` 压入 table；`Encode(TypeId, idx)` 返回输出缓冲视图（下次调用前有效） |
| `lua_pb.h/.cpp` | 总头 + `OpenPb(LuaStateView, std::shared_ptr<const Schema>)`：建 `Codec` userdata（`__gc`），注册 `package.loaded["pb"]` |
| 测试 | `lua_pb_wire_test`、`lua_pb_schema_test`、`lua_pb_codec_test`、`lua_pb_hostile_test`、`-fno-exceptions` 变体；本地 `CHECK` 宏，风格同 `lua_ext_test.cpp` |
| `testdata/` | 自包含：`*.proto`（提交）+ 构建期生成的描述符（进本库 build 目录）+ 黄金字节 |
| `src/cpp/bench/lua_pb_bench.cpp` | 照 `lua_binding_bench.cpp` 的 `RunCase`（预热 + 5 轮中位数，`ns/op`） |

CMake：`breeze_lua_pb`（链 `breeze_lua_binding`，`cxx_std_26`，`-Wall -Wextra`）+ 测试 `foreach` + `BREEZE_BUILD_LUA_PB_BENCH`，照 `src/cpp/CMakeLists.txt:255-300, 767-772`。

## 4. Schema 编译

- 描述符只解析必要字段：file{name, package, message_type, enum_type, syntax}、message{name, field, nested_type, enum_type, options.map_entry}、field{name, number, type, label, type_name, oneof_index, proto3_optional}、enum{name}（枚举只需"存在"，不解析值）。
- `type_name` 是 protoc 给出的全限定名（`.pkg.Msg`），去前导点直接查表，不做作用域解析。
- 每条消息：字段按 number 排序；**number 稠密（`max_number < 4×字段数`）用数组直取，否则二分**；每字段预算 `Op` 枚举（varint32/64、zigzag32/64、bool、enum、fixed32/64、float、double、string、bytes、message、map）、预编码 tag、presence（隐式 / 显式 = optional · oneof 成员 · message）。分派用 `switch`，不用虚函数。
- 类型全名 → `TypeId` 用有序 vector + `lower_bound`（Schema 只建一次，不依赖 `flat_map` 的三端支持）。
- 多个 `.pb` 拼接字节即可（protobuf repeated 的拼接语义），按 file name 去重。
- 建库期报错：引用解析失败、`syntax` 非 `"proto3"`、group、嵌套过深。

## 5. 解码

- `lua_createtable(L, 0, min(字段数, 32))`——填默认值后表接近稠密；**绝不按不可信长度预分配**（常量 32 由 P4 基准校准）。
- 字段名取自**每 state 懒建的 per-type "kit" 表**（`[1..n]` 是字段名字符串），`lua_rawgeti` 取名，不做字符串哈希、不堆分配；写入 `lua_rawset`。
- repeated：packed 定长元素按字节数预分配数组；**packed 与非 packed 两种线上形式都接受**；非 packed 元素用"当前 repeated 字段"单槽缓存（连续出现时数组留在栈顶，非连续再出现退化为 `rawget + rawlen`），无 per-message 堆分配。
- map：条目内解 key/value，缺失补默认，`rawset` 进 map 表。
- 未知行为：schema 里没有的 field number → **校验后跳过**（前向兼容）；wire type 与字段不匹配 → 同样按未知字段跳过；未知字段不保留（需要透传时走 RpcModule 的"不解码"路径）。int32/uint32 的超范围 varint 按协议截断。
- 缺省值：**隐式存在的标量缺失时逐字段填 0 / false / `""`**；optional / oneof / message / repeated / map 缺失为 nil。填默认值让 `pairs`、深拷贝、JSON 序列化都能看到（元表方案留待 P4 证明收益，且要先解决"默认值经深拷贝丢失"）。
- 安全上限见 §9。

## 6. 编码

遍历输入表用**混合方案**（等价于 protobuf 的 deterministic 模式）：

1. `lua_next` 遍历，代价只和实际字段数有关；**先判 `lua_type`，只有 `LUA_TSTRING` 才取串**（starwing 与 quanta 都在这里对数字 key 直接 `lua_tolstring`，会原地改 key，违反 Lua 手册对 `lua_next` 的约束）。
2. 按 key 查字段：Lua 5.5.1 里 ≤40 字节的短字符串驻留（`LUAI_MAXSHORTLEN=40`），用 `lua_topointer` 取 `TString*`（`lapi.c:492-505`）查指针哈希；≥41 字节的字段名回退内容比较。**指针哈希必须 per-state**（`TString*` 属于各自的 `lua_State`），与 kit 表一起懒建。
3. 未命中的键**报错**（未知字段名 = 拼写错误，与 protobuf text/JSON 默认行为一致）；**这条只作用于消息字段，不作用于 map 的 key**——map key 是数据，按类型解。命中的字段用位图记下。
4. 按字段号序遍历位图（字段已按 number 排序），`rawgeti + rawget` 取值写出 → **输出确定**，且只和实际字段数有关。

写入规则：

- 隐式存在标量等于默认值 → 不写；**空 repeated / 空 map 表 = 未设置，不写**（同 protobuf）。
- **显式存在字段**（optional / oneof 成员 / message）：表里放 `0` 就是"存在且为 0"（wire 写 0，presence 置位）；不放或 nil 才是未设置。
- repeated 数值 / bool / enum 一律 packed；`lua_rawlen + rawgeti`，空洞报错。map 用 `lua_next`（条目顺序不确定，协议允许）。
- **同一 oneof 出现多个成员 → 报错**（对齐 protobuf text/JSON 的 `FORBID_SINGULAR_OVERWRITES`；starwing 不校验）。
- packed 形式：proto3 数值 / bool / enum 一律按 packed 编码，忽略对方 `[packed=false]`（我们已去掉 `[packed]` 选项）；解码侧两种形式都收，互通无碍。
- 嵌套长度**预留 1 字节回填**，长度 ≥128 才 `memmove`，避免两遍遍历 Lua 表。
- 严格类型：整数字段要求 number 且能无损转整数（拒绝字符串）、越界报错；bool 只收 boolean；string / bytes 只收 string；float / double 收 number；enum 只收整数。表自引用由深度上限拦住。
- 输出缓冲 `Codec` 自有、跨调用复用（**不用 `thread_local`**）；Lua 层 `lua_pushlstring` 复制一次，是否换 `lua_pushexternalstring` 由 P4 决定。

## 7. 值与枚举映射

| protobuf | Lua |
|---|---|
| int32 / int64 / sint / fixed | integer |
| uint32 | integer |
| **uint64** | integer；**≥2^63 回绕成 `lua_Integer`**（位精确、往返无损）。要有符号语义用 `math.ult` / `string.format("%u")`。不做 string 模式 |
| float / double | number |
| bool | boolean |
| string / bytes | string（二进制安全，两者在 Lua 侧不可区分） |
| enum | integer（双向都是整数；名字由 IDL 生成器出常量） |
| repeated | array，1..n |
| map | table |
| nested message | table |
| oneof | 只出现实际设置的成员 |
| optional | 未设置 = nil |

字段名沿用 proto 的 `snake_case`（生成器与本库一致）。

## 8. 错误模型

- **C++ API**：`std::expected<T, Error>`。`Error` 是平凡可析构 POD（`code`、`type_id`、`field_idx`、`offset`），跨 `longjmp` 安全；文本用 `Describe(Error, Schema)` 只在最外层格式化一次。
- **Lua 表面**：`pb.encode` / `pb.decode` 失败抛 `luaL_error`，消息形如 `decode pkg.Msg.field @12: invalid varint`。与 starwing / quanta 一致（它们的 encode/decode 都抛错）。
- **全程没有 C++ 异常**，也不在核心 `longjmp`：`-fno-exceptions` 可编。最外层那一帧只有平凡可析构的局部变量，这一点照 `base/lua_binding` 的两阶段 trampoline（`trampoline.h:151-196`、`error.cpp:9-19`）。
- 生产路径（RpcModule 适配器）走 C++ API，把 `Error` 映射成 `kDecodeFailed` 之类的框架错误码，**不产生 Lua 错误、不需要 `pcall`**。
- 错误码（初稿）：`kBadDescriptor`、`kUnknownType`、`kTruncated`、`kInvalidVarint`、`kLengthOverflow`、`kDepthExceeded`、`kFieldZero`、`kTypeMismatch`、`kUnknownField`、`kOneofConflict`、`kMissingRequiredValue`（encode 侧字段类型不符用 `kTypeMismatch` 并带 `field_idx`）。

## 9. 上限（固定值，不配置）

| 限制 | 本库 | 其他库 |
|---|---|---|
| 嵌套深度 | **100**（`kMaxDepth`） | protobuf C++ `kDefaultRecursionLimit = 100`；upb `kUpb_WireFormat_DefaultDepthLimit = 100`（可配 1–65535）；starwing `LPB_NESTLIMIT = 1000`；sproto 64；lua-cjson 1000 |
| varint | **≤10 字节** | 各家一致（64 位 spec 上限） |
| field number 0 | **拒绝** | protobuf 规范 |
| 每个 LEN 长度 | **与剩余字节做溢出安全比较** | protobuf 用 EPS 拷贝流做边界检查 |
| 输入总字节 | **不由库限**，交调用方（RPC 层有帧长上限） | protobuf C++ 默认 `INT_MAX`（约 2 GB） |
| Lua 栈 | **每层 `lua_checkstack`** | starwing / cjson 同 |
| repeated 元素数 | 不单独限（受字节数自然约束） | 无库单独限；lua-cmsgpack 只限 encode 嵌套 16 |

深度取 100 是因为 protobuf 自己的 C++ 与 upb 都是 100，是最广共识值。已知放大（1 MB packed bool → 约 16 MB Lua 数组）由沙箱内存上限兜底。

## 10. 与框架的边界

| 属于本库 | 不属于本库（框架） |
|---|---|
| 描述符字节 → `Schema` | 读描述符文件、路径配置、启动失败处理 |
| Lua table ↔ wire | 消息号、信封、RPC 挂起表、路由、超时 |
| `Codec` 的 per-state 状态 | `msg_id` → handler 注册、`audience`、`route_key` |
| 编解码错误（结构化） | 错误码到框架 `Status` 的映射、日志与限频 |

`RpcModule` 的 Lua 适配器直接调 C++ API；`require("pb")` 只给脚本 / worker / 测试用。worker 线程不能直接发起或处理 RPC（沿用 `docs/RPC与IDL设计.md` §6）。

## 11. 测试

- **wire**：varint 边界（0 / 127 / 128 / 2^64-1）、10 字节上限、zigzag、溢出安全比较、回填长度的 127/128 边界。用 `constexpr` + `static_assert` 覆盖纯函数部分。
- **schema**：proto3 / 嵌套 / map / oneof / optional / enum 引用 / 拼接去重 / 引用失败 / proto2 · edition · group 报错 / 描述符缺失与截断。
- **codec**：往返、黄金字节（与 `protoc` 生成的字节比对，protoc 输出进 testdata）、多 state 各自命中、长字段名回退、稀疏与稠密。
- **严格项**：未知字段名报错、类型不符报错、同一 oneof 两成员报错。
- **表语义**：空 repeated / 空 map 不写、optional 放 `0` 表达 presence、map key 不参与字段名匹配。
- **恶意输入**：截断、超长 LEN、深度炸弹、field 0、乱序 repeated、wire type 错配、map 缺 key/value。
- **回归清单**（来自 quanta `luapb` 的实际缺陷）：packed repeated 入站、oneof 往返、数字 key 不被转串、负 enum、uint64 高位、编码字节确定。
- 预设：debug + apicheck、asan、tsan（**跨线程共享 Schema、各线程独立 Codec**）、`-fno-exceptions` 变体；`tools/check_includes.py`。

## 12. 基准

`lua_pb_bench`：64B 标量 / 1KB 嵌套 + repeated / 16KB packed + bytes；量 `ns/op`、分配次数、GC 次数。临时 CMake 里并排对比 starwing `lua-protobuf` 与 quanta `luapb`（只用于对比，不进版本库）。据数据校准预分配常量、决定 `lua_pushexternalstring`、验证混合遍历对稠密消息是否仍有净收益。**不预先承诺数字。**

## 13. 阶段

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 本文档 + `proto/` 布局 + CMake `breeze_proto_descriptor` + C++26 探针 + `pb.c` 核对 | **已完成** |
| P1 | `wire.h`、`Schema::Load` 及测试 | **已完成** |
| P2 | decode + encode + 往返 + 黄金字节 + 恶意输入 + 混合遍历与表语义专项 | **已完成** |
| P3 | `Codec` / `OpenPb` / `require("pb")`、`-fno-exceptions` 变体、apicheck | **已完成**（asan / tsan 本机跑不了，见 §20） |
| P4 | 基准与对比，参数定稿 | **已完成**（见 §21） |

## 14. 明确不做

JSON / 文本格式；`Any` / `Timestamp` 等 well-known type 特判（按普通消息处理）；extension；proto2 group；editions；未知字段保留；hooks；`pb.slice` / `pb.buffer` 低层 API；`pb.load`（Lua 不能加载 schema）；文件 IO；UTF-8 校验；消息 userdata / 类对象（一律普通 table）；msg_id / 信封 / RPC；运行时可配选项。

## 15. 决策记录（与同类库对照）

| 议题 | 本库 | starwing `lua-protobuf` | quanta `luapb` | protobuf 官方 |
|---|---|---|---|---|
| 协议范围 | 只 proto3 | proto2 / proto3 | proto2 / proto3 | proto2 / proto3 / editions |
| 描述符来源 | 构建期 `run/proto/*.pb`，框架读一次 | 运行期 `pb.load(bytes)`（`io.open` 读文件或 `protoc.lua`） | 运行期 `protobuf.loadfile()`，路径来自 `QUANTA_PROTO_FILE` | 生成代码内嵌（C++/Go/Java/Python） |
| 缺省值 | 逐字段填 | `auto_default_values` 对 proto3 逐字段拷贝（另有 `use_default_metatable`） | 每消息遍历哈希表填 | 访问器返回默认 |
| 编码遍历 | 混合（`lua_next` + 指针哈希 + 位图），按字段号序输出 | 默认 `lua_next`（顺序不定），`encode_order` 按字段号 | `pairs` 序 | 反射默认遍历已设置字段，deterministic 需显式开 |
| 未知字段名（编码） | 报错 | 静默忽略 | — | text / JSON 解析器默认报错 |
| 同一 oneof 多成员 | 报错 | 不校验 | 不校验 | text / JSON 解析器报错（`FORBID_SINGULAR_OVERWRITES`） |
| 未知 field number（解码） | 跳过 | 跳过（跳过失败才报错） | 跳过 | 跳过；C++ 收进 unknown field set |
| uint64 ≥2^63 | 回绕成 integer | 默认回绕；string 模式只对超出 `[INT_MIN, UINT_MAX]` 的值加 `#` 前缀 | 回绕 | 由语言映射决定 |
| 嵌套上限 | 100 | 1000 | 无 | 100（C++ / upb） |
| 异常 | 无 C++ 异常，核心不 `longjmp` | C，`luaL_error` | C++ 异常 + `luaL_error(e.what())` | C++ 用 `Status` |

## 16. C++26 探针结果（2026-11-18，Windows MSYS2）

编译单元同时用到 `std::expected`、`std::span`、`std::bit_cast`、`std::countl_zero`、`[[assume]]`、`std::unreachable`、`using enum`、`constexpr` 里的 `static_assert`：

| 组合 | 结果 |
|---|---|
| `g++ 16.2.0 -std=c++26 -Wall -Wextra -O2` | 通过 |
| `clang++ 22.1.8 -std=c++26 -Wall -Wextra -O2` | 通过 |
| `g++ -std=c++26 -fno-exceptions` | 通过 |
| `g++ -std=c++26 -Wall -Wextra -Werror` | 通过 |

**踩到的坑**：`std::unreachable` 定义在 `<utility>`，不显式包含会报 `'unreachable' is not a member of 'std'`（GCC 与 Clang 都如此）——`wire.h` / `codec.h` 里用到就必须包含 `<utility>`。

## 17. `pb.c` 核对结果（2026-11-18，对照上游源码）
| 我们的假设 | 核对结果 |
|---|---|
| 默认按 `lua_next` 遍历 + 驻留名字指针查找 | **成立**：`lua_tolstring(L, -2, &len)` 取 key，再 `pb_fname(t, lpb_name(...))`；`encode_order` 走 `lua53_getfield` |
| 数字 key 会被转成字符串 | **成立**：key 一律过 `lua_tolstring`，数字会**原地转成字符串**，且当作字段名匹配 |
| 未知 field number 静默跳过 | **成立**：`lpbD_skipunknown`，跳过失败才 `luaL_error("invalid unknown field ...")` |
| 编码不校验 oneof | **成立**：`lpbE_onefield` 不看 `oneof_idx`，两个成员同时存在就都写 |
| 嵌套上限 1000 | **成立**：`#define LPB_NESTLIMIT 1000`，encode / decode 各自 `--e->levels` |
| int64 默认走 `lua_pushinteger` | **成立**（64 位 Lua 5.3+）；string 模式的转换条件是 `mode != LPB_NUMBER && ((u && n < 0) \|\| n < INT_MIN \|\| n > UINT_MAX)`，前缀 `#`（十六进制是 `#0x`） |
| 解码表按字段数预分配数组部分 | **部分不成立**：`lua_createtable(L, 0, fieldcnt)`，数组部分是 0，repeated 表是懒建的；我们的设计在 packed 定长场景会预分配数组，比它进一步 |

## 18. P1 实现记录（2026-11-18）

P1（`wire.h` + `Schema::Load` + 测试）已落地，实现中踩到的坑记在这里，避免 P2 重复踩：

1. **`std::vector::resize` 当 Commit 会清零**：`Reserve` 拿到指针后写入，再 `resize` 提交——新增的那些字节被值初始化成 0，刚写的内容全丢。`Writer` 改成裸分配 + `size_`/`capacity_`（与 `base/buffer` 同思路）。测试里专门有一条 127/128 字节负载的用例守住它。
2. **map 字段的 `type_name` 指向 entry 消息**：proto3 的 `map<K,V>` 在描述符里是 `repeated XEntry`，entry 是带 `options.map_entry = true` 的嵌套消息。entry 不登记为类型，所以字段解析时要先用"含 entry 的全名表"查目标，再判断它是不是 entry；只有非 map 位置才要求落到已登记类型上。
3. **"可打包"不能当 map key 的判据**：`map<string, ...>` 的 key 是 string——可做 key 但不可打包。key 的合法集合是"整数类型 / bool / string"（proto3 不允许 float / double / bytes / enum / message 当 key），单独用 `OpIsValidMapKey` 判。
4. **`std::unreachable` 要 `#include <utility>`**（GCC 16 / Clang 22 都如此，见 §16）。
5. **Windows 上 protoc 不能收非 ASCII 绝对路径**（见 §2）：CMake 里改成在 `proto_dir` 下用相对路径调用。
6. **本机（MSYS2）直接 `g++` 编出的可执行文件会加载 Git 自带的旧 `libstdc++-6.dll`** 而崩溃；对照实验要加 `-static-libstdc++ -static-libgcc`，或用仓库的 CMake 构建。这不影响工程构建，只影响手动编译的对照实验。

验证：`lua_pb_wire_test`、`lua_pb_schema_test`、`lua_pb_noexc_test`（`-fno-exceptions`）三个用例在 g++ 16.2 / clang++ 22.1.8 下都通过；用仓库里真实的 CMake 段落配置+构建+`ctest -R lua_pb` 三个用例全绿。

## 19. P2 实现记录（2026-11-18）

`codec.h/.cpp`（Lua table ↔ wire）+ `lua_pb_codec_test` / `lua_pb_hostile_test` 已落地。踩到的坑：

1. **`Op` 必须把有符号 / 无符号拆开**（`kInt32` / `kUint32` / `kInt64` / `kUint64`）：int32 的负数在线上是 10 字节 varint，解码要按 int32 解释；uint32 / uint64 的范围检查也不同。合并成一个 Op 会同时错两处。
2. **map 字段不能拿 `rawlen` 判空**：map 是字符串键的表，`rawlen` 恒为 0，会被当成"空 map 不写"整条跳过。改用 `lua_next` 探一条。
3. **非 packed 的 repeated message 元素不能走标量读取路径**：`ReadScalar` 不认识 `kMessage`，会报 `kTypeMismatch`；要在 repeated 分支里单独解子消息。
4. **"隐式标量等于默认值就不写"必须先判 Lua 类型**：`lua_tointeger('x')` 与 `lua_tointeger(3.5)` 都返回 0，会把类型错误静默当成默认值跳过。改成先看 `lua_type`，判 0 时对整数走 `lua_isinteger` + `lua_tointeger`，浮点走 `lua_tonumber` + `signbit`。
5. **非 packed repeated 的栈缓存**：缓存数组表时要记下它所在的栈位（`cache_top`）。原先用 `lua_settop(base - 1)` 会把**刚建好的解码结果表一起截掉**，表现为"解码返回 nil"。
6. **`Reader::Skip` 对 wire type 6 / 7 不能落到 `std::unreachable()`**：tag 的低 3 位可以是 6 / 7，属非法输入。改成返回 false，并新增错误码 `kInvalidWireType`（group 同样拒绝）。
7. **每个消息帧要 `lua_checkstack`**：一次字段处理最多压几个值，深嵌套时逐层叠加；不显式扩栈在 release 下会静默写坏栈（Lua 只在 debug 构建里断言）。
8. **`Codec` 必须在它绑定的 `lua_State` 关闭之前析构**（析构里要 `luaL_unref` kit 表）。测试里先 `lua_close` 后析构会直接崩——这条已写进 `codec.h` 的注释，也是 P3 绑定必须遵守的顺序。
9. **repeated 的空洞检测是尽力而为**：Lua 的 `#` 在有空洞时边界未定义（`{1, 2, 3}` 置 `n[2] = nil` 之后 `#` 可能只有 1），所以只有 `#` 恰好把空洞包进来时才会报 `kTypeMismatch`。这是 Lua 语义决定的，不是实现偷懒；用 C API 造表也不能让 `rawlen` 稳定包含空洞。

验证：`lua_pb_wire_test` / `lua_pb_schema_test` / `lua_pb_noexc_test` / `lua_pb_codec_test` / `lua_pb_hostile_test` 五个用例在 g++ 16.2 与 clang++ 22.1.8 下全过（含 `-fno-exceptions`），并经仓库真实 CMake 段落 `ctest -R lua_pb` 全绿。

## 20. P3 实现记录（2026-11-18）

`lua_pb.h/.cpp`（`OpenPb` + `require("pb")` 的 `pb.encode` / `pb.decode`）与 `lua_pb_binding_test` 已落地。

1. **Codec 放进 userdata，靠 `__gc` 析构**：这样"Codec 必须先于 `lua_State` 关闭而析构"（P2 §19 第 8 条）由 GC 顺序自动满足，宿主不用自己排顺序。`OpenPb` 把 userdata 当 upvalue 挂在两个闭包上。
2. **错误路径两段式**：拼错误文本的那一层（`PushErrorText`）**不调用 `lua_error`**，让 `std::string` 正常析构；真正 `lua_error` 的那一帧只留平凡局部变量（`longjmp` 不会跳过析构）。用法错误（参数类型不对、类型名不存在）直接抛 Lua 错误，与 starwing / quanta 的 Lua 表面一致。
3. **`OpenPb` 只写 `package.loaded["pb"]`，不设全局变量**（沙箱友好）：脚本用 `require("pb")` 取模块，没有 `pb` 这个全局名。
4. **库目标漏写源文件时，链接错误会被 `collect2` 吞掉**：本机只看到 `ld returned 1 exit status`，没有任何 undefined reference 明细；排查时用 `LUA_USE_APICHECK` 版 Lua 或直接把源文件列在手边编译更快。
5. **验证**：7 个用例（wire / schema / noexc / codec / hostile / binding / binding_noexc）经仓库真实 CMake 段落 `ctest -R lua_pb` 全绿；`lua_pb_binding_test` 里含两个线程各用各的 `lua_State` + 共享 Schema 的冒烟。
6. **apicheck 真跑过**：本机用仓库外（quanta）自带的 Lua 5.5.1 源码加 `-DLUA_USE_APICHECK` 现编了一份 Lua，codec / hostile / binding 三个用例在它上面全过——说明没有 Lua C API 误用、栈索引都合法。
7. **asan / tsan 本机跑不了**（与仓库其它模块的既有说明一致）：MSYS2 的 MinGW 没有 asan 运行时（链接失败），WSL 里的 clang 只有 14、不满足 C++26。绑定层里有线程冒烟，等真机 Linux 环境时按 `cmake --preset tsan` 跑一次即可。

## 21. P4 基准与校准（2026-11-18）

`bench/lua_pb_bench.cpp` 已落地（`-DBREEZE_BUILD_LUA_PB_BENCH=ON`，要用 release 预设跑，否则是无优化数字）。
并排对比在临时工程里做：把 starwing 的 `pb.c`（GitHub 上游单文件）编进同一个进程，注册成 `sw_pb`，
与我们的 C++ API、我们的 Lua 表面用**同一个 `lua_State`、同一份描述符、同一张表、同一批轮次**测；
每行预热 10% + 5 轮取中位数。环境：Windows 11 / MSYS2 ucrt64 / g++ 16.2.0 `-O2` / Lua 5.5.1（自编，非 vcpkg）。

| 负载 | 路径 | encode (ns/op) | decode (ns/op) |
|---|---|---|---|
| small 41 B | 本库 C++ API | 241 | 487 |
| small 41 B | 本库 Lua 表面 | 360 | 588 |
| small 41 B | starwing（Lua 表面） | 392 | 992 |
| medium 697 B（100 个 int + 3 个 string + 40 个嵌套子消息） | 本库 C++ API | 4483 | 7832 |
| medium 697 B | 本库 Lua 表面 | 4818 | 8096 |
| medium 697 B | starwing（Lua 表面） | 5013 | 13219 |
| large 16395 B（bytes 载荷） | 本库 C++ API | 164 | 1968 |
| large 16395 B | 本库 Lua 表面 | 1668 | 3259 |
| large 16395 B | starwing（Lua 表面） | 6349 | 3611 |

分配（停掉 GC 跑 20000 次解码，看 Lua 堆净增）：medium **6253 byte/op** —— 这是构建 40 个嵌套子表 + 数组的固有成本。

**结论**

1. 同口径下本库比 starwing 快：小消息 encode 1.6×、decode 2.0×；中消息 decode 1.7×；大消息 encode 约 38×
   （starwing 逐字段走 Lua 表 + `lua_next`，16KB 载荷时代价被放大）。
2. Lua 表面比 C++ API 每次多约 120–150 ns（小消息）；大消息多约 1.5 µs —— 那就是 `lua_pushlstring` 复制 16KB 的一次拷贝，
   与设计里写的"Lua 侧至多一次拷贝"一致。生产路径走 C++ API 就不付这一笔。
3. 口径说明：starwing 只能从 Lua 调，我们额外给了"本库 Lua 表面"一行做同口径参照；C++ API 行不是同一个调用形态，别直接拿它和 starwing 比。

**校准 1：建表哈希预分配（`kHashPreallocCap`）**

| 预分配 | small decode | medium decode |
|---|---|---|
| 0（不预分配） | 965 ns | 11058 ns |
| 17 | 490 ns | 8634 ns（该消息只有 4 个字段，17 与 32 都不生效，差值属噪声） |
| 32 | 495 ns | 8571 ns |

**不预分配会慢一倍**，所以这条优化必须保留；17 与 32 在只有 17 个字段的消息上等价，取 **32 不变**（给字段更多的消息留余量）。
常量现在可被 `-DLUA_PB_HASH_PREALLOC=N` 覆盖，只为复现这次校准。

**校准 2：`lua_pushexternalstring` —— 决定不做**

它能省掉大消息在 Lua 表面的那次 16 KB 拷贝（上表 large 行的 1.5 µs），但我们的 `Writer` **跨调用复用**：
external string 与 Writer 的缓冲区是同一块内存，下一次 `Encode` 会把它改写，调用方手里的字符串会变。
要安全用它就得每次编码新分配缓冲（把省下的拷贝又还回去）或改成"借用、下次调用前必须用掉"的 API —— 都是脚枪。
真正需要零拷贝的是 C++ 调用方，他们本来就直接拿 `span`，不受影响。以后若确有需要，加一个
`EncodeTo(span)`（写进调用方提供的缓冲）比动 Lua 表面更干净。

**局限**：单机、单线程、无 asan/tsan；Lua 是自编的 5.5.1；数字只作相对比较，不是绝对承诺。

### 21.1 Linux（WSL2 Ubuntu 22.04）上的复核 —— 2026-11-18

工具链：WSL2 / Ubuntu 22.04 / **g++-14.3.0（PPA 装的，`-std=c++26`）** / cmake 4.4.5 / protoc 3.12.4 / Lua 5.5.1（quanta 源码现编）。
`lua_pb` 的 7 个用例经**仓库真实 CMake 段落**（抽出来 + 桩 `breeze_lua_binding`）配置、构建、`ctest -R lua_pb` → **7/7 通过**；
protoc 版本判断正确触发（日志里能看到"protoc libprotoc 3.12.4 较老，加 --experimental_allow_proto3_optional"）。

| 检查 | 结果 |
|---|---|
| g++-14 `-std=c++26` 五个用例 | 全过（含多线程冒烟） |
| `-fsanitize=address,undefined -g -O1`（五个用例，`detect_leaks=1`） | **全干净**：无内存错误、无 UB、无泄漏 |
| `-fsanitize=thread`（多线程 binding 用例） | **无数据竞争报告**，且本机没有踩到"unexpected memory mapping"（直接跑与 `setarch -R` 都行） |
| `-fno-exceptions` 变体 | 通过 |

**Linux 上的并排对比（同一 harness、Release）**

| 负载 | 本库 C++ | 本库 Lua 表面 | starwing |
|---|---|---|---|
| small 41B enc / dec | 226 / 402 | 291 / 469 | 286 / 827 |
| medium 697B enc / dec | 4290 / 6898 | 4521 / 7099 | 4252 / 10473 |
| large 16KB enc / dec | 143 / 1011 | 803 / 1589 | 1783 / 1993 |

**和 Windows 的结论有出入，必须分开说**：Windows 上我们 encode 大消息快 38×，Linux 上只快 12×、中消息基本持平（4290 vs 4252）。
原因是 starwing 走 Lua 表逐字段遍历，MinGW 下 Lua 的字符串/表操作慢得多，差距被放大；
**decode 两侧一致：Linux 上我们快 1.5–2×**（small 2.1×、medium 1.5×、large 2.0×）。
所以对外只能说"decode 稳定快，encode 取决于平台与负载"，不能说"encode 全面快 38×"。
