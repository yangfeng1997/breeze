# CLAUDE.md

配置 schema 生成器：`config/schema/*.proto` → C++ 头文件 / Go 源文件 / JSON Schema。
改 schema 格式、校验规则或生成内容前先读本文件。

## 定位

- **proto 是 schema，YAML 是配置数据**。`.proto` 是配置结构的事实源，生成物不进版本库、不手改；
  运行时读的仍是 YAML 文件。
- 生成的 C++ 是对 [`src/cpp/base/config`](../../src/cpp/base/config/config.h) 字段表 API 的调用（`StructTraits` /
  `Field(...)` / `EnumTraits`），本身不含解析逻辑；合并、严格解析、行号错误、`${ENV:}` 展开、拼写检查都在运行时库里。
- 配置按**键名**（proto 字段名）读取，字段编号只为满足 proto 语法，改动无影响。
- 只用 proto 的 **schema 能力**，不用它的序列化：配置不是 protobuf 二进制，也不生成 `*.pb.*`。
  与 `docs/Actor模型与RPC设计.md` 里的 RPC proto 互不相干，option 编号段不重叠（见 `options.proto` 头注释）。

## 依赖：protoc

生成器调用 `protoc --descriptor_set_out --include_imports --include_source_info`，
再用**自带的无依赖 wire 解码器**读 descriptor。**不需要** Python protobuf、不需要 pip。

protoc 查找顺序：`--protoc` > 环境变量 `PROTOC` > PATH 上的 `protoc` / `protoc.exe`。
CMake 侧用缓存变量 `BREEZE_PROTOC`。

WSL 下用 Windows 的 `protoc.exe`（interop）时有两个坑，生成器已规避，改 `run_protoc` 前先知道：
- 它读不了 WSL 的 `/tmp`（UNC 路径失败），且会弄乱非 ASCII 参数（仓库路径含中文）；
  所以 schema 与 include 目录会先**拷到 ASCII 的 Windows `%TEMP%` 目录**再调用，报错里的路径再映射回源文件。
- 路径要用 `wslpath -w` 转成盘符形式。

## 用法

```
python3 scripts/configure.py                  # 日常入口：生成全部产物，再 cmake --preset debug（--gen-only 只生成）
python3 tools/configgen/configgen.py --schema-dir config/schema --check     # 只校验
```

`scripts/configure.py` 调 configgen（带 `--prune`，清掉 schema 删改后留下的过期文件，只删本生成器的文件名模式），产物：

| 路径 | 内容 |
|---|---|
| `src/cpp/gen/config/` | C++ 头文件，`#include "config/<proto 文件名>_config.h"` |
| `src/go/gen/config/` | Go 源文件，import 路径 `project/gen/config` |
| `build/jsonschema/` | JSON Schema，给编辑器的 yaml 补全 |

新增一类生成物（如策划表）：在 `configure.py` 的 `GENERATORS` 加一项，输出放 `src/<语言>/gen/<类别>/`。

C++ 构建也不依赖先跑脚本：CMake 里 `breeze_config_schema(<target> SCHEMA_DIR <dir> GEN_SUBDIR config)` 会在 schema
（或 `options.proto`、`configgen.py`）变动时自动重生成 `src/cpp/gen/config/`；不给 `GEN_SUBDIR` 则生成到构建目录
（测试用的样例 schema 这样，不污染 `gen/`）。两条路径写同一份确定的输出，互不冲突；只有脚本会 prune。

输出是确定的（无时间戳），内容没变就不重写，不会触发无谓的重编。

## schema 格式

标准 proto3。文件名（不带 `.proto`）即 schema 名，也是生成文件名；`package` 必填，映射为 C++ 命名空间
（`config.sample` → `config::sample`；`package config;` 就是 `namespace config`）。所有 schema 都 `import "options.proto"`，
`import` 只能指向 schema 目录（或 `-I` 给的目录）内的文件。

```proto
syntax = "proto3";
package config.gate;
import "options.proto";

enum Mode {
  MODE_UNSPECIFIED = 0;          // 必须有，且是第一个、值为 0
  MODE_FAST = 1;                 // yaml 里写 fast；C++ 枚举子 Fast
  MODE_SAFE = 2;
}

message GateConfig {
  option (config.root) = true;   // 为它生成 JSON Schema（给编辑器补全、拼写检查）

  // 注释变成生成代码里的注释和 JSON Schema 的 description
  uint32 tcp_port = 1 [(config.required) = true, (config.range) = {min: 1, max: 65535}];
  uint32 heartbeat_sec = 2 [(config.default) = "30", (config.reloadable) = true];
  string password = 3 [(config.secret) = true];
  Mode mode = 4 [(config.default) = "fast"];
  repeated Node nodes = 5 [(config.min_count) = 1];
}
```

| option | 作用于 | 含义 |
|---|---|---|
| `required` | 字段 | 键必须存在（按是否写了判断，`0` 也是合法值）。与 `default` 互斥 |
| `default` | 字段 | 字符串，按字段类型解析：`"30"` `"true"`，枚举写 yaml 名 `"fast"`。仅标量 / string / 枚举；`secret` 字段不能有（密钥不该进仓库） |
| `range` | 字段 | `{min, max}` 闭区间，仅数值；`int64` / `uint64` 边界超过 2^53 会丢精度，被拒绝 |
| `min_count` | 字段 | `repeated` / `map` 的最少元素个数。`required` 只管键存在，`[]` 能通过它，要拦空列表必须再加这个 |
| `secret` | 字段 | `ConfigDump` 里显示 `***`，`ConfigRedacted` 里打码。与 `reloadable` 互斥 |
| `env_value` | 字段 | 因环境而异：`config/env/*.yaml` **只能覆盖标了它的字段**，且每个环境文件都要写出来（不靠继承，防止漏写时悄悄沿用别的环境的值）；`common.yaml` 里也要有。只能标在叶子字段（标量 / 枚举 / list / map）。只进 JSON Schema 的 `x-env-value`，不改 C++ / Go 输出 |
| `reloadable` | 字段 | 允许热更，默认不允许。标在 message 类型字段上表示整个子树可热更 |
| `root` | message | 生成 JSON Schema |

**类型映射**：proto 标量 `bool int32 uint32 int64 uint64 float double string`、enum、message；
`repeated T` → list，`map<string, T>` → map（键只能是 string），proto3 `optional T` → optional。

**枚举命名**：`<ENUM 名蛇形大写>_UNSPECIFIED = 0` 必须是第一个；其余值带同一前缀，生成器剥掉前缀并转小写作为
yaml 里的名字（`ENV_DEV` → `dev`），编号要连续。C++ 枚举里多出一个 `Unspecified`（枚举没有 `default`
时落在它上），它**不是合法配置值**，写 `unspecified` 会报错。

**不引用手写类型**：schema 只描述自己生成的结构，不能指向手写的 C++ / Go 类型（没有 extern）。
需要把配置交给已有的结构（如 `base/logger` 的 `LoggerConfig`）时，在用到的地方读生成的配置，自己赋值过去。
这样 `base` 库不用依赖生成代码，生成器也不用知道别处的类型。

**不支持**（生成器直接报错，带 `文件:行号`）：proto2、缺 `package`、`service`、`extend`、嵌套 message / enum、`oneof`。
字段名 `[a-z][a-z0-9_]*`，C++ 转 camelCase、Go 转 PascalCase；类型名 `[A-Z][A-Za-z0-9]*`。

## 生成物

| 目标 | 内容 |
|---|---|
| C++ `<名>_config.h` | 结构体（成员带默认值）、枚举、`ConfigTraits` 字段表。读取、`ConfigDump`、`ConfigRedacted`、`ConfigNonReloadableChanges` 直接可用 |
| Go `<名>_config.go` | 结构体（yaml tag）、枚举（`string` 类型 + `Valid()`）、`NewX()`（填默认值）、`Validate()`（范围、枚举、`min_count`、嵌套）、`RequiredKeys()`、`Redacted()`、`NonReloadableChanges(next)`；已 `gofmt` |
| `<根 message 蛇形名>.schema.json` | draft-07，`additionalProperties: false`（拼错的键在编辑器里直接标红）；`$ref` 旁有 default / description 时用 `allOf` 包一层 |

## 已知限制

- **Go 的 `required` 分不清“没写”和“写了 0”**：`Validate()` 不查必填，由加载器按 `RequiredKeys()` 检查键是否存在。
  Go 侧目前没有加载器（还没有服务读 yaml），生成的代码只做过 `go vet` 与行为测试。
- 默认值只支持标量 / string / 枚举；list / map / optional 默认就是空。
- 所有 schema 的 Go 类型共用一个包，类型名要全局唯一（C++ 按 package 分命名空间，没有这个限制）。
- 没有 struct 级跨字段校验（`Validate` 钩子）与 `oneof`。
- `options.proto` 里的 option 编号必须与 `configgen.py` 的 `OPTION_NUMBERS` 一致，启动时核对，不一致报 `options.proto:1:`。

## 测试

- `testdata/schema/` 样例 schema，覆盖标量、枚举、嵌套、容器、optional、import、secret、reloadable、range、
  默认值、`min_count`。
- `configgen_cpp_test`（CMake）：样例 schema → 生成的头文件 → 运行时，端到端。
- `configgen_py_test`（CMake，`test_configgen.py`）：wire 解码器、命名转换、各种写错的 schema 必须被拦下且带
  `文件:行号`、确定性输出、JSON Schema、生成的 Go 代码能 `gofmt` / `go vet` / `go test`
  （`testdata/go_behavior_test.go.txt`，与 C++ 侧同一组期望，两种语言行为要一致）、非 ASCII 路径。
- `src/cpp/framework/kernel/common_config_test.cpp`：真实的 `config/schema/common.proto` 与 `config/common.yaml`、`config/env/*.yaml` 回归。
- `test_shipped_config_is_full`（`ShippedConfigTest`）：common.yaml + 每个 env 文件合并后，与 schema **逐字段双向对照**：
  schema 有而 yaml 没写（含取默认值的）、yaml 有而 schema 没有（拼错或 schema 已删），都失败，路径形如 `redis.nodes[0].port`；
  列表元素、map 的值也逐个查，optional 字段可不写。`common.yaml` 自己也必须是全量。合并规则与运行时一致（map 递归、标量和列表整体替换）。
- `test_env_files_override_exactly_the_env_value_fields`：每个 `env/*.yaml` 写的字段集合必须**恰好等于** schema 里标了 `env_value` 的集合——
  多写（覆盖了没标的）或少写（漏了某个 env_value 字段）都失败；env_value 字段在 `common.yaml` 里也要有。

## 工作规则

- 改了生成内容，先改 `test_configgen.py` / `configgen_cpp_test.cpp` 里对应的断言，再改生成器。
- 加 option 时，同步：`options.proto`、`OPTION_NUMBERS`、校验、C++、Go、JSON Schema 四处，以及本文件的表格。
- 不要让生成物依赖构建机信息（时间、路径），否则输出不再确定。
- protoc 自己会拦的错（JSON 名冲突、枚举值重名等）不重复校验，测试里断言 protoc 的报错即可。
