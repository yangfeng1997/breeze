# `proto/` —— IDL 源文件

这里只放**手写的 `.proto` 源文件**（RPC 服务定义与消息定义），是 IDL 的唯一真源。

- 配置类的 schema 不在这里，仍在 `config/schema/`（由 `tools/configgen` 消费）。
- **不要提交生成物**。`.pb` 描述符与各语言的生成代码都落在已被 `.gitignore` 忽略的路径下：
  - `run/proto/*.pb` —— `protoc --descriptor_set_out` 产出的 `FileDescriptorSet`，运行时由框架读取后交给 `base/lua_pb`（见 [docs/Lua_pb库设计.md](../docs/Lua_pb库设计.md)）。
  - `src/cpp/gen/`、`src/go/gen/`、`src/lua/gen/` —— 各语言的生成代码（暂未开工）。

## 生成

CMake 里的 `breeze_proto_descriptor` 负责产出描述符，protoc 按 `-DBREEZE_PROTOC=<路径>` > 环境变量 `PROTOC` > `PATH` 查找。

```sh
cmake --build <build-dir> --target proto_descriptor
```

手写单个文件也可以直接调，**注意在 proto 目录下用相对路径**（Windows 的 protoc 用 ANSI 解析 argv，绝对路径里的非 ASCII 会乱码）：

```sh
cd proto && protoc --descriptor_set_out=../run/proto/descriptor.pb --include_imports -I . *.proto
```

## 现状

IDL 生成器（`tools/protoc-gen-svcstub`）还没开工，`docs/RPC与IDL设计.md` §2 定义了生成物形态。在它落地之前，`base/lua_pb` 用自己的测试 proto（`src/cpp/base/lua_pb/testdata/`）跑通链路。
