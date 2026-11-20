# App ↔ Agent 通道设计（Sidecar 数据面）

**状态**：设计草案，没有代码。本文只设计 App 侧到本机 Agent 的通道：控制面、传输选择、shm / UDS 数据面、线程归属、失败语义。

相关文档：

- 共享内存布局、握手、fd、保留/Reattach、路由：[共享内存IPC设计.md](共享内存IPC设计.md)
- C++ 主循环、Dispatcher、RPC/IDL：[Actor与线程模型设计.md](Actor与线程模型设计.md)
- 网络库与 `MainLoopInbox`：[网络库设计.md](网络库设计.md)

---

## 0. 结论速查

1. **App 侧是一对一**：一个 App 进程只连本机 Agent 的一个 `AgentServer`；AgentServer 才是一对多。
2. **传输方式在握手时确定**：`Shm` 或 `UdsOnly`，整个会话不变。
3. **不做运行中降级/切换**：不会在会话中间把 `ShmAgentChannel` 换成 `UdsAgentChannel`。
4. **控制面独立**：`AgentControl` 在控制线程/IO loop 上做握手、fd 传递、心跳、探活、Reattach、退避重连。
5. **shm 数据面在主线程**：`ShmAgentChannel` 是 Tx 环唯一生产者、Rx 环唯一消费者；零拷贝；处理完再 `Pop`。
6. **UDS 数据面在 IO 线程**：`UdsAgentChannel` 的 socket 读写由 IO 线程完成，经 `MainLoopInbox` 回投主线程，多一次 payload 拷贝。
7. **统一接口**：RpcModule / MessageModule / 业务只看到 `IAgentChannel`，不感知 shm / UDS。
8. **失败即会话失败**：shm 或 UDS 会话出错时，关闭会话、按退避重新握手；新会话可以重新选择 transport，但不会在旧会话内切换。
9. **`MainLoopExecutor` 改名 `MainLoopInbox`**，留在 NetModule 内部，服务普通 socket transport；shm 通道旁路它。
10. AgentServer 保持一对多，不因 App 侧改造而改变。

---

## 1. 角色与边界

```text
App 进程
  ├─ AgentControl          控制面：UDS 握手、fd、心跳、Reattach、选择 transport
  ├─ IAgentChannel         数据面统一接口
  │    ├─ ShmAgentChannel  主线程；Tx/Rx ring
  │    └─ UdsAgentChannel  IO 线程 + MainLoopInbox
  ├─ MessageModule         消息号、信封、派发（Dispatcher）
  ├─ RpcModule             RPC 语义：注册、挂起、超时、codec 选择
  └─ NetModule             普通 socket transport、AgentServer、MainLoopInbox
```

### 1.1 App 侧 vs Agent 侧

| | App 侧 | Agent 侧 |
|---|---|---|
| 连接数 | 一对一（本机 Agent） | 一对多（多个 App） |
| 传输选择 | 握手时协商 | 握手时决定给不给 |
| 数据面 | shm 主线程 / UDS IO 线程 | 共享内存/链路路由 |
| 路由 | 不需要选 Agent | 按 busid / Node 路由 |
| 心跳 | 控制面 UDS | Agent 检测 App |

**App 侧没有多连接池、负载均衡、连接路由**，这是可以专门设计 channel 的前提。

---

## 2. 总体结构

```text
             控制面（UDS）
App ────────────────────────────── AgentServer
  │   Hello / fd / heartbeat / Reattach
  │
  │  数据面（握手时二选一）
  ├─ Shm：App 主线程 Tx/Rx ring ── memfd ── Agent
  └─ Uds：IO 线程 socket ── MainLoopInbox ── App 主线程
```

### 2.1 为什么控制面和数据面分开

- 控制面是低频、带超时/重试/状态机的网络 IO，不能占主循环；
- shm 数据面是高频、要求零拷贝和主线程顺序，适合主线程直接处理；
- UDS fallback 数据面仍然是 socket，适合 IO 线程 + MainLoopInbox；
- 三者生命周期和失败语义不同，不能塞进一个 `IConnection`。

---

## 3. 握手与传输选择

### 3.1 策略

| 配置 | 行为 |
|---|---|
| `ShmRequired` | 必须 shm；Agent 不给或创建失败 → 连接失败 |
| `ShmPreferred` | 先要 shm；只有可用性失败（ENOSPC、memfd 失败、Agent `allow_shm=false` 等）才退化到 UDS |
| `UdsOnly` | 不创建共享内存；握手 UDS 直接作为数据通道 |

### 3.2 握手时确定，会话内不变

```text
Hello
  → Agent 回复 Shm / UdsOnly / Reject
  → AgentControl 创建对应 channel
  → 会话内 transport 固定
```

- 不做运行中 `Shm → Uds` 或 `Uds → Shm` 切换；
- 不维护双 channel；
- 不 drain 旧 channel 再换新 channel；
- Reattach 只能恢复同一 transport 的会话。

### 3.3 重新握手

如果会话失败：

```text
会话失败
  → 关闭当前 channel
  → 按退避重新 Hello
  → 新会话重新选择 transport
```

这是**新会话**，不是运行中降级：

- 旧会话未消费消息、在途 RPC 不会迁移；
- 新会话可能选 shm，也可能选 UDS；
- 业务看到的是连接断开/重连，不是同一个连接换了底层。

---

## 4. ShmAgentChannel（主线程数据面）

### 4.1 职责

```text
ShmAgentChannel
  ├─ Tx ring：唯一生产者 = 主线程
  ├─ Rx ring：唯一消费者 = 主线程
  ├─ Send：TryReserve → 写 → Commit
  ├─ Drain：Peek → handler → Pop
  ├─ eventfd：Tx 通知 Agent / Rx 唤醒主循环
  └─ state：Active / Paused / Unavailable
```

### 4.2 收发

```text
发送：
  主线程 Send
    → TryReserve(len)
    → 写记录
    → Commit（盖入队时间）
    → eventfd 通知 Agent

接收：
  Agent 写 Rx 环
    → eventfd 唤醒主循环
    → 主线程 Peek
    → handler 处理 span
    → Pop
```

### 4.3 零拷贝与不丢

- `Peek` 返回共享内存里的只读视图，不拷贝；
- handler 处理完才 `Pop`，槽位在处理前不会复用；
- 进程在 Pop 前崩溃，消息仍在 ring，重启后可重投；
- 语义是 **at-least-once**：处理完但 Pop 前崩溃会重复投递，业务需要幂等/去重；
- 主循环慢 → Rx ring 满 → 背压传回 Agent；
- Tx ring 满 → `Send` 返回 `Full`，不阻塞。

### 4.4 线程规则

- ring 的 `Peek`/`Pop`/`TryReserve`/`Commit` **只能在主线程**；
- 控制线程只做映射、fd、握手、Reattach，不读写 ring；
- 任何 IO 线程/后台线程不能直接写 Tx 环或读 Rx 环；
- 需要从其他线程发送时，只能投到主线程的 pending 队列，由主线程写。

---

## 5. UdsAgentChannel（IO 线程数据面）

### 5.1 职责

```text
UdsAgentChannel
  ├─ socket 读写：IO 线程
  ├─ Send：ConnectionHandle → IO 线程
  ├─ 接收：IConnectionHandler::OnMessage
  │    → MainLoopInbox（拷贝）
  │    → 主线程 handler
  └─ 连接生命周期：IO 线程 / 控制面
```

### 5.2 拷贝

```text
socket 读缓冲 → IConnectionHandler::OnMessage(span)
  → MainLoopInbox::OnMessage
  → SharedBuffer::CopyFrom(payload)   ← 一次拷贝
  → 主线程
```

- UDS 路径**需要 MainLoopInbox**；
- 多一次 payload 拷贝；
- 业务回调仍在主线程；
- 顺序由 MainLoopInbox/连接队列保证；
- 崩溃不丢能力弱于 shm：依赖 socket/上层重传。

### 5.3 为什么 UDS 不放主线程

- 主循环不应集成 socket 等待/非阻塞状态机；
- 心跳、超时、重连要独立调度；
- 收益只是省一次拷贝，而 shm 已经覆盖高性能路径；
- UDS 是降级/兼容路径，保持简单。

---

## 6. IAgentChannel 接口

```cpp
struct MessageView {
    std::span<const std::byte> payload;
    // UDS 的 payload 由 inbox 的 SharedBuffer 持有；
    // shm 的 payload 在 Pop 前有效，release 由 channel 负责。
};

enum class AgentState { Disconnected, Handshaking, Active, Paused, Unavailable };

class IAgentChannel {
public:
    virtual ~IAgentChannel() = default;

    virtual std::expected<SendResult, Error> Send(MessageView message) = 0;
    virtual void SetHandler(IAgentHandler& handler) = 0;

    virtual void Start() = 0;
    virtual void Stop() = 0;

    virtual AgentState state() const = 0;
    virtual std::uint64_t epoch() const = 0;
};
```

- `SendResult` 至少区分 `Ok` / `Full` / `Unavailable` / `Closed`；
- `epoch` 用于拒绝旧会话的晚到回调/应答；
- MessageModule / RpcModule 只依赖 `IAgentChannel`，不依赖 `ShmConnection` / `IConnection`。

---

## 7. 控制面

### 7.1 AgentControl

```text
AgentControl（控制线程 / IO loop）
  ├─ UDS 连接、Hello、SCM_RIGHTS 传 fd
  ├─ 协商 Shm / UdsOnly / Required / Preferred
  ├─ 心跳、探活、EOF
  ├─ Agent down/up
  ├─ Reattach（仅同 transport）
  ├─ 退避重连
  └─ 创建 / 关闭数据 channel
```

### 7.2 心跳

- 心跳走 UDS，即使数据走 shm；
- Agent 断线/心跳超时 → `AgentControl` 标记 channel `Unavailable`；
- 主线程 `Send` 返回 `Unavailable`，不写环；
- 按退避 Reattach；失败则关闭会话，重新 Hello。

### 7.3 为什么心跳不在主线程

- 心跳是定时/网络 IO；
- 主循环长 tick 会错过心跳窗口；
- 重连退避不能占用业务 tick；
- 控制面失败不应拖慢业务。

---

## 8. 失败与重连

### 8.1 不做运行中降级

- 会话内 transport 固定；
- 不把 `ShmAgentChannel` 换成 `UdsAgentChannel`；
- 不做 drain-on-downgrade；
- 不维护两个 channel 同时可用。

### 8.2 失败处理

| 失败 | 处理 |
|---|---|
| shm ring 校验失败 / 映射损坏 | channel `Unavailable`；关闭会话；重新 Hello |
| Agent 崩溃 | 标记 down；尝试 Reattach |
| Reattach 失败 | 关闭会话；重新 Hello（可重新选 transport） |
| UDS 断开 | 关闭会话；重连 |
| Tx ring 满 | `Send` 返回 `Full`，不是降级 |
| 主循环慢导致 Rx 满 | 背压传回 Agent，不是降级 |

### 8.3 语义

- 在途 RPC：返回 `kAgentDown` / `kUpstream`，不静默等超时；
- 出站消息：`Send` 返回 `Unavailable`，业务决定重试/丢弃；
- 旧会话未消费消息：能 drain 就 drain，否则计数丢失；
- 新会话不迁移旧消息；
- 重复投递由业务幂等/去重处理；
- 旧 epoch 的回调和应答丢弃。

---

## 9. 线程规则

| 组件 | 线程 | 约束 |
|---|---|---|
| `AgentControl` | 控制线程/IO loop | 不碰 shm ring 数据 |
| `ShmAgentChannel` | **主线程** | 唯一 Tx 生产者、唯一 Rx 消费者 |
| `UdsAgentChannel` | IO 线程 + 主线程接口 | socket 在 IO；业务回调在主 |
| `MessageModule` | 主线程 | 只依赖 `IAgentChannel`；消息号/信封/派发 |
| `RpcModule` | 主线程 | RPC 语义；通过 MessageModule → `IAgentChannel` |
| `MainLoopInbox` | IO → 主 | 服务普通 socket / UDS 数据面 |

禁止：

- 主线程以外的线程读写 shm ring；
- 主线程直接 poll UDS socket；
- 控制线程直接调用主线程 channel 的 `Drain`/`Send`；
- 从 IO 线程直接写 Tx 环。

---

## 10. 背压与缓冲

### 10.1 shm

- Tx 满 → `Full`；
- Rx 满 → Agent 侧处理/丢弃/背压；
- 主循环慢 → Rx 满；
- 不引入无界进程内队列。

### 10.2 UDS

- 复用 `ConnectionHandle` / IO 线程发送背压；
- MainLoopInbox 的每连接条数/字节/全局预算；
- 超限 `PauseRead`。

---

## 11. 生命周期与停机

```text
Start:
  AgentControl 握手
  → 创建 channel
  → 主线程 Start（shm）/ IO 线程 Start（UDS）

Stop:
  停止业务新发送
  → 等待在途/排空
  → 主线程 Stop（shm）/ IO 线程 Stop（UDS）
  → AgentControl Detach/Bye
```

- shm channel 必须在主线程停止；
- 控制线程不能先销毁，否则无法完成 Detach；
- AgentServer 侧按现有保留/Bye 语义处理。

---

## 12. 配置

```text
net.agent_transport = shm_required | shm_preferred | uds_only
net.agent_shm_ring_mb = 16
net.agent_endpoint = unix:///run/breeze/agent.sock
```

- `shm_preferred` 只在握手阶段退化；
- 运行中不降级；
- 重新握手可以按同一配置重新选择。

---

## 13. 测试计划

- 握手矩阵：Shm / UdsOnly / Preferred 退化 / Required 失败；
- shm：Send / Drain / Peek / Pop / ring 满 / 主循环慢；
- shm：处理前崩溃重投、重复投递、幂等计数；
- shm：Agent down / Reattach 成功 / 失败后重新 Hello；
- UDS：MainLoopInbox 拷贝、顺序、背压；
- 会话内 transport 不变；运行中不切换；
- 旧 epoch 回调/应答丢弃；
- 控制线程与主线程无数据竞争（tsan）；
- 停机：主线程先停 shm，再 Detach；
- 性能：64B / 1KB / 16KB，shm 对比 UDS，p50/p99、CPU、ring full。

---

## 14. 明确不做

- 运行中 `Shm → Uds` / `Uds → Shm` 切换；
- 双 channel 并存；
- drain-on-downgrade；
- 主线程直接处理 UDS socket；
- 控制线程直接读写 shm ring；
- App 侧多 Agent 连接池/负载均衡/路由；
- 在 AgentChannel 内做消息号/RPC 派发（归 MessageModule / RpcModule）。

---

## 15. 与现有代码的关系

- 现有 `AgentClient` 的握手/Reattach 逻辑可复用到 `AgentControl`；
- 现有 `ShmConnection` 的 IO 线程路径保留为 UDS/fallback 参考，或作为迁移期 fallback；
- `MainLoopExecutor` 改名 `MainLoopInbox`，留在 NetModule 内部；
- 普通 TCP/UDP/HTTP 继续使用 `MainLoopInbox`；
- AgentServer 保持一对多，不变。
