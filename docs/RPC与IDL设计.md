# RPC 与 IDL 设计（C++ / Lua 混合）

**状态**：设计阶段，没有代码。本文定义 RPC/IDL 的运行时分层、`RpcModule` 与 `MessageModule` 的边界、IDL 生成物、C++/Lua 混合 handler 的 codec 选择，以及性能规则。

相关文档：

- C++ 主循环、Dispatcher、实体与进程拓扑：[Actor与线程模型设计.md](Actor与线程模型设计.md)
- App ↔ Agent 通道（shm / UDS、`IAgentChannel`）：[Agent通道设计.md](Agent通道设计.md)
- Lua 运行时与 worker：[Lua脚本框架设计.md](Lua脚本框架设计.md)
- Lua 绑定库边界（不含 codec）：[Lua绑定库设计.md](Lua绑定库设计.md)
- 共享内存协议/路由：[共享内存IPC设计.md](共享内存IPC设计.md)

---

## 0. 结论速查

1. **RPC 运行时独立成 `RpcModule`**，依赖 `MessageModule`，不反向依赖；默认是 `IModule`，若不需要独立生命周期可退化成 `RpcRuntime` 普通对象（见 §11.2）。
2. **`MessageModule` 只做“消息号 → handler”的通用派发 + 信封 + transport 发送**，不懂 RPC 语义。
3. **`Dispatcher` 是消息号登记表**（`Init` 登记、`Start` 前冻结），属于 `MessageModule` 内部实现；不再作为业务直接使用的 API。
4. **codec 按 service/method 选择**：C++ handler 用 C++ protobuf；Lua handler 用 Lua protobuf 库。
5. **`RpcModule` 不解析 payload**：只处理 RPC 头（`msg_id` / `seq` / `epoch` / 超时 / 路由），payload 以不透明字节传递，谁处理谁解码。
6. **IDL 生成 C++ / Go / Lua 三份产物**：C++/Go 是类型化 stub，Lua 是薄绑定（metadata + Call/Send 包装 + 注册 helper）+ `.pb` 描述文件。
7. **不生成 Lua 消息类**：Lua 侧用 `base/lua_pb`（自研库，见 [Lua_pb库设计.md](Lua_pb库设计.md)）在 handler 边界解码。
8. **一条消息号只有一个 handler**：C++ 与 Lua 不能重复注册同一 `msg_id`；一个 service 的方法可以分属不同语言，但同一方法不拆。
9. **RPC 一律异步**：`CallHandle` / `ReplyHandle`，无同步版本；超时由 main loop 定时器驱动。
10. **`msg_id` 在 IDL 中显式写**，生成器输出常量，不做哈希（沿用 Actor 文档 §8.4）。

---

## 1. 分层与模块职责

```text
业务层
  ├─ C++ service impl        ──┐
  └─ Lua service impl        ──┤
                               ▼
                          RpcModule（IModule）
                            ├─ service/method 注册表
                            ├─ 挂起表：seq / epoch / deadline
                            ├─ CallHandle / ReplyHandle
                            ├─ codec 选择（C++ / Lua）
                            └─ 错误映射（kUpstream / kTimeout / kDecodeFailed）
                               │
                               ▼
                        MessageModule（IModule）
                            ├─ Dispatcher：msg_id → handler
                            ├─ 信封编解码（RPC 头 / 普通消息）
                            └─ transport 发送（NetModule / IAgentChannel）
                               │
                               ▼
                  NetModule / AgentChannel（transport）
```

| 组件 | 类型 | 职责 | 不做什么 |
|---|---|---|---|
| `RpcModule` | `IModule` | RPC 语义：注册、挂起、超时、codec 选择、应答路由 | 不碰 socket / ring / IO 线程 |
| `MessageModule` | `IModule` | 消息号派发、信封、发送、接收投递 | 不懂 RPC 语义、不解 payload |
| `Dispatcher` | 普通对象 | `msg_id → handler` 登记表，冻结后只读 | 不是模块、不做生命周期 |
| `NetModule` | `IModule` | socket/连接/普通 transport、AgentServer、`MainLoopInbox` | 不懂消息号、不懂 RPC |
| `AgentChannel` | transport 实现 | App→Agent 数据面（shm 主线程 / UDS IO 线程） | 不做消息号/RPC 派发 |
| `LuaModule` | `IModule` | 主线程 Lua 运行时、注册 Lua handler | 不做 codec |

### 1.1 依赖方向

```text
业务 / IDL 生成代码
        ↓
     RpcModule
        ↓
   MessageModule
        ↓
 NetModule / AgentChannel
```

禁止：

- `MessageModule` 反向依赖 `RpcModule`；
- IDL 生成代码直接依赖 `NetModule` / `AgentChannel`；
- `RpcModule` 直接操作 socket / shm ring。

### 1.2 与 Actor 文档的关系

[Actor与线程模型设计.md](Actor与线程模型设计.md) §4 的 `Dispatcher`、§8 的 RPC/IDL 语义仍然有效；本文把实现拆成 `MessageModule` + `RpcModule` 两层，并补上 C++/Lua 混合 codec 的选择规则。

---

## 2. IDL 与生成物

### 2.1 schema 写法

沿用 gRPC schema 语法（只用语法，不用 gRPC 运行时）：

```proto
service PlayerService {
  option (service_type) = PLAYER;
  option (object_type) = PLAYER;
  rpc GetPlayer(GetPlayerReq) returns (GetPlayerRsp);
  rpc Kick(KickReq) returns (google.protobuf.Empty); // Send
}
```

- `msg_id` 在 IDL 中**显式写**（message option 或 method option），生成器输出常量；
- 不做哈希、不做自动分配；
- `stream` 生成器报错；
- `audience` 区分客户端命令与服务间 RPC；
- 请求消息用 `(route_key)` 标对象 id 字段。

### 2.2 生成物

| 目标 | 产物 | 内容 |
|---|---|---|
| C++ | `gen/cpp/*.pb.h/.pb.cc`、`*_service.h/.cpp` | 消息类型、service 存根、服务注册、metadata |
| Go | `gen/go/*.pb.go`、`*_service.go` | 同 C++（已有） |
| Lua | `gen/lua/<service>.lua` | 薄绑定：service/method 常量、`call`/`send` 包装、注册 helper |
| 描述文件 | `run/proto/*.pb`（构建产出，不进版本库） | 运行期由框架读一次交给 `base/lua_pb` 的 `Schema::Load`；C++/Go 侧由生成代码内嵌 |

### 2.3 Lua 生成物示例

```lua
-- gen/player_service.lua
local rpc = require("framework.rpc")

local M = {}

M.service    = "PlayerService"
M.service_id = 1001

M.methods = {
    GetPlayer = {
        msg_id    = 2001,
        req       = "GetPlayerReq",
        rsp       = "GetPlayerRsp",
        kind      = "call",          -- call | send
        codec     = "lua",           -- cpp | lua
        route_key = "player_id",
        audience  = "server",
    },
    Kick = {
        msg_id    = 2002,
        req       = "KickReq",
        rsp       = "google.protobuf.Empty",
        kind      = "send",
        codec     = "lua",
        route_key = "player_id",
        audience  = "server",
    },
}

function M.GetPlayer(req, opts)
    return rpc.call(M.service, "GetPlayer", req, opts)
end

function M.Kick(req, opts)
    return rpc.send(M.service, "Kick", req, opts)
end

function M.register(impl)
    return rpc.register_service(M.service, M.methods, impl)
end

return M
```

- 不生成 Lua 消息类、不生成字段访问器、不生成序列化代码；
- 类型信息来自 `.pb` 描述文件 + Lua protobuf 库；
- 生成器负责 service/method 名、`msg_id`、Call/Send、`codec`、`route_key`、`audience` 的一致性。

---

## 3. codec 选择

### 3.1 按 service/method 决定

| codec | 使用方 | 编解码 |
|---|---|---|
| `cpp` | C++ handler | C++ 生成类型 `ParseFromArray` / `SerializeToString` |
| `lua` | Lua handler | `base/lua_pb` 在 handler 边界（见 [Lua_pb库设计.md](Lua_pb库设计.md)） |

`codec` 由 IDL/生成器写入 metadata，`RpcModule` 启动时建表：

```text
msg_id → { service, method, codec, req_type, rsp_type, kind, handler_target }
```

### 3.2 RpcModule 不解 payload

- `RpcModule` 只解析 **RPC 头**（`seq` / `epoch` / `msg_id` / `deadline` / 错误码）；
- payload 是 `SharedBuffer` / `span`，透传到 handler；
- 需要转发的 RPC（不读字段）可以**零解码透传**；
- 需要审计/路由字段时，才解一次（由 C++ 或 Lua 按 codec 决定）。

### 3.3 混合同一个 service

- 同一个 service 的不同 method 可以用不同 codec；
- 同一个 method 不拆成 C++/Lua 两段；
- 需要“C++ 预处理 + Lua 业务”时，在一个 handler 内组合：
  - C++ handler 调用 Lua 函数；或
  - Lua handler 调用 C++ 绑定；
  - 组合不改变 msg_id 的唯一 handler 规则。

---

## 4. RPC 生命周期

```text
调用方 main loop：
  ① 分配 seq；RpcModule 挂起表登记；设 deadline
  ② codec 编码（cpp 或 lua）→ payload
  ③ MessageModule → 信封 → NetModule / AgentChannel

被调方 main loop：
  ④ MessageModule 按 msg_id 派发到 RpcModule 注册的 handler
  ⑤ RpcModule 出队时再判 deadline
  ⑥ handler 按 codec 解码 payload
  ⑦ ReplyHandle::Complete(status, payload)

调用方 main loop：
  ⑧ 应答回到挂起表；执行回调；清超时
```

规则：

- 解码在 **handler 执行时**（main loop 线程），不在 IO 线程；
- 单向消息（`Send`）decode 失败：计数 + 限频日志，不断连接；
- 应答与超时竞争：先到者赢，后到者计数（`late_reply`）；
- 无挂起项的应答：`unknown_seq`；
- `epoch` 变化后旧应答丢弃（配合 AgentChannel / 重连）。

---

## 5. C++ handler 路径

```text
MessageModule → RpcModule → C++ handler
  payload span → GeneratedMsg::ParseFromArray(span)
  → handler
  → response.SerializeToString(&out)
```

- 用生成类型，不用反射/dynamic message（热路径）；
- 大消息考虑 protobuf Arena；
- decode 失败 → `kDecodeFailed`，请求回错误；单向计数；
- C++ handler 在主循环执行，不阻塞；重计算走 `Offload`（Actor 文档 §7）。

## 6. Lua handler 路径

```text
MessageModule → RpcModule → Lua 适配器
  payload → Lua string（一次拷贝）
  → lua_pb 的 C++ API 解码 → Lua table
  → generated stub → user impl
  → rsp table → lua_pb 的 C++ API 编码 → payload
```

- Lua 侧依赖 **`base/lua_pb`**（自研库，设计见 [Lua_pb库设计.md](Lua_pb库设计.md)）：只支持 proto3 与 Lua 5.5.1，描述符编译后不可变、跨线程只读共享，每个 `luaState` 一个 `Codec`；语义细节（uint64 回绕、逐字段默认值、严格编码）以该文档为准。不引入 `pbc`；（历史方案：移植 quanta 的 `luapb` 或直接用 starwing 的 `lua-protobuf`，两者都因依赖/异常/严格性问题未采用）
- 描述符交付走**方案 b**：构建期 `proto/*.proto` 经 `protoc` 产出 `run/proto/*.pb`，运行期由框架读一次，交给 `Schema::Load(span)`——**读文件在框架层，库不碰文件系统**；
- 描述文件只加载一次（启动时），加载失败即启动失败，不重读、不热更；
- decode/encode 各一次，禁止业务内反复转换；
- Lua handler 异常用 `pcall` 捕获，返回框架错误 + traceback；
- Lua handler 在主循环执行；worker 线程不能直接发起/处理 RPC，必须投回主循环。

### 6.1 字段映射约定

| protobuf | Lua |
|---|---|
| int32/int64/sint/fixed | integer |
| uint32 | integer |
| uint64 | integer；超过 `2^63` 需 string，必须定策略 |
| float/double | number |
| bool | boolean |
| string | string |
| bytes | string（二进制安全） |
| enum | integer（或生成常量表） |
| repeated | array，1..n |
| map | table |
| nested message | table |
| oneof | 只出现设置的那个字段 |
| optional | nil 或缺省 |
| 未设置字段 | 省略 / 填默认值（定一种） |

字段名沿用 proto 的 `snake_case`（或统一转 camelCase），生成器与库保持一致。

---

## 7. 性能规则

1. **谁处理谁解码**：不做 C++ ↔ Lua 双向转换；
2. **RpcModule 不解 payload**：路由/转发零解码；
3. **描述文件与 metadata 启动时缓存**：`msg_id → codec/type/handler`、Lua 侧 schema；
4. **C++ 用生成类型**，热路径不用反射；
5. **Lua 用 C 实现的 protobuf 库**，不用纯 Lua；
6. **payload 移动不拷贝**：`SharedBuffer` / `span`；shm 通道直接在 ring 视图上解析；
7. **Lua 侧一次 copy 是上限**：库需要 contiguous string；避免额外 `table.concat`/`string.sub` 复制；
8. **转发透传**：不读字段就不 decode；
9. **大消息/高频优先 C++**；小消息 Lua protobuf 足够；
10. **用真实消息压测**：64B / 1KB / 16KB，分别测 cpp 与 lua 两条路径，看 p50/p99、CPU、分配、GC。

---

## 8. 错误、安全与版本

- 错误分层：框架错误走 `Status`，业务错误放 payload；
- `kDecodeFailed` / `kTimeout` / `kNoHandler` / `kUpstream`；
- wire 输入不可信：限制消息大小、嵌套深度、repeated 数量；
- enum 越界、类型不匹配、未知字段：策略在生成器/库层统一；
- IDL 版本：C++/Go/Lua/描述文件必须同源生成；Lua 侧描述文件版本要可检；
- 热更：
  - Lua `gen/*.lua` 可随文件级热更重载；
  - `.pb` 描述文件、C++ service 注册表不热更；
  - C++ 侧接口变更走重启（Actor 文档 §13：不做 C++ 原生热更）。

---

## 9. 测试计划

- 路由：C++/Lua 混合注册、同一 msg_id 冲突报错、冻结后注册失败；
- codec：cpp / lua 两条路径正确性；C++ 编码 + Lua 解码、Lua 编码 + C++ 解码交叉验证；
- Lua 库：描述文件加载、缺失类型、未知字段、enum、64 位、map、bytes、oneof；
- RPC：异步调用、超时、late reply、unknown seq、ReplyHandle 未回析构、链式预留；
- IDL：Call/Send 生成、stream 报错、`audience`、`route_key`、`msg_id` 常量；
- 性能：cpp vs lua、透传 vs 解码、64B/1KB/16KB；
- 故障：decode 失败、handler 异常、Agent 断线、epoch 变化丢弃旧应答。

---

## 10. 明确不做

- 同步 RPC、阻塞等待应答；
- 在 `lua_binding` 里做编解码（见 [Lua绑定库设计.md](Lua绑定库设计.md)）；
- 生成 Lua 消息类 / 字段访问器 / 第二套序列化；
- 全局单一 codec（必须按 service/method 可选）；
- `RpcModule` 反向依赖 `MessageModule` 之外的 transport；
- IDL 生成代码直接依赖 `NetModule` / `AgentChannel`；
- worker 线程直接发起/处理 RPC。

---

## 11. 与现有文档的关系与待决项

### 11.1 已对齐

- `Actor与线程模型设计.md` §8：异步 RPC、`CallHandle`/`ReplyHandle`、IDL 只用 schema、`msg_id` 手写不哈希、请求体在 handler 执行时解码；
- `Agent通道设计.md`：`MessageModule` 只依赖 `IAgentChannel`，AgentChannel 不做消息号/RPC 派发；
- `Lua脚本框架设计.md` / `Lua绑定库设计.md`：codec 不在绑定库、不在脚本框架，属独立库与新文档。

### 11.2 待决项（写代码前要定）

1. `Dispatcher` 与 `MessageModule` 的归属：`Dispatcher` 作为 `MessageModule` 内部实现，还是 App 成员由 `MessageModule` 包装；
2. `RpcModule` 是 `IModule` 还是 `RpcRuntime` 普通对象（若不做独立线程/生命周期，可以不是模块）；
3. `codec` metadata 由 IDL 生成还是配置；是否允许运行时覆盖；
4. ~~Lua protobuf 库最终选型：移植 `luapb` vs `lua-protobuf`~~ —— **已定：自研 `base/lua_pb`**，设计见 [Lua_pb库设计.md](Lua_pb库设计.md)；
5. 客户端命令（`audience = CLIENT`）的 handler 归属与 codec；
6. 非 RPC 普通消息：继续走 `MessageModule`，还是也统一到 `RpcModule`；
7. Lua handler 的 decode 失败与业务错误如何映射到 RPC `Status`；
8. RPC 与 `EntityManager` 的加载中排队、实体迁移（若有）的交互。
