# 共享内存 IPC 设计（Sidecar 本机通道）

**状态**（2026-11-01）：**已实现并有测试**：`base/shm`（数据结构）、握手 / UDS 传 fd、**握手时协商传输方式（共享内存 / 仅 UDS / 共享内存优先、给不了就退化）**、保留与恢复（App 重启）、Reattach（Agent 重启）、控制消息（`PrepareRestart` / `Bye`）、`ShmChannelServer` / `ShmChannelClient`（阻塞）、**信封与 Agent 的路由转发（`AgentRouter`）**、**App 一侧的非阻塞客户端 `AgentClient`（共享内存也表现成一条 `IConnection`）**、**`NetModule` 的接入（`AddAgentServer` / `AddAgentClient`）与配置（`net.agent_*`）**，见 §11 / §12。**还没有**：`framework/app` 里的自检线程（§6.4）、控制面（路由表的真实来源、Node 的登记）、Agent 之间链路的自动建立、多 loop 的 Agent、`Probe` / `Abort`（§6.6，暂缓）。**性能只有一版单连接的对比压测**（§10「实测」：WSL2、单连接、单 IO 线程）：其余的性能数字仍是目标，不是结果。**tsan 没有在本机跑过**（§12）。
读之前先看 [网络库设计.md](网络库设计.md)：本通道复用它的 UDS、`EventLoop`、`IConnection` 体系，但**不走** `IConnection`（共享内存里没有字节流）。

## 0. 结论速查

用户已定的几点（2026-10-31）：

1. **每个通道两条单向 SPSC 环，放在同一个 memfd 里**；一个方向一条，不共用一条（SPSC 只有一个生产者，双向必须两条）。
2. **队列跨 App 重启保留**：App 崩溃或热更重启期间，发给它的消息留在队列里，重启后接着读（§6）。
3. **每条环默认 16 MiB**（每通道 32 MiB；§8 算了账）。
4. **指标按需记录**，先记 §9 的清单。
5. **目标是同机路径比 Tbuspp2 更低延迟、更高吞吐**（§10）；目前只是目标，要靠基准验证。
6. **共享内存大小由 App 在握手时指定**，不指定就是默认 16 MiB；必须是 **2 的幂**，否则拒绝（§6.1）。
7. **记录起点按 64 字节（缓存行）对齐**（§4），由我按最终结构定，基准验证。
8. **Agent 崩溃而 App 没崩时，App 继续使用原来的共享内存**；Agent 重启后由 App 把 fd 传回去恢复（§6.5）。
9. **Tx 方向至多一次**：Agent 消费 Tx 环按"先标记、处理完 `Pop`、崩溃后跳过被标记的那条"实现，效果等价于先 `Pop` 再转发，丢了靠上层重传（§6.5）。
10. **App 卡死先由 App 自己管**（自检线程发现主循环不前进就 `abort()`，属 `framework/app`，§6.4）；Agent 侧只检测并记指标，阈值默认 10 秒。**Agent 经 UDS 让 App `abort()` 的方案暂缓，只记录在 §6.6，第一版不做。**
11. **过期**：信封里放相对剩余毫秒，通道每条记录都盖入队时刻，出队时由解析信封的一层算（§4）；单向 `Send` 至多一次、可丢，继承 `Ctx` 的 deadline 取较小者，默认 TTL 5 秒。

12. **握手时协商传输方式（2026-11-01）**：App 在 Hello 里说要什么（仅 UDS / 共享内存优先 / 共享内存必须），Agent 说了算给不给；"仅 UDS"时握手完的 UDS 直接当数据通道，App 侧可配置（`net.agent_shm_enabled`），不启用共享内存就退化成仅 UDS 通信（§6.1）。
13. **Agent 的路由转发在 IO 线程内完成**（不经 App 主循环）：解析信封、核对发送方身份、判过期、查路由表、直接写目的地的 Rx 环 / 连接 / 链路（§4、§10）。信封格式是参考几个开源 RPC 框架与两个自有项目后定的（§10）。
14. **共享内存对业务也表现成 `IConnection`**（`ShmConnection`）：`NetModule::AddAgentClient` 给业务的回调接口、`ConnectionHandle` 跨线程发送、`MainLoopExecutor` 回投主循环都与 TCP / UDS 的连接一样，业务不用关心底下是哪种传输（§11）。

由第 2、8 点推出的设计结果：**memfd 由 Agent 创建，之后两个进程各持有一份 fd，谁活着谁保管**。App 重启，Agent 把 memfd 传回给新 App（§6.2）；Agent 重启，App 把 memfd 传回给新 Agent（§6.5）。只有两边同时死，队列才真正消失。

## 1. 背景与范围

- **用途**：同机的 `GameApp` ↔ `Agent`（Sidecar 或 Node Agent）之间的数据通道。跨机仍走 TCP（网络库已有）。
- **UDS 的分工**：只做握手（用 `SCM_RIGHTS` 传 fd）和存活探针（对端关闭即 EOF），**不走业务数据**。这与 [网络库设计.md](网络库设计.md) §1.1 里"app ↔ sidecar：UDS"的定位一致，数据面换成共享内存。
- **要新增的能力**（现有库明确不做或没有）：`SCM_RIGHTS` 传 fd（[网络库设计.md](网络库设计.md) §8 的"不做"清单里有它，这里要做）、`memfd_create` / `mmap` 封装、跨进程环、eventfd 唤醒协议。
- **非目标**：跨机、加密、多生产者、Windows、对象级路由（属于业务层）、业务消息格式（环对载荷不透明）。

## 2. 总览

```
  GameApp 进程                        同一个 memfd（Agent 创建）                    Agent 进程
  Tx 环：生产者  ──── 写 ───▶  [ Tx 环 ]  ──── 读 ───▶  Tx 环：消费者
  Rx 环：消费者  ◀─── 读 ────  [ Rx 环 ]  ◀─── 写 ────  Rx 环：生产者
       ▲                                                          │
       └──── 对方睡着时敲 eventfd 叫醒（每个方向一个 eventfd）────────────┘
       ShmChannelClient ◀───── UDS：握手（传 3 个 fd）+ 存活探针 ─────▶ ShmChannelServer（每个 App 一个 ShmChannel）
```

- **一个通道 = 1 个 memfd + 2 个 eventfd + 1 条 UDS 连接**，对应 3 个经 `SCM_RIGHTS` 传递的 fd。memfd 由 Agent 创建；传出去之后 App 进程里也有这 3 个 fd 和映射，所以任何一方死掉，另一方手里的内存都还在。
- **一个 App 进程一个通道**，不是一个线程一个。业务层是单线程（主循环），所以主循环线程是 Tx 环唯一的生产者、Rx 环唯一的消费者，天然满足 SPSC；App 里别的线程要发，先汇到主循环。
- **Node Agent 有 N 个 App 就有 N 个通道**，各自独立的 memfd（App 之间内存隔离，一个 App 写坏自己的环不影响别人）。
- 命名：**Tx 环** = App 到 Agent，**Rx 环** = Agent 到 App（以 App 为视角）。

## 3. 内存布局

一个 memfd，控制块在前、数据在后，**每个区域按页对齐**：

```
偏移 0          ChannelHeader   (1 页)   魔数、版本、布局版本、`record_align`、`app_epoch`、`agent_epoch`、两条环的容量与偏移、标志位
        +1 页   TxControl       (1 页)   write_idx / read_idx / consumer_sleeping / `agent_inflight_idx` / 统计计数（各自独占缓存行）
        +2 页   RxControl       (1 页)   同上（`inflight_idx` 由 App 写，只给选择至少一次的 App 用）
        +3 页   TxData          (Tx 容量字节)
        +…      RxData          (Rx 容量字节)
```

- **控制块全放在前面**：一次 `mmap` 映射前 3 页；两个数据区各做一次**镜像映射**，所以每一侧共 **5 次 `mmap`**。
- **镜像映射**：先 `mmap(PROT_NONE)` 预留 `2 × 容量` 的连续虚拟地址，再对同一个 memfd 偏移用 `MAP_FIXED` 映射两遍。环里任何一条记录哪怕跨过尾部，在虚拟地址上都是连续的，读写方一行折返判断都不用写。容量必须是 2 的幂且是页的整数倍（16 MiB 满足）。
- **游标是单调递增的 64 位字节计数**，不回绕；位置 = `idx & mask`，占用 = `write_idx - read_idx`。
- **游标属于"角色"不属于"进程"**：Tx 的 `write_idx` 由当前生产者（App）写、`read_idx` 由当前消费者（Agent）写。任何一方换了进程，新进程都接着用共享内存里的游标，这是 §6 保留与恢复语义的基础。
- `ChannelHeader` 里有两个计数：**`app_epoch`** 每次 App 挂载加一，**`agent_epoch`** 每次 Agent 挂载（含 Reattach）加一，各自用来识别对应角色上过期的进程（§6）。
- **`record_align`**（目前固定 64）和 **`layout_version`** 一起写在头里：以后改对齐值，两边靠这两个字段识别，不会悄悄错位。
- **所有会放进共享内存的原子变量都必须 `is_always_lock_free`**（`static_assert`），否则 `std::atomic` 会用进程内的锁，跨进程就失效。

## 4. 环与记录格式

**记录**（16 字节头 + 载荷；**记录起点按 64 字节对齐**，整条记录占 `roundup64(16 + len)` 字节，尾部补的是空白）：

| 字段 | 大小 | 说明 |
|---|---|---|
| `len` | u32 | 载荷字节数 |
| `flags` | u16 | 保留，必须为 0 |
| `reserved` | u16 | 保留，必须为 0 |
| `enqueue_ts_ns` | u64 | 生产者 `Commit` 的单调时钟时刻，**每条都有**，由通道库在 `Commit` 里盖、同一批共用一次读数；用来判断过期（§4 "过期怎么算"）和量排队时间（§9）；`0` 无效 |

**环对载荷不透明**：Agent 要读的路由信息（比如文章里的 64 字节信封头）在载荷里，环不关心。

**为什么记录按 64 字节对齐**（我按最终结构定的，**用基准验证**，不是教条）：

- **相邻记录永远不共享缓存行**：生产者在写下一条、消费者在读上一条时，两边不会碰同一个缓存行。队列接近空、一条消息一条消息往返的路径（我们要量的低延迟路径）最受益；批量忙时影响小。
- **路由决策只读记录的第一个缓存行**：首行 = 16 字节记录头 + 载荷的前 48 字节。要求信封头把路由决策要用的字段都排在前 48 字节里（见 §10），Agent 转发一条消息就只触碰一个缓存行。
- **代价**：平均每条多占约 32 字节，小包的相对开销更大（64 B 载荷从 80 B 变成 128 B），所以同样的环容量能装的条数、能缓冲的时间都少了，§8 的表格已按 64 字节对齐算。内存带宽不是瓶颈，这主要影响保留时长。
- **可改**：`record_align` 写在通道头里。基准若显示代价大于收益，改常量并升 `layout_version` 即可，改动面小。

**API 语义**（单生产者、单消费者，都不阻塞）：

| 生产者 | 消费者 |
|---|---|
| `TryReserve(len)` 返回一段可写的连续内存，或 `Full` / `TooLarge` | `Peek()` 返回下一条记录的只读视图，或空 |
| `Commit()` 发布刚写完的记录 | `Pop()` 释放它 |
| 可连续写多条再**一次 `Commit`**（批量发布） | 可连续 `Peek` / `Pop` 多条再**一次推进 `read_idx`**（批量释放） |

- **批量发布与批量释放**：每次发布只做一次 `release` 存储加一次睡眠标志检查，忙时把原子操作和唤醒检查摊到一批消息上。
- **`Commit` 同时给这一批记录盖 `enqueue_ts_ns`**；`Peek()` 返回的视图带着它，调用方自己算年龄与过期（通道不读信封）。
- **视图只在 `Pop` 之前有效**，所以消费者必须在 `Pop` 之前用完（或拷走）。
- **满了怎么办**：`TryReserve` 返回 `Full`，由调用方决定丢、重试还是上报；环自己不阻塞、不静默丢。语义对应现有 `SendResult` 的背压。
- 单条记录载荷上限默认 **1 MiB**，且不超过环容量的 1/16（环最小 1 MiB 时即 64 KiB），握手时可调小。

**过期怎么算（已定：方案 B，2026-10-31；没有实现）**：

**一句话**：**信封里放相对剩余毫秒；通道给每条记录盖入队时刻；出队时由解析信封的那一层（Agent 的路由、App 的 RPC 层）自己算。通道库不碰时间语义。**

- **两个字段**：
  - 信封里的 `remaining_ms`（u32）：**发送那一刻**还剩多少毫秒。u32 约 49.7 天上限，超过的由 RPC 层拒绝，不静默截断。**必须排进信封的前 48 字节**（§10），Agent 出队时要读它。
  - 记录头里的 `enqueue_ts_ns`（u64）：生产者 `Commit` 的单调时钟时刻，**每条都有**，由通道库在 `Commit` 里盖，调用方不用管。
- **判定**：`现在的剩余 = remaining_ms × 10^6 − (now_ns − enqueue_ts_ns)`，`≤ 0` 就是过期。信封里的值会随排队变旧，所以要靠入队时刻把排队时间减掉。
- **无效值**：`remaining_ms == 0` 或 `enqueue_ts_ns == 0` 都**按已过期处理**，丢弃，单独计 `invalid_expire`（不与 `stale_drops` 混在一起）。发送方的 RPC 层负责不写 0（算出来剩余 `≤ 0` 就不发）。理由：相对量里的 0 本来就是"剩 0 毫秒"，口径一致；"忘了填"表现为消息全丢、测试立刻发现，而不是悄悄永不过期（RPC 设计 §4.4 批评过的"失效即放行"）。目前没有需要豁免的记录，真有再加 `flags` 位。
- **时钟**：用单调时钟（`Now::SteadyNs()`，底层是 `std::chrono::steady_clock`，libstdc++ 在 Linux 上即 `CLOCK_MONOTONIC`），**不用墙钟**：墙钟会被 NTP 校时和 GM 偏移（`Now` 的帧缓存时钟）拨动，让一批消息同时过期或同时复活。文章信封头里的 `deadline_unix_ms` 是墙钟绝对值，不适合，需要改。
- **前提**：两个进程在同一个时间命名空间，因为入队时刻是生产者写、消费者读。容器里一般成立，用了 `CLONE_NEWTIME` 才会不同；握手时 App 把自己的 `SteadyNs` 放进 `Hello`，Agent 与自己的差超过 1 秒就 `Reject`，成本几乎为零。
- **入队时刻怎么摊薄成本**：一批 `Commit` 共用一次时钟读数，取批内**首条 `TryReserve` 的时刻**，比真实入队略早，算出的年龄偏大，**只会早丢、不会晚丢**。每批一次读数的开销**没测过**（量级是几十纳秒，摊到整批），要在基准里单独量（§13）。
- **在哪检查**：
  - **入队前**：生产者（App 的 RPC 层、Agent 的路由层）在 `TryReserve` 之前判断，剩余 `≤ 0` 就不写；
  - **出队时**：解析信封的那一层在 `Peek` 之后判断，过期的 `Pop` 掉、不处理，计 `stale_drops`；
  - **环中间的记录删不掉**（§6.4 末尾那条限制），检查的精度是毫秒。通道库本身不检查，只提供记录的入队时刻。

**Agent 转发时怎么处理剩余量**（相对量在每一跳都要把排队时间减掉；网络传输耗时不扣，行业通行，毫秒级误差）：

| 路径 | 做法 |
|---|---|
| 网络 → Rx 环 | 信封原样写进环，通道 `Commit` 时盖入队时刻，**不用改信封** |
| Tx 环 → 别的 App 的 Rx 环 | 算出现在的剩余，写进**拷贝里**的 `remaining_ms`，再写入目标环 |
| Tx 环 → 网络 | 同上：把信封所在的第一个缓存行拷到本地、改 `remaining_ms`，与余下的载荷用 `writev` 一起发。**不改环里的记录**：消费者对环里的数据只读，而且 App 是不可信的 |

**为什么不在记录头放绝对时刻**（方案 A，已否决）：A 唯一的好处是环里不用做减法，代价是两套表示（环里绝对、网线上相对）、三个转换点、记录头布局要改、`TryReserve` 要多一个参数；而没有任何消费者需要在不认识信封的情况下丢记录——Agent 要解析信封做路由，App 的 RPC 层要解析信封做分发，载荷畸形也是 §7 本来就要在路由前校验的。

**行业通行的也是相对量加收到后落地**（我凭记忆写的，没有逐条核对）：gRPC 的 `grpc-timeout` 头是"数值加单位"的相对量，服务端收到转成本地 deadline，往下游发时再按剩余量重算；Envoy 的 `x-envoy-expected-rq-timeout-ms` 同理，原因是跨机器时钟不一致。RabbitMQ 的单条消息 TTL（相对毫秒）只在消息排到队头时才检查、才丢，与我们"环中间的记录删不掉、出队时才检查"是同一个取舍；fbthrift 的请求元数据里据我记忆既有客户端超时、也有"排队超时"，也是把"在队列里等太久"单独当一回事。

**不同消息的 `remaining_ms` 怎么设**（RPC 与单向通知本来就是两种语义，不能共用一个"超时"）：

| 消息 | `remaining_ms` | 过期后 |
|---|---|---|
| RPC 请求 | `ctx` 的剩余时间（只能缩短不能延长），算出 `≤ 0` 就不发 | 不执行、不回包。请求方早已判超时，"没执行"比"执行了没人要"更干净，也契合 RPC 设计的"已过期立即失败" |
| RPC 回包 | 对应请求的本机 deadline 减去 now，`≤ 0` 就不发 | 过期就丢：请求方此时已判超时，本来也会按 `seq` 丢弃它（RPC 设计 §4.1），Agent 提前丢掉等价且更省 |
| 单向 `Send` | `min(route 的 TTL, ctx 的剩余时间)`，TTL 没配就用**默认 5 秒（已定）** | 丢，计 `stale_drops`，**不通知发送方**。单向 `Send` 的语义就是"不关心结果、可以丢"（**已定**） |

- **单向 `Send` 继承 `Ctx` 里的 deadline，取较小者（已定）**：`ctx` 没设 deadline 就只看 TTL。
  - **这是不用对 `Send` 特殊处理的那一种**：`Send(t, route, body, ctx)` 本来就带着 `ctx`，`Call` 与 `Send` 共用同一个"有效剩余时间"的计算（`Call` 的底数是 route 的超时，`Send` 的是 route 的 TTL），也符合 RPC 设计 §4.4"deadline 只能缩短"。忽略 `ctx.deadline` 才是特例，要单独写代码和注释。
  - **好处**：规则与 `Call` 一致；过载时调用方超时，它链上派生的单向消息也跟着被丢，是顺手的卸载，不会在环里堆积。
  - **代价**：丢失与调用方超时相关，而不是随机的——处理函数提交了状态、再发通知，恰好调用方超时，通知就一出门即过期。**在"单向可丢"已定之后分量不重**（环满、过期本来也会丢），但它是系统性的，不是偶发的，业务要知道。
  - **与本通道无关**：`Ctx` 在 RPC 层，通道只盖入队时刻、不读剩余量；规则由 RPC 层在写信封前算好。`Ctx.deadline` 的零值（Go 的 `time.Time{}`）表示"没设"，由 RPC 层解析，**不要**和信封里 `remaining_ms == 0 = 无效` 混为一谈。
- **默认 TTL 定为 5 秒（已定；没有测量依据，正路是每条 route 自己配）**。注意环容量本身也限制了 TTL 的实际作用：16 MiB 的环在 200 B 载荷、10k msg/s 下约 6.5 秒就满（§8），TTL 设得比这长，决定丢不丢的就是环满而不是时间。
- **单向 `Send` 至多一次、尽力而为（已定）**：过期、环满、保留期结束、Agent 崩溃时已经 `Pop` 的，都不重发、不通知发送方，只计数（`stale_drops`、`rx_full`、`rx_full_detached`）供运维看。**必须送达的不要用单向 `Send`，用 RPC（有回包、有超时与重试）。** 这同时回答了 RPC 设计未决项里的"单向通知的可靠性"：通道一侧不提供至少一次档位。

## 5. 唤醒协议

**目标**：闲时 0% CPU（消费者在 `epoll_wait` 里睡），忙时 0 次系统调用（消费者醒着，生产者不用敲 eventfd）。

状态：`write_idx`、`read_idx`、`consumer_sleeping`（0 = 醒着，1 = 要睡了）。

**生产者发布**：

```
write_idx.store(w, release)            // 记录写完之后才发布
atomic_thread_fence(seq_cst)
if (consumer_sleeping.load(relaxed) != 0 &&
    consumer_sleeping.exchange(0, acq_rel) != 0)   // 只有一个生产者，exchange 防止重复敲
        eventfd_write(efd, 1)
```

**消费者准备睡觉**（先把队列搬空）：

```
consumer_sleeping.store(1, relaxed)
atomic_thread_fence(seq_cst)
if (write_idx.load(acquire) != 本地读游标)      // 回头再看一眼：这个窗口里有人写进来了（批量释放时本地游标可能领先 read_idx）
        { consumer_sleeping.store(0, relaxed); 继续干活 }
否则 epoll_wait(...)                            // 被 eventfd 唤醒；醒来后 read(efd) 清计数，再把标志置 0
```

- **正确性思路**：两边都是"先写自己的标志、插 `seq_cst` 栅栏、再读对方的标志"（Dekker 模式）。要么生产者读到 `sleeping == 1`、去敲 eventfd；要么消费者读到新的 `write_idx`、不睡。两者不可能同时漏掉，所以不会丢唤醒。**这只是设计依据，必须用测试验证（§12）**，不能只靠推理。
- **eventfd 用 `EFD_NONBLOCK | EFD_CLOEXEC`**；允许虚假唤醒（消费者醒来发现没数据就再走一遍睡眠流程）；计数累积没关系，醒来一次性读清。
  两种"白醒"分开统计（都在环控制块的 `consumer_stats` 里）：`spurious_wakeups` 是 eventfd 本来就空；`empty_wakeups` 是 `WokeUp` 读到了计数、但随后第一次 `Peek` 就是空的——
  生产者在消费者睡前回头一看的窗口里敲了 eventfd，记录却已在那条路上被收走，计数成了旧的。实测很少：单条在途 0%，64 条在途全速占醒来的 0.3%~2.6%，绝对数是 2870 万条里几百次。
- **睡前自旋是调用方的可选策略，默认关**：`base/shm` 里**没有自旋**，只提供 `PrepareToSleep` / `WokeUp` 与 eventfd。App 主循环（业务单线程）**不自旋**：每次 tick 或 eventfd 可读时在预算内搬空环，进 `epoll_wait` 之前调 `PrepareToSleep`，返回 false 就继续处理，醒来调 `WokeUp`。自旋只有专用的轮询线程上才可能值得（比如 Agent 的 IO 线程在高负载下），时长要由基准决定；**之前写的"20–50µs"是猜的，作废**。不自旋的代价：每次从空闲变忙，生产者多一次 eventfd 写、消费者多一次 epoll 唤醒（我没测过，量级是一次系统调用加一次调度）。
- **睡眠标志这套协议为什么仍然要有**：它让生产者只在消费者真要睡的时候才敲 eventfd，忙的时候 `Commit` 一次系统调用都没有；不要它也行，代价是每次 `Commit` 都 `write` 一次 eventfd，好处是没有 Dekker 与栅栏这一类难测的竞态。
- **Agent 一侧**：每个通道的 eventfd 挂进 Agent 的 `EventLoop`（`EventLoop::Add(IPollable*, fd, events)` 能挂任意 fd）。**App 一侧**：Rx 的 eventfd 挂进 App 主循环的 `epoll`。
- **每条环各有自己的一份标志与 eventfd**：一个进程在一条环上是消费者，在另一条环上是生产者。

## 6. 握手、挂载、崩溃与保留

### 6.1 握手（UDS）

握手消息是**定长小结构**，不用 Protobuf，减少依赖。

```
App   ── Hello{ 协议版本, busid, 挂载类型, Tx 容量, Rx 容量, pid, 标志 } ──▶ Agent
Agent ── Accept{ resumed, app_epoch, agent_epoch, 两条环的容量与偏移 } + SCM_RIGHTS[memfd, tx_efd, rx_efd] ──▶ App
Agent ── Reject{ 原因 } ──▶ App
```

**实现（线格式 v2，`base/net/shm/shm_protocol.h`）**：Accept 与 Reject 合成一条定长的 `Reply`（80 字节）：`result == 0` 即 Accept；Attach 新建 / 恢复时随这条消息用
`SCM_RIGHTS` 附上 `[memfd, tx_efd, rx_efd]`，Reattach 的 Accept 不带 fd（App 手里本来就有）；否则 `result` 是 errno（Reject），不带 fd。Hello 是 112 字节，
握手之后 App → Agent 还有 16 字节的定长 `Control`（§6.6）。两端同机，结构按本机字节序与布局直接收发，头里有魔数（字节序列 `BSHM`）与版本，改了任何字段就升版本（现在是 2）。
`busid` 是定长 64 字节的字符串，字符集 `[A-Za-z0-9._:@-]`、1 到 63 个字符。
Hello 里有 `transport`（`UdsOnly` / `ShmPreferred` / `ShmRequired`）与 `flags`（目前只有 `discard_retained`），Reply 里有 Agent 实际选定的 `transport`（`Uds` / `Shm`）与 `resumed`；
**阻塞的 `ShmChannelClient::Connect` 只发 `ShmRequired` 并要求回复是 `Shm`**；协商（含 `UdsOnly` 与退化）由非阻塞的 `AgentClient` 做（见下面"传输方式的协商"）。

**传输方式的协商（已实现）**：Hello 里的 `transport` 是 App 的意愿，Reply 里的 `transport` 是 Agent 实际给的。

| App 要 | Agent 给 | 说明 |
|---|---|---|
| `UdsOnly` | `Uds` | 不创建共享内存；握手之后这条 UDS 交给 `StreamConnection` 当数据通道（帧由 `IFrameCodecFactory` 切，两端要一致）；Agent 没提供数据通道（没给 `ShmUdsDataPath`）则 `Reject(ENOTSUP)` |
| `ShmPreferred`（默认） | `Shm`；给不了时 `Uds` | **只有"可用性"类的失败才退化**：共享内存总量配额满（`ENOSPC`）、`memfd` / eventfd 创建或登记失败（`ENOMEM` / `EMFILE` / `ENFILE` / `ENOSYS` / `EPERM`）、Agent 配了 `allow_shm = false`；App 自己的问题（容量不合法 `EINVAL`、准入被拒 `EACCES`、名字被占 `EBUSY`）照常拒绝，**不拿退化掩盖**；通道数配额满也照常拒绝（仅 UDS 也占名额） |
| `ShmRequired` | `Shm` | 给不了就 `Reject`（`ENOSPC` / `ENOTSUP` ……） |

- **通道名、配额、准入在两种传输之间统一**：仅 UDS 的通道和共享内存的通道共用一个 busid 命名空间与 `max_channels`，都要过 `OnAdmit`；保留中的共享内存通道不能被仅 UDS 的同名 Hello 顶替，要换传输方式得带 `discard_retained` 明说（只还给同一个 uid）。
- **仅 UDS 的对端的生命周期**：UDS 断了就是连接关闭（没有队列可保留，也没有 Reattach）；业务回调里 `IConnection::GetContext<UdsPeerContext>()` 取到它的 busid / `SO_PEERCRED`；服务端停机会关掉它们；**`~ShmChannelServer` 返回之后不会再有这些连接的回调**（要等它们回收完，见 §12 的坑）。
- **App 侧**：`AgentClientOptions::transport`；`NetModule` 的配置 `net.agent_shm_enabled`（false = `UdsOnly`，true = `ShmPreferred`）。连上之后 `AgentClient::link()` 告诉你实际走的是哪一种。`Reattach` 只能是共享内存（`CheckHello` 拒绝 `UdsOnly` 的 Reattach）。

`挂载类型`有两种：**`Attach`**（App 来要通道：新建，或恢复 Agent 持有的那一个，§6.2）和 **`Reattach`**（Agent 重启之后，App 把手里的 memfd 传回去，§6.5；这一种的 fd 方向相反，由 App 随 Hello 一起传给 Agent）。

**容量由 App 在 Hello 里指定**：

- Tx 环、Rx 环各一个值，**填 0 = 用默认（16 MiB）**；需要更大的 App 自己传（比如 Rx 环要攒更久的重启积压）。
- **必须是 2 的幂**，不是任意的 2 的倍数：游标用掩码取位置、镜像映射要整页，只有 2 的幂两样都成立。范围是 `[1 MiB, Agent 的单环上限]`，上限默认 256 MiB。
  **不需要再单独要求是 64 字节的倍数**：任何不小于 64 的 2 的幂天然是 64 的倍数，下限 1 MiB 又远大于页大小（x86 是 4 KiB，部分 ARM64 内核是 64 KiB），页对齐和缓存行对齐都自动满足，校验只写 `是 2 的幂 且 在范围内` 一条。
- **不满足一律 `Reject(EINVAL)`，不静默取整、不静默裁剪**：配置错了应该在开发期就暴露；超过 Agent 的总配额则 `Reject(ENOSPC)`。
- **恢复时沿用原容量**：保留下来的 memfd 已经定了大小，且已封印，不能再改。恢复时 Hello 里填 0 或者与现有值一致；不一致就 `Reject(EINVAL)`；想换大小就带上 `discard_retained` 标志，**丢弃保留的队列**，按新容量重建。

**其它规则**：

- **身份**：Agent 用 `SO_PEERCRED` 取对端 uid / pid，不只信 Hello 里自称的 busid；是否放行（uid 白名单）是 Agent 的策略。
- **memfd 在 Agent 发出之前加封印**：`F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL`。否则对端 `ftruncate` 缩小它，另一边一访问就收到 `SIGBUS` 崩溃。
- **服务端的处理顺序**（拒绝的代价依次递增，都在创建共享内存之前或之中）：Hello 的固定部分（魔数 / 版本 / 类型 / 标志 / busid，`EPROTO` / `ENOTSUP` / `EINVAL`）→
  busid 唯一（`EBUSY`）→ 通道数配额（`ENOSPC`）→ 准入回调（身份 / 策略，errno 原样回给对端）→ 创建通道（容量不合法 `EINVAL`）→ 共享内存总量配额（`ENOSPC`）→
  登记 Tx 的 eventfd → 发 Accept。
- **握手是不可信的入口**：服务端用 `recvmsg` 读 Hello（Reattach 的 Hello 要带 fd，读之前不知道类型），只给 4 个 fd 槽位、只读还差的字节数，收齐才处理：
  Attach 带了 fd 是 `Reject(EPROTO)`；带的 fd 比槽位多（控制缓冲被截断）整条当协议错误、不回复直接断开，已收到的随 `UniqueFd` 关闭；
  待握手的连接数有上限（`max_pending_handshakes`，超过直接断开），握手有超时（`handshake_timeout`，默认 5 秒）；握手之后 UDS 上只许有定长的 `Control`（§6.6），别的字节一律是协议错误并关闭（第一个不对的字节就识别，不等攒够 16 字节）。
  客户端一侧同样不信 Agent：读 Reply 用 `recvmsg`、只给 4 个 fd 槽位（期望 3 个，多了是协议错误，已收到的全部关闭），校验头、`result`、fd 的种类（封印齐全的 memfd、
  非阻塞的 eventfd），再用 Reply 里的布局与共享内存头里算出来的互相核对，对不上就拒绝。
- **通道刚建好要先跑一次消费循环**：它收尾的 `PrepareToSleep` 才会把 `consumer_sleeping` 置 1，App 的第一次 `Commit` 才知道要敲 eventfd；否则标志一直是 0，Agent 永远等不到第一声。
  这件事由 `ShmChannelServer` 做，回调里不用管（唤醒协议整个收进了服务端，见 `shm_channel_server.h`）。
- **`EBUSY` 与快速重启**：App 快速重启时，Agent 的事件循环可能还没处理旧连接的 EOF。两道防线：服务端见到同名 Hello 时**先探一下旧连接**（`ProbeAlive`：UDS 的 EOF 在对方退出时就已经排在内核里），断了就当场转入保留再恢复；
  客户端在 `busy_retry` 窗口内（默认 1 秒）按 5ms 起翻倍、至多 100ms 的间隔重试 `EBUSY`，最后一次尝试恰在窗口结束处。
  **服务端的探测什么时候才用得上**：IO 线程按就绪的先后处理事件，普通的快速重启（旧进程先退出、新进程才连）旧连接的 EOF 总是先于新 Hello 被处理，探测用不上——`TestRapidRestartNeverSeesBusy`（`busy_retry = 0`，连续 200 次）只证明这一点，
  **去掉探测它照样过**（变异验证里存活，我最初写成"证明只靠探测就够"是错的）。探测只在"新进程先连上、旧进程随后才退出"（滚动更新时新旧重叠）时才起作用：新 Hello 先就绪、旧连接的 EOF 后就绪。
  `TestHelloBeforeOldEofStillResumes` 把 IO 线程卡在一个回调里，卡住期间先发新 Hello、再关旧连接，造出这种次序；没有探测就是 `EBUSY`。
- **Reject 的原因**（对应 errno，沿用 rudp 的风格）：`EBUSY` 同一 busid 已有活着的 App、`ENOSPC` 超过通道数或共享内存总量的配额、`EPROTO` 版本或布局不符、`EACCES` 身份不被允许、`EINVAL` 容量不合法或 Reattach 的 fd 校验不过。

### 6.2 通道的状态

```
   (无)  ──Hello──▶ Attached ──UDS 断开──▶ Detached ──超过保留时间──▶ 销毁（munmap + close memfd）
                       ▲                      │
                       └──── 同一 busid 的 Hello（保留期内）────┘   resumed = true
```

- **Attached**：App 在线，双向正常。
- **Detached**：UDS 断开（App 崩溃、`kill -9` 或主动退出）。Agent 立刻感知（EPOLLRDHUP / EOF）。此时：
  - **Agent 继续消费 Tx 环里剩余的消息**，把崩溃前已经提交（`write_idx` 已发布）的"遗言"发完。写到一半的记录不会被看见，因为发布靠游标。
  - **Rx 环继续接收**发给这个 App 的消息，攒着，直到保留期结束或环满。
  - 启动保留计时器，**默认 30 秒**，可配置，App 退出前可用 `PrepareRestart(reserve)` 显式设置（热更新场景给长一点）。
- **resumed**：保留期内同一 busid 再来 `Attach`（身份校验同上），Agent **把同一个 memfd 再传一遍**，`app_epoch` 加一，`resumed = true`；App 接着用共享内存里的游标，看到的 Rx 环里就是重启期间积压的消息。
- **销毁**：保留期结束没人来，Agent 关掉 memfd；环里没读走的消息作废，计入指标，**这时业务看不到丢失，除非发送方设置了回执**（回执属于路由层，不在本文范围）。

**实现（`ShmChannelServer`，`shm_channel_server.h` 文件头有完整说明）**：

- **通道对象跨保留不换**：一条通道对应一个 `ShmServerSession`，id 从建立到销毁不变；重新来握手的连接（一个还在握手的会话）查到它，把 UDS 交过去（`ResumeOld`），握手会话随即销毁。
  **业务回调里的映射要以 `ShmServerChannel::id()` 为键，不要只用 busid**：`discard_retained` 重建时，同名的新通道会先于旧通道的 `OnClosed` 出现（事件序列 `Opened Detached Opened Closed(Discarded)`）。
- **回调**：`OnOpened`（`reattached()` 为真表示 Agent 重启后恢复的）→ `OnDetached(retain)` → `OnResumed` / `OnClosed(RetentionExpired)`。`OnReadable` 在 Detached 期间照常被调用（遗言），`rx()` 仍可写；`detached()` 告诉业务此刻 App 不在。
- **保留多久**：`retain_default`（默认 30 秒，0 = 不保留）；App 退出前 `PrepareRestart(ms)` 只管**下一次**断开，超过 `retain_max`（默认 10 分钟）按上限算；`Bye` = 不保留。
  **不保留（`Bye` / `retain_default` 为 0）时先排空遗言再销毁**（`OnClosed(PeerBye / PeerClosed)`），排空有上限 `drain_timeout`（默认 5 秒）兜底，不能让一个还在写的对端把通道一直拖着。
- **只把"UDS 读到 EOF / ECONNRESET"当 App 没了**：`ECONNRESET` 是 App 没读完 Accept 就被 kill 时内核的报法，同样进保留；其余 IO 错误、协议错误、`Close()` 一律直接销毁，不替不老实的对端留队列。
- **恢复的规矩**：保留着的队列只还给**同一个 uid**（`SO_PEERCRED`，在准入回调之前就拒绝，`EACCES`）；沿用原容量，Hello 里的容量 / 载荷上限填 0 或与现有值一致，否则 `Reject(EINVAL)`（被拒绝不毁掉保留着的队列）；
  想换大小就带 `discard_retained`：**新通道建好、过了配额之后才丢旧的**（容量不合法时旧队列还在），新通道名在旧的开始关闭时就放出来，丢弃的旧通道占的配额在判断时抵扣。
  `app_epoch` 在回复**之前**加一（客户端一收到回复就拿头里的值对账，`Superseded()`）；Agent 的 epoch 不变。
  同名通道的 App 还活着（探测后仍然在线）就 `EBUSY`，不抢。
- **统计**：`detached_channels` / `detached_total` / `resumed` / `reattached` / `retention_expired` / `discarded` / `byes` / `inflight_skipped` / `rx_bytes_discarded`。
  这些计数在回复**之后**才更新，事件回调（`OnDetached` 等）又晚于计数：测试里要 `WaitUntil`，不能读到计数就去读事件。

### 6.3 保留语义的几个细节

- **默认至多一次**：Rx 环的 `read_idx` 在 `Pop` 时推进。App 在 `Pop` 之后、处理完成之前崩溃，这条消息就丢了。想要至少一次的 App，用 `Peek` 加延后 `Pop`，处理完再释放。
- **重启后积压的消息可能已经过期**：App 恢复时可以选"全部跳过"或"逐条处理"；信封里的 `remaining_ms` 加记录头里的入队时刻（定义见 §4"过期怎么算"，单向消息都带 TTL）让 App 或 Agent 在消费时丢掉过期的，避免重启后被陈旧请求冲垮。
- **epoch 防过期进程**：每次挂载对应角色的 epoch 加一，写入头部；进程用自己拿到的 epoch 与头部比对，不一致说明有新进程接管了这个角色，旧进程要自行退出。
- **满了怎么办**：Detached 期间 Rx 环满了，Agent 丢**最新**的（保住已经排好的、保证顺序），计 `rx_full_detached`；Attached 期间满了同理，计 `rx_full`。
- **App 活着但卡死**：UDS 没断，Agent 看到的是 Rx 环越积越多。检测办法见 §6.4（看门狗）。
- **布局版本不符**：Accept 时 Agent 校验头部 `layout_version` 与 `record_align`，保留的旧 memfd 与新 App 的版本不兼容就拒绝恢复、销毁旧队列。

### 6.4 App 卡死的看门狗

**什么是卡死**：App 进程还活着，UDS 连接没断，但主循环不再消费消息了。

**为什么 UDS 没断还会卡死**：UDS 只在**进程退出**的时候才由内核关闭，一个挂住的进程仍然握着所有 fd，连接就一直开着；传输层的"存活"不等于应用层的"存活"（TCP keepalive 也一样，内核会替挂住的进程回应）。主循环卡住的常见原因：

- **死锁**：两个线程互相等对方的锁，或者主循环等一个永远不会来的条件。
- **死循环**：业务逻辑里的 bug，某一帧里出不来。
- **阻塞在系统调用里**：同步访问数据库、磁盘 IO 停滞、网络文件系统挂起、不可中断睡眠（D 状态）。
- **被外部暂停**：`SIGSTOP`，调试器附着（gdb 一断点整个进程停住），`ptrace`。
- **资源被卡住**：容器的 CPU 配额用完被节流，内存压力下被换出，大锁（比如 `malloc` 的竞争）上排长队。

**后果**：它的 Rx 环越积越满，满了之后发给它的消息被丢；而路由层和控制面都还以为它是好的，继续往它发。

**行业怎么解决**（凭一般经验；有的属于运维层，不在我们库里）：

| 做法 | 例子 | 要点 |
|---|---|---|
| **把"卡死"变成"崩溃"** | 游戏服务器常见的**帧耗时看门狗**：一个独立线程盯主循环的计数，某一帧超过 T 秒就打印所有线程的栈，然后 `abort()` 留 core | 最稳：崩溃是我们已经处理好的路径（UDS 断开、保留、恢复），卡死不是 |
| **工作进程定时报到，主进程杀掉不报到的** | gunicorn 的工作进程每轮循环刷新一个心跳文件，主进程发现太久没动就杀掉重启；systemd 的 `WatchdogSec` 要求服务定期 `sd_notify` | 同一思路：存活要靠"我还在前进"证明，不靠连接还在 |
| **编排器的存活探针** | K8s liveness probe：探测失败就重启容器 | 探针要**由主循环来响应**，否则主循环卡了探针照样通过 |
| **租约 / TTL** | etcd 租约、ZooKeeper 会话、Consul 的 TTL 检查 | 进程不续约就从服务发现里摘掉 |
| **调用方侧的离群剔除、超时与熔断** | Envoy 的 outlier detection；请求带 deadline | 不管对端为什么没响应，调用方都不会无限等 |
| **主线程卡住的告警** | Android 的 ANR 与系统服务的 Watchdog | 同样是"盯主线程有没有前进" |

**我们的分层办法**：

1. **App 自救（已定：现阶段的处理手段）**：`framework/app` 里加一个自检线程，监视主循环的帧计数，**超过 T 秒（默认 30 秒，必须大于最长的正常帧）就打印所有线程的栈并 `abort()`**。App 一崩，就走 §6.2 的 `Detached` → 保留 → 恢复，积压的消息留着，重启后按 `deadline` 丢过期的。本机用 `base/backtrace` 打栈即可。调试器附着时（`/proc/self/status` 里 `TracerPid` 非 0）自检线程应当暂停，免得开发时一下断点进程就被杀。**这一项目前没有，属 `framework/app` 的事，不在 `base/shm` 里；它自己在 App 进程内工作，不依赖 UDS 上的任何控制消息。**
2. **Agent 侧检测（兜底）**：**基于消费进度**，不需要 App 配合——
   - Agent 每秒采样每个通道 Rx 环的 `read_idx`；
   - 环**非空**，且 `read_idx` 连续 **N 秒**（默认 **10 秒**，可配置）没动，判 `Stalled`；
   - **满得更快就判得更早**：Rx 环已经**满了**而 `read_idx` 连续 **2 秒**没动，也判 `Stalled`。原因：默认 16 MiB 的环在高流量下几秒就满了（200 B 载荷、10k msg/s 约 6.5 秒），不能等 10 秒才发现，否则检测出来之前新消息已经开始被丢。
3. **判为 `Stalled` 之后的动作**（分阶段）：

   | 阶段 | 条件 | 动作 |
   |---|---|---|
   | 第一阶段 | 刚判 `Stalled` | 置指标 `stalled = 1`、打 WARN；**通知控制面把这个 Endpoint 置 standby**，让 P2G 不再选它；发给它的 P2P 消息仍入队，环满了丢最新的并计数；若路由层有回执，对发送方回"目标卡死" |
   | 第二阶段 | `Stalled` 持续超过 M 秒（默认 30 秒，可配置） | **暂缓，只记录**：Agent 经 UDS 向 App 发 `Probe` / `Abort` 让它自己 `abort()`（设计储备见 §6.6）。第一版 Agent 在这一阶段**什么都不做**，卡死由 App 自救线程处理；Agent 自己也不 `kill` App——Sidecar 与 App 往往在不同容器、不同 PID 命名空间里，Node Agent 模式下更是别的 Pod |
   | 恢复 | 环又开始被消费 | `stalled` 清零，通知控制面恢复 ready；已入队的消息正常消费，过期的靠 `deadline` 丢 |

   **第一版只实现第一阶段里的"记指标加日志"**；通知控制面、第二阶段（`Probe` / `Abort`）、liveness 探针依赖控制面、控制线程与编排的接口，后做。
4. **局限**：环是空的时候检测不到（没有消息要读，也就没有伤害），消息来了之后要等 2 到 10 秒才会被发现，这段时间里发给它的消息仍会堆积、满了就丢。这个窗口靠发送方的 `deadline` 和超时兜底。

**卡死期间 Agent 又收到发给这个 App 的消息**：Agent 是 Rx 环**唯一的生产者**，只能往里写，推进不了 `read_idx`（那是消费者 App 的游标）。所以它能做的只有"入队、满了丢、别再发过来"：

| 阶段 | Agent 对新消息的处理 |
|---|---|
| 卡死刚开始，环里还有空位 | 照常写进 Rx 环。这时 Agent 还不知道它卡了，要到 2 到 10 秒后才判 `Stalled` |
| 环满了 | `TryReserve` 返回 `Full`，**丢最新的**（保住已排好的顺序），计 `rx_full`；发送方靠自己的 `deadline` 与超时发现失败，路由层若有回执可以回"目标卡死"（路由层的事，本文不展开） |
| 判 `Stalled` 之后 | 置 `stalled = 1` 并打 WARN；**P2G 不再选它**（要控制面把它置 standby，第一版没有，只有指标）；发给它的 P2P 仍照常入队直到满 |
| App 自救线程 `abort()`（T = 30 秒） | UDS 断开 → `Detached`：Rx 环**继续接收**直到满（`rx_full_detached`）；保留期内 App 重启并挂载回来就接着读，**过期的靠 `deadline` 丢**，没过期的正常处理 |

**这里有一个绕不开的限制**：环里已经排好的、马上就要过期的消息，Agent **清不掉**（清环要推进 `read_idx`，而那是消费者的写权限），所以环可能被一批注定作废的消息占满，之后到的新消息反而被丢。缓解办法只有两条：Agent 入队时就把 `deadline` 已经过去的丢掉；App 恢复后按 `deadline` 快速跳过过期的记录（最坏要扫过整条 16 MiB 的环）。**"这个 App 已经卡死，要不要干脆拒收"是路由层的决定**，Agent 一侧只提供 `stalled` 状态。

**为什么不用心跳**：App 主循环每帧在控制块里递增一个计数，Agent 看它停没停，这样**连环是空的时候的卡死也能发现**；但它要 App 配合，阈值也难选（有的帧本来就长，会误判）。进度检测只在有积压时才触发，不会误伤忙碌但健康的 App。**但 App 自救用的帧计数就是同一个东西**，所以将来 Agent 侧加心跳几乎没有额外成本，留作后续可选项。

### 6.5 Agent 崩溃，App 没崩

**要求**：Agent 崩溃或重启时，App 不崩，**继续使用原来的共享内存**，不新建。

**为什么做得到**：memfd 和两个 eventfd 在 App 进程里也各有一份 fd 和映射。Agent 死了，内核不会回收它们，因为 App 还握着。

**App 一侧**：

```
 Attached ──UDS 断开──▶ AgentDown ──重连并 Reattach 成功──▶ Attached
```

- **`AgentDown` 期间，App 的主循环不受影响**。Tx 环照常写，直到写满，满了 `TryReserve` 返回 `Full`，由上层处理；Rx 环里剩余的消息照常读完。
- App 在后台按退避重连 UDS 路径，不放弃；Agent 缺席了多久用指标 `agent_down_seconds` 暴露，要不要告警由 App 的上层决定。

**Agent 重启后**：App 连上来，发 `Hello{挂载类型 = Reattach}`，并随这条消息把 `[memfd, tx_efd, rx_efd]` **传回给新 Agent**。

新 Agent 不认识这个 memfd，里面的内容都不可信，所以要校验（任何一项不过都 `Reject(EINVAL)`，**不尝试修复**）：

1. fd 确实是 memfd，且封印齐全（`fcntl(F_GET_SEALS)` 含 `F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL`）；
2. 头部的魔数、版本、`layout_version`、`record_align`、容量与偏移自洽，容量在配额内；
3. 两条环的游标合法（§7 的校验）；
4. 这个 busid 当前没有别的活着的通道；
5. `SO_PEERCRED` 的身份被允许；
6. 通过后 `agent_epoch` 加一写入头部，回 `Accept{resumed = true}`。

**实现（`ShmChannelClient::Reattach` 与 `ShmChannelServer` 的 Reattach 路径）**：

- 客户端：`AgentGone()` 为真之后调 `Reattach(options)`（阻塞，同 `Connect`；`busy_retry` 窗口内重试 `EBUSY`），本对象的映射与 `tx()` / `rx()` 原样接着用；
  回复必须是 `resumed == 1`、不带 fd、`transport == Shm`、布局与手里的内存一致、`app_epoch` 与本进程一致（否则 `ESTALE`：被别的进程接管，本进程该退出）、`agent_epoch` 不倒退；失败时本对象保持原样，可以再试。
- 服务端：App 交来的三个 fd 走 `ShmChannel::Attach` 的全套校验（封印、大小、头、布局、容量上限、两条环的游标——对端此刻活着，校验对此是安全的），再核对通道数与总量配额、准入回调；
  通过后 `RecoverFromCrash`（跳过"处理到一半"的那一条，`inflight_skipped` 计数）、清 `consumer_sleeping`、`agent_epoch` 加一、回 `Accept{resumed = 1}`（不带 fd）、先跑一轮消费循环。
- **Reattach 命中还留着的通道**：Agent 其实没重启，只是 App 这边的 UDS 断了（`shutdown` 一下就能造出来）。通道名在 Agent 里还留着时，服务端用 `fstat` 的 inode 核对 App 交来的是不是**同一块内存**：是就原样恢复
  （原来的 App，`app_epoch` 不变，不再发 fd，`agent_epoch` 不变）；不是就 `EBUSY`。App 已经被别的进程用 `Attach` 接手时，原进程的 Reattach 要么 `EBUSY`（新进程还活着），要么在新进程走后得到 `ESTALE`（`Superseded()` 为真）。
- **清 `consumer_sleeping` 其实是冗余的**：接手后的第一轮消费循环无条件扫一遍，收尾的 `PrepareToSleep` 会重新置位（变异验证里去掉它测试照样过）。留着是为了把"标志与实际状态不符"的窗口缩到零。

**恢复后的状态**：

- **游标原样沿用**，它们本来就在共享内存里。
- **`consumer_sleeping` 要重置**：Agent 是 Tx 环的消费者，旧 Agent 如果是睡着时死的，标志可能停在 1。新 Agent 先把它置 0，**无条件扫一遍 Tx 环**，再进入正常的睡眠协议。eventfd 里残留的计数最多引起一次虚假唤醒，无害。

**Agent 不在期间的消息**：

- App 写进 Tx 环的消息留在环里，Agent 回来后继续发出去。**这些消息可能已经过期**，靠信封里的 `remaining_ms` 与记录头里的入队时刻（§4）在消费时丢掉（计入 `stale_drops`）；Agent 不在期间排的队，年龄一直在涨，恢复后大多会被丢。
- Rx 环没有生产者，App 读完剩下的就空了；发往这个 App 的消息，在别的 Agent 那里会因为目标不可达被丢或打回（这是路由层的事，本文不展开）。

**投递语义与 Agent 的释放策略**（已定：**Tx 方向至多一次**，丢了靠上层重传）：

两种朴素做法各有毛病：

- **字面上的"先 `Pop` 再转发"**：要先把记录拷到 Agent 自己的内存，才能释放环里的槽位（否则生产者会覆盖还在用的数据），每条消息多一次 `memcpy`。
- **"先转发再 `Pop`"**：Agent 死在转发之后、`Pop` 之前，这条会被新 Agent 重发（重复）；**更糟的是毒包**——如果这条记录本身会让 Agent 崩溃（畸形包触发了 bug），它还留在环里，新 Agent 恢复后又读到它、又崩，**陷入崩溃循环**。

**做法：先标记，处理完再 `Pop`，崩溃后新 Agent 跳过被标记的那条**：

- Tx 控制块里有一个只有 Agent 写的 `agent_inflight_idx`，**初值是哨兵 `kNoInflight = UINT64_MAX`**（初值写 0 会和 `read_idx == 0` 撞上，让新 Agent 误跳过第一条）。Agent 即将处理游标为 `i` 的记录**之前**先写 `agent_inflight_idx = i`（`release` 存储，再加一道编译器栅栏，保证标记先于后面的处理），处理完（`send` 已返回、或已拷进连接待发缓冲、或已拷进目标环）立刻**逐条** `Pop`，把 `read_idx` 推进到下一条。
  只在真要交接的时候才写标记：因背压（连接待发缓冲满了）决定先不处理这条时，要把标记清回 `kNoInflight`，否则此时崩溃会把一条根本没处理过的记录跳掉。
- Reattach 时新 Agent 比较：**`agent_inflight_idx == read_idx`** 说明崩溃发生在处理这条记录的期间，**把它当作已经处理过，跳过**——校验记录头（长度、对齐、不越过 `write_idx`）之后按其长度推进 `read_idx`，计入 `inflight_skipped`；`agent_inflight_idx < read_idx` 或为哨兵，说明崩溃发生在两条记录之间，什么都不用跳；`agent_inflight_idx > read_idx` 不可能出现，按页被写坏处理，拒绝 Reattach。这个页 App 也映射着、能写，所以标记同样是**不可信输入**，只能决定"跳不跳一条已校验过的记录"，不能决定别的。
- **效果等价于"先 `Pop` 再转发"**：每条记录至多被处理一次，转发了就不重发，没转发就丢；**同时没有额外的拷贝，也不会被一条毒包拖进崩溃循环**。代价是每条记录多一次对 Agent 独占缓存行的普通存储，App 不读它，没有缓存一致性流量。
- **已经 `Pop`、还在 Agent 内存里没发出去的**（连接待发缓冲）：丢，不重发，上层靠 RPC 的超时与重试补。下游仍应按 `seq_id` 做幂等——重试本身就会产生重复，与这里无关。
- **Rx 方向同理**：App 默认在处理前就 `Pop`（至多一次）；选择"`Peek` 加延后 `Pop`"的 App 会遇到同样的毒包崩溃循环，可以用同样的标记（`inflight_idx` 由 App 写），否则要自己处理。

**两边同时死**：memfd 没有人持有，内核回收，新 App 来 `Attach` 就是全新通道，没有任何恢复。整台机器重启同理。

**Node Agent 崩溃**：N 个 App 各自独立 `Reattach`，互不依赖。新 Agent 启动后要给一个**接纳窗口**，期间不拒绝老通道的 Reattach，也不因为"通道数还没到齐"而判异常。

### 6.6 UDS 控制消息与 Agent 触发的 `Abort`（暂缓，只记录）

**状态（2026-10-31 决定）**：**先由 App 自己管卡死**（§6.4 的自检线程），**Agent 不发 `Abort`**。本节除 `PrepareRestart` / `Bye` 外都是设计储备，第一版不实现；等 App 自救线程跑起来、真有"自救也救不了"的案例再回头评估。

握手完成之后，UDS 连接不只是存活探针，还可以作为一条**定长消息的控制流**（实现里是 16 字节的 `Control{头, op, arg}`，`CheckControl` 校验：头不符 `EPROTO`，op 不认识 / 参数与 op 不匹配 `EINVAL`；`PrepareRestart` 的参数不能是 0，`Bye` 不带参数）：

| 方向 | 消息 | 用途 | 第一版 |
|---|---|---|---|
| Agent → App | `Probe` | 问 App 的控制线程"主循环还在前进吗" | 暂缓 |
| App → Agent | `ProbeAck{帧计数, 距上一帧的毫秒数}` | 回答 `Probe`，由控制线程回，不经过主循环 | 暂缓 |
| Agent → App | `Abort{原因}` | 让 App 自己 `abort()`（§6.4 第二阶段） | 暂缓 |
| App → Agent | `PrepareRestart{保留毫秒数}` | App 退出前设置保留时长（§6.2） | **已实现**（`ShmChannelClient::SendPrepareRestart`，单位毫秒，只管下一次断开） |
| App → Agent | `Bye` | App 主动正常退出，不必等保留期 | **已实现**（`SendBye`；Agent 排空 Tx 就销毁，`OnClosed(PeerBye)`） |

**若将来做 `Probe` / `Abort`，App 一侧要由库起一个控制线程**（第一版不起：UDS 由 `ShmChannelClient` 挂在 `EventLoop` 的 IO 线程上读，`PrepareRestart` / `Bye` 在那里发就够了）：

- **它独占读 UDS**，并且**必须独立于主循环**：如果由主循环来读 UDS，主循环卡死时 `Abort` 永远收不到，这条消息就没有意义。
- 它的活是**回应 `Probe`**。App 自救用的**自检线程**（盯主循环的帧计数，§6.4）是另一个东西、属 `framework/app`；两者将来可以合并成一个线程。
- 它**只碰原子变量，不碰业务状态**，所以业务仍是单线程；库里多的就这一个线程。

**收到 `Abort` 之后应当怎样**（设计储备）：

- **只认这条连接的对端**：UDS 是已连接的流，除对端外没人能往里写；App 握手时用 `SO_PEERCRED` 核对对端 uid 与预期的 Agent 一致，消息带魔数与版本，长度或类型不对的一律忽略，不会被畸形输入误触发。
- 记录原因，**尽力打栈（带超时）**，然后 `abort()`，由内核产生 core。
- 打栈要小心：卡住的线程可能正握着 `malloc` 或日志的锁，进程内打栈本身可能也卡死，所以打栈之后要有定时兜底（`alarm` 或定时 `_exit`）。**真正可靠的是 core**：事后用 core 看所有线程的栈。容器里要确认 core 没有被关掉。
- **调试器附着时忽略**（`/proc/self/status` 里 `TracerPid` 非 0），开发时一下断点不会被杀。
- 开关 `abort_on_stall` **默认关**。Agent 误判会杀掉健康的 App（比如 App 正在加载数据，一帧本来就要十几秒），所以只在 `Stalled` 持续 M 秒之后才发，并且先 `Probe`：控制线程能回、但帧计数停滞，才算确认主循环卡死。

**它救得了什么、救不了什么**（评估记录；App 自救线程同样救不了"整个进程被停住"这一类）：

- **救得了**：主循环卡了、别的线程还能跑，也就是死锁、死循环、阻塞在某个系统调用里。
- **救不了**：整个进程被 `SIGSTOP`、处于不可中断睡眠、所有线程都阻塞、控制线程自己也被锁住。这种情况下 `Probe` 没有回应，升级为：
  - 编排器的存活探针重启容器；
  - 或者 App 握手时把自己的 **`pidfd`** 经 `SCM_RIGHTS` 交给 Agent，Agent 用 `pidfd_send_signal` 发 `SIGABRT` 或 `SIGKILL`（`SIGKILL` 连被 `SIGSTOP` 的进程也能杀）。**能不能跨容器发信号取决于 uid、能力和命名空间的关系，要实测，默认不开。**

## 7. 不可信输入与资源限制

共享内存里的字节，对端随时可以改，所以**所有从对端那里读到的东西都要当作不可信**：

- **先拷头再用**：读记录时先把 16 字节头拷到本地，再校验；不直接对共享内存里的字段做多次读取（防 TOCTOU）。
- **校验项**：`write_idx - read_idx <= 容量`、`len <= 单条上限`、记录不越过 `write_idx`、`reserved == 0`、游标没有倒退。任何一项不满足，就把这个通道置为 `Corrupt`，断开 UDS、销毁队列、计数，**不尝试修复**。
- **载荷里的路由字段**（信封头）在转发之前同样要拷到本地并校验，再去查路由表。
- **对端不能让 Agent 阻塞**：Agent 每次醒来对每个通道只处理有上限的记录数（公平调度），一个狂发的 App 不能饿死其它通道。
- **配额**：单 Agent 的最大通道数、共享内存总字节数、单通道最大容量，Hello 超出就 `Reject(ENOSPC)`。
- **Reattach 时对端交来的 fd 同样不可信**：App 可以随便交一个 fd 过来，校验项见 §6.5。
- **接手时的校验必须对"对端还活着"安全**：比如 App 重启而 Agent 一直在跑，对端还在推进游标和 `inflight` 标记。校验只能检查"合法状态下永远成立"的关系，并且按"较晚读到的一侧在合法状态下只会更大"来排读的顺序（`shm_channel.cpp` 的 `RingStateValid` 有推导）。**实现时踩过**：最初把 `inflight` 放在游标之后读、再与 `read_idx` 比较，对端运行时约有 1% 的合法接手被误拒；`shm_untrusted_test` 的 `TestAttachWhilePeersRunning` 是它的回归测试。`inflight` 与 `read_idx` 的精确关系留给消费者角色的 `RecoverFromCrash` 查（那时旧消费者已不在）。
- **memfd 封印**见 §6.1；**UDS 路径的权限**由部署保证（只有该 Pod 的 App 能连）。

## 8. 容量与背压：16 MiB 的账

**我按"每条环 16 MiB"理解**（每通道 32 MiB）。如果你指的是每个共享内存对象（含两条环）16 MiB，则每条环 8 MiB，下面的时间都减半。

每条环 16 MiB = 16,777,216 字节；记录 = 16 字节头 + 载荷，**起点按 64 字节对齐**（占 `roundup64(16 + len)`，§4）：

| 载荷 | 占用 | 能装多少条 | 10k msg/s 能缓冲 | 100k msg/s | 1M msg/s |
|---|---|---|---|---|---|
| 64 B | 128 B | 131,072 | 13.1 秒 | 1.31 秒 | 131 ms |
| 128 B | 192 B | 87,381 | 8.7 秒 | 0.87 秒 | 87 ms |
| 256 B | 320 B | 52,428 | 5.2 秒 | 0.52 秒 | 52 ms |
| 1 KiB | 1,088 B | 15,420 | 1.5 秒 | 0.15 秒 | 15 ms |

- **忙时的突发缓冲够用**：只要 Agent 不会停顿超过几十毫秒量级（100k msg/s 的 128 B 消息约 0.87 秒、1M msg/s 约 87 毫秒）。
- **对"保留 30 秒"不够用的场景要心里有数**：以 200 B 载荷（占 256 B）、10k msg/s 计，16 MiB **只能攒约 6.5 秒**，攒满 30 秒需要约 73 MiB；流量降到 1k msg/s 才能撑约 65 秒。所以 **保留时长受环容量限制，不是想设多长就多长**；热更新窗口长、流量大的服务要调大 Rx 环容量，或者接受重启期间的丢弃。
- **内存账**：Node Agent 混部 30 个 App，30 × 2 × 16 MiB = **960 MiB**（页被触碰之后才占物理内存，环用久了会全部触碰）。K8s 里这部分要算进容器的内存限制；据我所知 memfd 的页记在首次触碰它的进程所属的 cgroup 上，Agent 与 App 分属不同 cgroup 时要核算清楚，**这点上线前要在目标环境里实测确认**。
- **容量由 App 在握手时指定**（规则见 §6.1：2 的幂、`[1 MiB, 256 MiB]`、不合法就拒绝）；默认两条环都是 16 MiB，允许两条环大小不同（Rx 环通常要更大，因为有保留需求与扇入）。
- **背压语义**：环不阻塞。Tx 环满了，App 侧 `TryReserve` 返回 `Full`，业务看到的是 `SendResult::Full`（§11），由 App 的上层决定；Rx 环满了见 §6.3。两侧都要有"占用率高水位"指标（§9）。

## 9. 可观测

按需记录；**计数放在控制块里、每个写者独占自己的缓存行**，不引入共享写，Agent 汇总后对外暴露（Prometheus 等）。

| 指标 | 说明 |
|---|---|
| `tx_msgs` / `tx_bytes`、`rx_msgs` / `rx_bytes` | 每个方向的消息数与字节数 |
| `ring_occupancy_hwm` | 占用字节的高水位（定期清零重取），两条环各一份 |
| `full_events` | 生产者遇到 `Full` 的次数；`rx_full_detached` 单独计 |
| `eventfd_writes` / `eventfd_reads` | 真正敲了多少次 eventfd；忙时应趋近 0。**已实现**为环控制块里的 `producer_stats.wakeups_sent` / `consumer_stats.wakeups_received`；压测实测：64 条在途全速时每条约 0.001~0.003 次，单条在途约 1 次（§10「实测」） |
| `spurious_wakeups` / `empty_wakeups` | **已实现**（`consumer_stats`）：两种白醒——eventfd 本来就空；读到了计数、但第一次 `Peek` 就是空的（旧计数：生产者在消费者睡前回头一看的窗口里敲了 eventfd，记录已被收走）。实测很少（§5） |
| `spin_hits` / `sleeps` | 睡前自旋期间来了新消息的次数 / 真正去睡的次数（自旋是调用方的可选策略，没做自旋就只有 `sleeps`） |
| `recheck_rescues` | 消费者"回头再看一眼"发现有新写入、取消睡眠的次数（Dekker 的第二道保险有没有真的起作用） |
| `queue_delay_ns` 直方图 | 用 `enqueue_ts_ns`（每条都有，消费者按采样记直方图）量**排队时间**（对应 Tbuspp2 的 step1 / step3 两段） |
| `validation_failures` | §7 的校验失败次数，按原因分 |
| `attach` / `detach` / `resume` / `destroy_on_expire` | 通道生命周期事件 |
| `agent_reattach` / `reattach_rejects` | Agent 重启后 App 传回 fd 的次数 / 被拒绝的次数（按校验项分） |
| `agent_down_seconds` | App 一侧：Agent 缺席了多久 |
| `stalled` / `rx_stall_seconds` | 看门狗（§6.4）：是否判为卡死 / Rx 环没有被消费的持续秒数 |
| `stale_drops` | 因过期（§4）在入队前或出队时丢掉的数量；**由解析信封的一层记**（Agent 的路由、App 的 RPC 层），不是通道库的指标 |
| `invalid_expire` | `remaining_ms == 0` 或 `enqueue_ts_ns == 0`（发送方没填）被丢掉的数量，应当恒为 0；同样由解析信封的一层记 |
| `handshake_rejects` | 按原因分（重复挂载、配额、版本、身份、容量） |
| Agent 级：`channels`、`shm_bytes` | 通道数与共享内存总量 |

**诊断工具**：memfd 是匿名的，外部工具没法按路径打开，所以只读诊断要经 Agent 的管理接口，或者从 `/proc/<pid>/fd` 里拿 fd 只读映射。不照搬 Tbuspp2 的 `dump_mq` 靠路径的做法。

## 10. 性能目标与手段

**以下是目标；其中一部分有第一版实测（本节后面的「实测」），其余没测过。** 基线是我们现有的数字：UDS 往返 p50 约 27µs（直接处理）/ 54µs（经主循环），单条消息 IO 线程成本 4–5µs（回环）。

**目标（待基准修正）**：

| 场景 | 初始目标 |
|---|---|
| 同机、消费者忙等、64 B | 单向 p50 ≤ 2 µs |
| 同机、消费者在睡、需要唤醒 | 单向 p50 ≤ 15 µs（受 eventfd 唤醒和调度限制） |
| 单核吞吐、忙时、64 B | ≥ 500 万 msg/s |
| 忙时系统调用 | 每条消息 ≈ 0 次 |

**手段**，按对 Tbuspp2 的差异来写（Tbuspp2 部分来自笔记，**其内部实现笔记里没写的我不猜**）：

| 手段 | 说明 | 对比 Tbuspp2（笔记） |
|---|---|---|
| 唤醒抑制（睡眠标志） | §5，忙时 0 系统调用；睡前自旋是可选的，默认关 | 笔记没写它的唤醒机制 |
| 批量发布与批量释放 | §4，摊薄原子操作 | — |
| 少一级队列 | 目标：OutQ 里的记录经路由后**直接**写目标环或目标连接的写路径，不再先进一个 Agent 内部的中转队列 | 它是 OutQ → TxQ → 网络两级（拷贝次数笔记没说，**推断**有一次入 TxQ） |
| 从环里直接 `send` | 消费者 `Peek` 到记录后直接对目标连接 `send(span)`，写不完的部分拷进该连接的待发缓冲，**然后立刻 `Pop`** | — |
| 镜像映射 | 变长记录跨尾部也连续，不拼接 | — |
| memfd 而非 `/dev/shm` 文件 | 没有路径清理与空间不足（它的错误码里有 1007），页记在 cgroup 里 | 用 `/dev/shm/<busid>/q_N.dat` |
| 缓存行布局 | 游标、睡眠标志、统计各占独立缓存行；**记录起点按 64 字节对齐**，相邻记录不共享缓存行（§4） | — |
| 多线程 Agent | 通道按 App 绑定到固定的 loop，路由表用快照指针无锁读 | 它是多个 NetWorker 线程 |
| 可选大页 | `MFD_HUGETLB` 减少 TLB 缺失，要预留大页，作为后续优化 | — |

**实测：Agent 的连接 vs 普通 UDS 连接（2026-11-01，第一版）**

- **怎么测**：`bench/agent_bench.cpp` + `bench/run_agent.sh`（`cmake --preset release -DBREEZE_BUILD_AGENT_BENCH=ON`）。四种连接，同一个驱动、同一种消息（含 64 字节信封，总长 64 / 1024 / 16384），闭环 ping-pong，每条都是"驱动 → 服务端 → 驱动"：
  `uds` 普通 UDS 连接（服务端只 echo）；`shm` UDS 握手 + 共享内存数据通道（服务端也只 echo，**这一对才是纯传输对比**）；`agent-shm` 完整的 Agent 通路（服务端是路由器，驱动给自己发信封）；`agent-uds` 同上但协商成仅 UDS。
  服务端 cpu2、驱动 cpu6（各占一个物理核），回调都在 IO 线程上（不经主循环）；每组预热 1 秒、计时 3 秒，5 轮交错取中位数。环境：i9-14900KF（P 核）、**WSL2**（不是裸机 Linux）。
- **单条在途（延迟）**：p50 往返 `shm` 22.8 µs，`uds` 25.3 µs（64 B；各轮范围 22.9~23.2 对 25.3~26.4，不重叠），只快约 10%，吞吐同比高约 10%，大小对结果几乎没影响。
  原因：每条之间两边都睡进 epoll、靠 eventfd 唤醒，唤醒延迟占大头，共享内存省掉的只是 socket 拷贝与系统调用。**设计里"睡前自旋"没有实现**（库里没有，只有多进程测试里的 `WaitPeek`），所以 §10 的"忙等 p50 ≤ 2 µs"在仓库的代码里达不到；加了自旋的实验结果见下。按往返的一半估单向约 11 µs，在"消费者在睡 ≤ 15 µs"的目标内，但没有单独测单向。
- **64 条在途（吞吐）**：往返 / 秒 `shm` 对 `uds`：64 B 797 万对 102 万（7.8×），1 KiB 705 万对 91 万（7.7×），16 KiB 93 万对 30 万（3.1×，受拷贝带宽限制）。
  服务端每次往返的 CPU 时间：64 B 0.10 对 1.04 µs（10×），1 KiB 0.15 对 1.18 µs（8×），16 KiB 1.18 对 3.19 µs（2.7×）。**64 B 与 1 KiB 的 `shm` 数字受驱动限制**（驱动 CPU ≥ 100%、服务端 82%~104%），是通道能力的下限。该档延迟只是排队，不是连接延迟。
- **定速（单条在途，每秒 5000 / 20000 次往返，1 KiB）**：服务端 CPU `shm` 7.3% / 23.6%，`uds` 8.0% / 26.6%（20000 档各轮范围 23.6~24.0 对 26.3~27.3，不重叠），低约 10%；p50 低约 2 µs。**低负载下共享内存的优势很小。**
- **路由器的开销**（`agent-shm` 对 `shm`）：单条在途 p50 多 0.2~0.4 µs；1 KiB 饱和吞吐低 12%（6.22M 对 7.05M，各轮范围不重叠），16 KiB 低 4%；64 B 两者都被驱动限制，`agent-shm` 反而更高，**不能读成路由器更快**。`agent-uds` 与 `uds` 几乎一样（差 3% 以内）：UDS 的系统调用盖过了路由器。
- **睡前自旋实验**（在临时副本里给两端的"读空之后、`PrepareToSleep` 之前"加了可配窗口的自旋，**没有进仓库**；`shm` 连接、1 KiB、3 轮中位数）：
  - **单条在途、闭环全速**：p50 往返 23.7 → **0.3 µs**，吞吐 3.7 万 → 250 万往返/秒（67×），p99.9 269 → 5.9 µs；窗口 20 µs 就够。代价是两端各烧满一个核（CPU 46% → 110%）。
  - **定速时窗口必须大于消息间隔才起作用**：每秒 2 万次（间隔 50 µs）窗口 100 µs 才到 p50 0.4 µs，20 µs 只到 12.2 µs（只有驱动一侧在窗口内收到）；每秒 5 千次（间隔 200 µs）窗口 300 µs 才到 0.6 µs，100 µs 只有 14.4 µs。
    服务端 CPU：5 千次档 8% → 111%，2 万次档 25% → 110%。窗口小于间隔时，白烧一部分 CPU、延迟只好一半（5 千次、20 µs 窗口：CPU 8% → 15%，p50 28.6 → 15.0 µs）。
  - **64 条在途**：吞吐 +15%（629 万 → 721 万往返/秒），p99 41.5 → 21.5 µs；CPU 本来就满。
  - **睡眠统计**：窗口够大时每条敲 eventfd 从约 1 次降到 0.0001~0.003，生产者一侧的系统调用也省了；空唤醒占醒来的比例 0~2.3%，自旋没有引入新的问题。
  - **条件与局限**：两个线程各绑一个 P 核、同一缓存域、WSL2；自旋是塞在回调里的（会阻塞整个 loop，管不了别的 fd 与定时器），真要做必须是 loop 级的忙轮询；代价按"每个自旋的 IO 线程占满一个核"算，容器的 CPU 限额会把它算进去而限流；
    只有共享内存受益，UDS 每条都要系统调用。在这种条件下 §10 的"忙等 p50 ≤ 2 µs"目标可达（往返 0.3 µs）。**要不要做**取决于有没有个位数微秒的延迟要求，见 §13。
- **对照目标**：单核吞吐 ≥ 500 万 msg/s：64 B 时服务端在 82% CPU 下每秒一读一写约 800 万次，达到。"忙时每条 ≈ 0 次系统调用"没测。
- **没覆盖的**：多 App 并发与路由器扇入（路由器是单线程）、满环 / 慢消费者下的行为、经 `MainLoopExecutor` 回投主循环（对两种连接同样多一跳，基线是 27 → 54 µs）、裸机 Linux、ARM、tsan。

**信封（已实现，`base/net/agent/envelope.h`，库 `breeze_net_agent`）**：仓库里原来没有信封，这是按下面的调研结论定的，**字段布局要和仓库外的信封维护者对一遍**（§13）。64 字节，小端，前 48 字节是路由区：

| 偏移 | 字段 | 说明 |
|---|---|---|
| 0 | `magic` u32 | 字节序列 `BENV` |
| 4 / 5 / 6 | `version` u8 / `type` u8 / `flags` u16 | `type`：1 Request、2 Response、3 Oneway（单向显式，不靠 `seq == 0`）；`flags` 目前只有"追踪"位，不认识的位一律丢 |
| 8 | `body_len` u32 | 头之后的载荷字节数，**必须恰好等于 总长 - 64** |
| 12 | `msg_id` u32 | 业务消息号 |
| 16 / 20 | `src` / `dst` u32 | NodeId（库里当不透明的 u32）；Direct 与 Response 的 `dst` 必填 |
| 24 / 26 / 27 / 28 | `service_type` u16 / `route_type` u8 / `hops` u8 / `gray_tag` u8 | `route_type`：1 Direct、2 Any、3 Hash、4 Broadcast；`hops` 每转发一跳加一，到 8 丢弃（防环） |
| 29 | 保留 3 字节 | 必须为 0 |
| 32 | `route_key` u64 | Hash 路由用的**数值**（实体 id 或发送方算好的哈希），不是字符串 |
| 40 | `remaining_ms` u32 | 发送那一刻的相对剩余毫秒，0 无效（§4）。**在路由区里**，Agent 出队时要读、转发时要改 |
| 44 | `err_code` u32 | 只有 Response 用 |
| 48 | `seq` u64 | 相关性：单调递增、永不复用、命名空间是发送方；**Agent 不改写它** |
| 56 | `trace_id` u64 | 只在回包与追踪时用 |

校验（`ParseEnvelope`，先把 64 字节拷到本地再校验）：总长不足 64、魔数、版本、类型、`route_type`、Direct 缺 `dst`、Response 不是 Direct、不认识的 `flags`、保留位、`body_len` 对不上，**任何一项不过就丢弃并按原因计数**，不退化成别的路径。
`remaining_ms` / `hops` / `src` 的合法性不在这里判：过期由 `CheckExpire` 判，`hops` 由 `PatchForForward` 判，`src` 由路由器核对发送方的身份。
`CheckExpire`：剩余 = `remaining_ms × 10^6 - (now - 入队时刻)`，**对剩余向下取整到毫秒，不足 1 毫秒也算过期**（写进拷贝的 `remaining_ms` 不能是 0，宁可早丢）；`remaining_ms` 或入队时刻为 0 是 `kInvalid`，单独计数；
不是从环里来的消息（UDS / TCP 的帧）没有入队时刻，调用方传"收到的时刻"，排队时间记 0。

**调研依据**（2026-11-01，子代理在 `D:\GameserverNew`、`D:\silver-server`、brpc、grpc-go、thrift、TarsCpp 里按关键词定位的，引用的行号没有逐条核对）：
- **几乎所有项目的坑都是"路由字段在变长区"**：brpc 的服务名 / 方法名 / `timeout_ms` 在 protobuf meta 里，Tars 在 tars 编码里（`iTimeout` 还在载荷之后），gRPC 在 HPACK 头里（超时是十进制文本加单位），自有的 GameserverNew 把 `RoutingKey` 和方法名放在定长区之后，
  所以转发节点读不全，也没法原地改写超时（改写会让变长整数 / 文本的宽度变化，要连带改长度）。本信封路由区全是定长数字。
- **相对剩余毫秒是共识**（gRPC、brpc、Tars、GameserverNew 都是），但**没有一个在转发一跳时扣排队时间**（gRPC 只在发送时用 `time.Until(deadline)` 重算；Tars 只在服务端按队列等待超时丢；GameserverNew 的 Agent 不改 `DeadlineMs`；silver-server 干脆不带）。
  我们有记录头里的入队时刻，所以能做到。
- **魔数 + 版本 + 头长度**（Thrift 的 THeader、brpc 的 `PRPC`、silver 的 natsagent `NATSAGT`）；GameserverNew 没有，解码失败就退回旧路径转发，是反例。
- **单向用显式类型**（Thrift `ONEWAY`、Tars `cPacketType`），不靠 `seq == 0`（GameserverNew、silver-server 都靠约定，silver 回包时看到 0 才丢）。
- **Agent 改写 seq 要维护映射表**（GameserverNew 的 `remoteSeq`，疑似不清理）：本设计不改 seq，回包按 `(src, seq)` 索引。
- **路由键用数值**：GameserverNew 的 Direct 用字符串带 nodeID 还要 `ParseUint`，一致性哈希 key 也是字符串。
- **发送方身份**：GameserverNew 没有核对 `SrcNodeID` 与握手时的 node 一致；本设计由路由器核对（本机 App 的 `src` 必须等于握手时登记的 NodeId，否则可以冒充别人）。
- **校验要有下界**：silver-server 旧共享内存队列没有 `length < sizeof(header)` 的检查，相减下溢，一条坏消息堵死队列；GameserverNew 的 UDS 上限 1MB 与 peer 的 16MB 不一致。

**Agent 的路由器（已实现，`base/net/agent/agent_router.h`）**：同一个 `AgentRouter` 既是 `ShmChannelServer` 的 `ShmChannelHandler`（共享内存通道），又是 `ShmUdsDataPath` 与 Agent 之间链路的 `IConnectionHandler`。
每条消息的流程：`ParseEnvelope`（先拷头再校验）→ **本机 App 的 `src` 必须等于握手时登记的 NodeId**（否则丢并计 `bad_src`；来自别的 Agent 的链路是受信的，不核对）→ `CheckExpire`（出队时用记录头的入队时刻扣排队时间；
UDS / 链路的帧没有入队时刻，排队时间记 0）→ `PatchForForward`（拷贝里改 `remaining_ms`、`hops` 加一，到 8 丢弃防环）→ 路由表 `Resolve` → 投递：
- **本机共享内存的 App**：直接 `TryReserve` 它的 Rx 环、改写过的头与载荷各拷一次、`Commit`，不经中转队列；App 在保留中（Detached）照样写，满了丢并计 `rx_full_detached`；
- **本机仅 UDS 的 App**：`IConnection::Send`；**别的 Agent**：`ConnectionHandle::Send`（路由表的 `RemoteOf(node)` 给出链路句柄；同一个 Node 既在本机又登记成远端时，本机优先）。
- **失败的处理**：Oneway / Response 无路可走、过期、环满：丢并计数，不通知发送方（单向可丢，已定）；**Request 送不到时回一条错误 Response**（`kRouteErrNoRoute = 1` / `kRouteErrUnavailable = 2`，带回 `seq` / `msg_id` / `trace_id`，`dst` 是请求的 `src`），让调用方快速失败而不是等满 deadline；过期的 Request **不回**（请求方早已判超时）。
- **释放策略**：共享内存通道上"先 `MarkInflight`、转发完再 `Pop`"，Agent 崩溃后新 Agent 靠标记跳过处理到一半的那一条（`TestInflightMarkedWhileForwarding` 在路由表的 `Resolve` 里卡住路由器，从 App 一侧看控制块，证明标记确实立着）。
- **路由表**（`RouteTable`，真实来源是控制面，仓库里还没有）：接口加一个内存实现 `StaticRouteTable`：Direct 发给 `dst`；Any 轮询；Hash 对"按 NodeId 排序的实例列表"取模（**不是一致性哈希**，实例增减时大部分键会换去别的实例）；Broadcast 发给全部实例（不含发送方）。
  通道名到 NodeId 的映射由 `AgentRouterOptions::node_of` 给（默认：通道名就是十进制的 NodeId）；不认识的 `Reject(EACCES)`，两个通道名映射到同一个 Node `Reject(EBUSY)`。
- **单线程**：路由器、路由表、对端表都只在一个 loop 线程上访问，所以 Agent 进程的 `EventLoopGroup` 必须 `io_threads = 1`（`AgentServer::Start` 在 group 有多个 loop 时返回 `EINVAL`；`NetModule::AddAgentServer` 在 `net.io_threads != 1` 时报错）。
  Agent 之间的链路用 `AgentServer::router()` 当 `IConnectionHandler`（`NetModule::AddIoServer` / `AddIoClient`，回调直接在 IO 线程上跑，不回投主循环），再把"远端 Node 经哪条链路"登记进路由表。

**关于"零拷贝转发"的一个更正**：我之前提过 `SharedBuffer` 需要能包装外部内存才能零拷贝。细想后不需要：从环里直接 `send(span)`，写不完的部分拷进连接缓冲，环里的槽位立刻释放，就**不需要延后释放，也就不会让最慢的目的连接拖住整条环**。`SharedBuffer` 的外部内存只在"同一条消息要异步多次发送且来不及立刻写完"时才有用，不是第一阶段的前提。
接收方向（TCP → App 的 Rx 环）目前仍是一次拷贝（读到本线程缓冲，再拷进环）；"`recv` 直接读进环"是后续优化，要先知道帧长，复杂度较高。

**怎么验证"比 Tbuspp2 更好"**：我们没法在同一套环境里跑 Tbuspp2，只能拿它文档里的数字（128 B、P2P、约 116 万 TPS 发送、平均时延约 1.1ms，含跨机）作参考，**硬件、跨机与否、测量口径都不同，不能直接比大小**。所以第一阶段只和自己比：和现有 UDS 路径比、和不同参数（自旋、批量、容量、对齐）比，并把环境、参数、原始数据一起记下来（沿用 `bench/` 的方法：绑核、多轮、配对）。

## 11. 代码布局与集成（提案）

**实现状态（2026-10-31）**：`src/cpp/base/shm/` 已有——`shm_defs`（线格式、布局计算）、`shm_memfd`（memfd 创建与封印、eventfd）、
`shm_mirror_map`（镜像映射）、`shm_ring`（`RingProducer` / `RingConsumer`：批量发布与释放、`inflight` 标记与崩溃接手、睡眠协议）、
`shm_channel`（`ShmChannel::Create` / `Attach`，含对端 fd 的全套校验）；CMake 目标 `breeze_shm`，只在 Linux 构建。
`src/cpp/base/net/` 里新增：`stream/internal/fd_passing`（UDS 上 `SCM_RIGHTS` 传 fd 的通用封装，在 `breeze_net` 里）、
`shm/shm_protocol`（握手线格式）、`shm/shm_channel_server`（`ShmChannelServer`：Agent 一侧，握手 + 准入 + 配额 + 探活 + 替调用方跑 Tx 环的唤醒协议）、
`shm/shm_channel_client`（`ShmChannelClient`：App 一侧，阻塞的 `Connect` + `liveness_fd()` / `AgentGone()`）；CMake 目标 `breeze_net_shm`（单独一个库，照 rudp 的先例）。
**范围**：`Attach`（新建 / 保留期内恢复 / `discard_retained` 重建）、`Reattach`、保留计时器与 `Detached`、控制消息 `PrepareRestart` / `Bye`、`Superseded()` 都已实现（§6.2 / §6.5 / §6.6）；
`ShmChannelServer` 的全部通道、握手与回调都在 accept 所在的那一个 loop 上（容量上限是一个 IO 线程）。

**新增（`src/cpp/base/net/agent/`，库 `breeze_net_agent`，依赖 `breeze_net_shm`）**：
- `envelope`：信封（§10）、`ParseEnvelope` / `CheckExpire` / `PatchForForward`；`route_table`：`RouteTable` 与 `StaticRouteTable`；`agent_router`：`AgentRouter`（§10）；
- `agent_server`：`AgentServer`（`INetServer`）= `AgentRouter` + `ShmChannelServer` + 仅 UDS 的数据通道，`NetModule` 经它持有；
- `shm_connection`（内部）：`ShmConnection : IConnection`——共享内存通道做成进连接表的 `IConnection`：Tx 环的生产者与 Rx 环的消费者都在所属 IO 线程上，业务线程发送经 `ConnectionHandle` 投递到那个线程
  （发送方预扣的 `pending_bytes` 写完环就还回去）；PauseRead / ResumeRead 对应暂停 / 恢复读 Rx 环（消息留在环里不丢）；Agent 不在时 Send 照常写 Tx 环，满了丢这一条并计 `tx_full_drops`、**返回 `SendResult::Full`**（IO 线程上直接 `Send` 当场如此；业务线程经 `ConnectionHandle` 发时写环是异步的，靠槽上的 `tx_full` 提示：环满之后 `kFullHintWindow`（2 ms）之内的发送当场得到 `Full`、不入队，计 `NetStats::send_full`，到点由 IO 线程上的一次性定时器清掉，清掉之后的第一条才真的去试）。**`Full` 保证没发出；`Ok` 不保证发出了**——提示置位之前已投递的那几条仍可能被丢，只进 `tx_full_drops`；
- `agent_client`：`AgentClient`（`INetClient`）——App 一侧的**非阻塞**客户端：握手 → 按协商结果建 `ShmConnection` 或 `StreamConnection`；共享内存模式下 UDS 断开（Agent 掉线）**连接不关**，只置 `agent_down()`，后台按退避 `Reattach`；
  Reattach 被判定不可能成功（校验不过、被别的进程接管）才关闭这条连接再走全新的 Attach；**共享内存里的环被对端写坏（读写环时校验不过）、或接手 Agent 保留着的内存时校验不过，下一次 Attach 自动带 `discard_retained`**
  （Agent 保留着的是同一块坏内存，再"恢复"它只会再坏一次；不这样做会因为 `EINVAL` 不可重试而卡在 Idle——这是写 `TestCorruptTxRingClosesConnection` 时发现的）；仅 UDS 模式 UDS 断了就是关闭，按退避重新握手；`PrepareRestart(ms)` 之后的 `Stop()` / 析构当作重启，不发 `Bye`；析构同步停掉自己并等连接回收完。
- `framework/net`：`NetModule::AddAgentClient`（端点 / 是否启用共享内存 / 环容量取配置 `net.agent_*`，业务回调回投主循环）、`AddAgentServer`、`AddIoServer` / `AddIoClient`（回调直接在 IO 线程）。配置：`net.proto` 的 `agent_endpoint`、`agent_shm_enabled`、`agent_shm_ring_mb`。

**还没有**：`framework/app` 的接入（自检线程 §6.4、`Probe` / `Abort` §6.6）、控制面（路由表的真实来源、Node 的登记、Agent 之间链路的自动建立）、多 loop 的 Agent、按 uid 的配额。
include 在 main 分支上目前是与同目录其它库一致的扁平写法；合并 `refactor/net-layout-and-includes` 之后要按那边的规则改成从 `src/cpp` 起算。
`UniqueFd` 在 `base/net/internal` 里，`base/shm` 不能依赖它，所以有一个重复的 `shm::Fd`，等 `UniqueFd` 上提到 `base` 之后合并。

- **`base/shm/`（新）**：纯数据结构，**不依赖 `EventLoop`**——memfd 封装（含封印）、镜像映射、单向 SPSC 环、通道头与控制块、唤醒协议里的睡眠标志与 eventfd 的收发。eventfd 的 fd 暴露出来，谁的事件循环都能挂。
- **UDS 传 fd**（已实现）：`sendmsg` / `recvmsg` + `SCM_RIGHTS` 的小封装。`IConnection` 带不了 fd，所以握手用 `Acceptor` 接出裸 fd，自己管会话，不走 `IConnection`。
- **`ShmChannelServer` / `ShmChannelClient`（已实现，放在 `base/net/shm/`，库 `breeze_net_shm`）**：握手、传输协商、通道状态机、保留计时器、控制消息（§6.6）、接入 `EventLoop`。因为依赖 `EventLoop` 与 UDS，更靠近 `base/net`。不起额外线程（§6.6 的控制线程只有做 `Probe` / `Abort` 时才需要）。
- **分层**：遵守 `tools/check_includes.py` 的规则（`base/` 不能依赖 `framework/`）；include 从 `src/cpp` 起算。
- **与 `framework/app` / `framework/net` 的关系**：**已经接进 `NetModule`**（`AddAgentClient` / `AddAgentServer`）：App 侧 Rx 的 eventfd 挂在 `NetModule` 的 IO 线程上，消息经 `MainLoopExecutor` 回投主循环，业务看到的是一条普通的 `IConnection`（§11）；
  没有走"让主循环线程直接当 Tx 环的生产者 / Rx 环的消费者"的路——那样 App → Agent 能少一次线程切换，但要把共享内存的唤醒挂进主循环、业务线程之外的发送也得汇到主循环，改动面大，等有基准证明值得再做。`framework/app` 的自检线程（§6.4）仍然没有。

## 12. 测试与验证计划

**已实现并通过的（2026-11-01，debug / asan+ubsan / release 三种配置；`shm_*`、`fd_passing_test`、`envelope_test`、`agent_*`、`net_module_agent_test` 共十三个目标）**：

| 测试 | 覆盖 |
|---|---|
| `shm_layout_test` | 记录大小、容量与载荷上限规则、布局偏移、结构体对齐、memfd 封印与不接受的 fd、eventfd 校验（含 /proc 不可用）、镜像映射、映射失败不泄漏地址空间 |
| `shm_ring_test` | 空 / 满 / 恰好满、批量发布与释放、跨镜像边界（100 圈）、游标绕过 2^64（含接手与跳过）、唤醒协议各分支（含旧计数造成的空唤醒 `empty_wakeups`）、两个双线程压力、锤子测试、**litmus 测试**（见下） |
| `shm_untrusted_test` | 篡改记录头 / 游标 / 通道头 / fd 的 30 多种用例、崩溃接手的各分支、对端运行时接手的回归、3 万次随机翻字节的模糊测试 |
| `shm_multiprocess_test` | 跨进程收发（用 `Attach` 重新映射）、两个方向的乒乓（含每条都睡）、`kill -9` 消费者 400+ 次（至多一次、跳过）、`kill -9` 生产者 200+ 次（批内原子）、接手、SIGBUS 对照 |
| `fd_passing_test` | 传递与 `CLOEXEC`、对端塞太多 fd / 不期望带 fd 时不泄漏、EOF / `EAGAIN` / 参数错误、信号打断下的补发、补发时对端关闭返回 `EPIPE` 而不是被 `SIGPIPE` 杀死 |
| `shm_protocol_test` | busid 规则、Hello 各字段的校验（含 Reattach 的限制）、`Control` 的校验、Reply 头的校验、结构尺寸 |
| `shm_channel_test`（服务端按 `retain_default = 0` 跑：UDS 断开就排空并销毁） | 握手 + 回显（含默认容量）、跨睡眠的唤醒（共享内存里的计数证明 eventfd 起了作用）、预算用完要让出且不丢不卡、多客户端并发、同名 `EBUSY` 与关闭后复用、各种配额 / 准入 / 容量拒绝、协议滥用（错版本 / 错魔数 / 全零 / 附带一堆 fd / 握手后写数据 / 半条或拆成两段的 Hello）、握手超时与待握手上限、服务端停止、损坏的环、**假 Agent**（客户端面对错误回复、fd 数量与种类不对、布局对不上、分多段到、不回复 / 半条回复）、**跨进程的真正 `SCM_RIGHTS` 与 `kill -9`**（回显、被杀后同名顶替） |

| `shm_retention_test` | **App 重启**：遗言在保留期里被排空、Agent 在保留期里往 Rx 写、新进程恢复后先读到积压（顺序不乱不少）、`app_epoch` 加一、事件序列 `Opened Detached Resumed`；保留到期（不早于保留时长、`rx_bytes_discarded` 精确）；恢复后旧计时器作废；预算只有 3 时 300 条遗言一条不丢；**控制消息**（`PrepareRestart` 延长 / 超上限按上限 / 只管下一次，`Bye` 不保留，不合法的控制消息、第一个字节就不对、分段到、半条就走）；**恢复的规矩**（活着的同名通道 `EBUSY` 与重试窗口、容量对不上、别的用户 `EACCES`、`discard_retained` 的事件顺序与失败时旧队列不毁、连续 200 次快速重启不见 `EBUSY`）；停机销毁保留中的通道；业务主动 `Close` 保留中的通道；**Agent 重启**（Agent 不在期间 App 照写、旧 Agent 死在处理一条记录的期间则跳过那一条、睡眠标志停在 1 也能恢复、Reattach 命中还留着的通道、`Superseded` / `ESTALE`）；**Reattach 的恶意输入**（不是 memfd、没封印、eventfd 位置放了管道、魔数被改、游标越界、容量超限、fd 数量 0 / 1 / 2 / 4、通道名被占、配额、准入拒绝，之后规矩的 Reattach 仍然成功且在这块内存上收发正常）；**假 Agent 对 Reattach 回错的东西**；**跨进程**：`kill -9` App 后新进程恢复读到 100 条回显、`kill -9` Agent 五次后 Reattach（在 24000 条里至多丢"被杀那一刻处理到一半的那一条"，不重复、不乱序） |

| `shm_transport_test` | **传输协商**：`UdsOnly` / `ShmPreferred` / `ShmRequired` 对 Agent 的各种配置（提供 / 不提供共享内存、有 / 没有仅 UDS 的数据通道、总量配额满、通道数配额满、容量不合法、准入被拒）各得到什么（只有"可用性"类失败才退化）；仅 UDS 的数据通道（200 条收发、大帧、空帧、超长帧头关连接、对端关闭、身份 `UdsPeerContext`）；通道名与配额在两种传输之间统一（含保留中的共享内存通道要换仅 UDS 必须带 `discard_retained`、别的用户 `EACCES`）；仅 UDS 的 Hello 带 fd；Hello 与第一帧数据同一次 `write` 到达；停机关掉仅 UDS 的连接；**服务端先于连接销毁、处理器随即释放**（`~ShmChannelServer` 之后不再有回调） |
| `envelope_test` | 布局与每个字段的偏移、`ParseEnvelope` 的每一种拒绝（含边界：63 / 64 字节、`body_len` 差一、极值）、先拷头、`CheckExpire` 的边界（刚好用完、差 1 纳秒、不足 1 毫秒的零头、时钟回拨、`u32` 极值不溢出）、`PatchForForward`、20 万次随机翻字节的模糊测试 |
| `agent_router_test` | 转发矩阵（共享内存 / 仅 UDS 两两互转，头里 `src` / `dst` / `seq` / `msg_id` / `trace_id` 都不改，`hops` 加一，`remaining_ms` 扣了排队时间）、拨快时钟模拟排队（过期 / 无效 / 差不多用完、过期的 Request 不回包）、坏信封与冒充 `src`、无路由与错误回包（含服务存在但实例列表为空）、目的地满 / 单条超限 / 保留中 / 已断开、`hops`、Any / Hash / Broadcast、`node_of` 映射、**转发期间 `inflight` 标记立着**、本机优先于链路、discard 重建后仍登记在名下、两个发送方各 2 万条的顺序与数量、**两个 Agent 之间经 TCP 链路转发**（含回包、跨链路的错误回包、链路上的过期） |
| `agent_client_test` | 两种传输下的收发与 `Stop`（`Bye`）；协商结果（退化、`ENOTSUP` / `ENOSPC` / `EACCES` / `EINVAL` 各自可不可重试）；**Agent 重启时共享内存连接不关**（后台 `Reattach`，期间写 Tx 的由新 Agent 处理，两轮）、仅 UDS 的自动重连；**App 热更重启**（`PrepareRestart` 后析构不发 `Bye`、保留期里 Agent 写的积压一连上就交给业务）；暂停 / 恢复读（含在回调里暂停）；Agent 还没起来的退避重试、名字被占的重试、`Stop` 取消重试；**Reattach 被判不可能成功时关闭连接重新 Attach**；`Tx` 环满的丢弃与计数；**`Full` 的返回**（IO 线程直发当场 `Full`、经句柄 / `AgentClient` 在提示期间当场 `Full` 且不入队不占 `pending_bytes`、Agent 恢复消费并过了提示窗口后又能发，`TestTxFullReturnsFullAndRecovers`）；发送计账不泄漏 `pending_bytes`；`discard_retained`；9 种恶意 Agent 回复；并发发送（4 线程 × 3000）；**客户端先于连接销毁、处理器随即释放** |
| `net_module_agent_test` | 真实的 `App` + `NetModule`：一个 Agent（`AddAgentServer`）加两个 App（`AddAgentClient`），一个走共享内存、一个因配置关掉共享内存只用 UDS，互发请求与回包（20 条，保序）；业务回调都在各自 App 的主循环线程上；配置 `net.agent_*` 到选项的映射；端点 / 通道名 / `io_threads` 不对时 `App::Run` 带着原因返回 |

**握手相关代码的变异验证**：把 `fd_passing` / `shm_protocol` / `shm_channel_server` / `shm_channel_client` 里 107 处关键判断逐个改坏，release 下 **98 个被杀死**；
存活 9 个里，`BeginClose` 在回调栈里销毁自己（use-after-free）与 `eventfd` 不摘除这两个在 **asan 下被抓到**（所以这套测试必须也在 asan 下跑），
fd 槽位越界写同样只有 asan 能抓；其余存活的是冗余防线（摘 fd / 忽略已关闭会话的事件 / 取消定时器 / busid 映射按 id 校验，任何一个单独去掉都无害）和造不出来的错误路径
（对有效 fd 的 `getsockopt(SO_PEERCRED)` 与 `epoll_ctl` 失败）。

**保留 / 恢复 / Reattach 的变异验证**（`/tmp` 副本里逐个改坏，release）：83 个，**首轮杀死 68 个**；存活的 15 个逐个分析后补了用例（探测旧连接、Reattach 的 uid、`inflight` 标记本身不合法、`ECONNRESET` 当 IO 错误、discard 后名字登记、discard 的配额抵扣、分两段累计的 fd、排空超时、`Superseded`、Reattach 之后换连接……），**补完杀死 83 个中的 81 个**，剩下两个：
`SV38`（Reattach 清 `consumer_sleeping`，冗余：接手后的第一轮消费循环无条件扫一遍）与 `SV46`（析构里的栅栏，竞态，不稳定地存活，asan 满载下抓到过它防的那个 UAF）。**这轮变异验证挖出的真问题**：测试夹具里处理器比 group 先销毁（见下面的坑）。

**协商 / 信封 / 路由器的变异验证**：80 个，首轮杀死 73 个，补用例后又杀 4 个（`Any` 的实例列表为空、转发期间 `inflight` 标记、本机优先于链路、仅 UDS 通道名重名）；剩下 3 个：`EN20` 与 `AR17` 是**等价变异**（`age >= budget` 与后面的 `left == 0` 判同一件事；路由器经 UDS 与 Agent 间链路发送，这两种连接不会返回 `Full`，所以 `!= Closed` 与 `== Ok` 在这里一样——`SendResult` 有了 `Full` 之后，将来链路换成共享内存就不再等价，到时要补用例），
`TR22`（`~ShmChannelServer` 等仅 UDS 的连接回收完）**asan 下也存活**：析构里的第二个 `CallSync` 栅栏在实践中已经间接等到了回收（回收在同一轮事件循环的末尾，栅栏任务要等下一轮才能跑），显式等待只是把"不会再有回调"从靠时序变成保证；用例把 `OnClosed` 里睡 30ms 撑开窗口也测不出差别，属于冗余防线。

**`SendResult::Full` 的变异验证**：13 个，杀死 11 个；存活 2 个都是测不出来而不是没写：`FU04`（提示已置位时仍重复起定时器）只多出若干个到点就无事可做的定时器，最坏是后一轮的提示被前一轮遗留的定时器提前清掉、窗口变短，提示本来就是尽力的；`FU06`（定时器不核对槽的 `generation`）要在 2 ms 内让同一个槽被新连接复用才会清错别人的提示，槽是 FIFO 复用、测试里的 group 又没有可注入的时钟，造不出这个窗口，核对只是廉价的防线（与 `ScheduleDrain` 里的同款写法一致）。

**`empty_wakeups` 的变异验证**：5 个——`WokeUp` 读到计数不置待查标志、空 `Peek` 不计数、计数后不清标志（重复计）、有记录时不清标志、eventfd 本来就空时也置标志——全部被 `shm_ring_test` 的 `TestWakeProtocol` 杀死（`/tmp` 副本里改，release）。

**`AgentClient` / `ShmConnection` / `NetModule` 接入的变异验证**：68 个，首轮（64 个）杀死 47 个；存活的逐个分析补了用例（暂停时的交付、关闭时的槽状态、Tx 环游标被写坏、Reattach 回复的各项校验、握手超时、`AgentServer` 要求单 loop、成功后退避重置、
写坏后丢弃重建……），**最终杀死 63 个**，剩下 5 个：`AC24` / `AC25`（`Stop` 不显式取消握手 / 定时器：过期检查——epoch 比较与状态判断——已经覆盖）、`AC26`（去掉握手回调里的 epoch 比较：状态判断仍然挡得住）、
`AC34`（客户端析构显式等连接回收：与 `TR22` 同理，析构里的栅栏在实践中已经间接等到了）、`AC41`（`Stop` 立刻清当前连接：去掉的最坏后果是旧连接晚到的 `OnClosed` 取消一次进行中的握手、多等一次重试，最终结果相同，测不出差别，但补丁是对的）。
**这轮变异验证挖出的真问题**：①环被对端写坏后 Agent 保留着同一块坏内存，原来的实现会因为接手校验失败（`EINVAL`，不可重试）卡在 Idle——现在检测到写坏、或接手保留着的内存时校验不过，下一次 Attach 自动带 `discard_retained`；
②`Stop()` 之后紧接着 `Start()` 时，旧连接要等回收才 `OnClosed`，`StopOnLoop` 要立刻清掉当前连接的指针。

**`base/shm` 的变异验证**：把库里 98 处关键判断逐个改坏，**96 个被测试杀死**，存活 2 个：
`write_idx` 的发布从 `release` 改 `relaxed`（**x86 的内存模型下看不出来，只能靠 ARM 真机或 TSAN**）、
`S_ISREG` 检查（与封印检查等价的纯防御）。验证过程中发现并修了一个真 bug（§7）、删掉了几处走不到的检查。
**litmus 测试**专门逼 Dekker 的两道栅栏：两个线程用自旋屏障在每一轮里同时执行 `PrepareToSleep` 与 `Commit`，
直接判定"消费者决定睡、生产者没敲 eventfd"。变异验证里去掉任何一道栅栏它都抓到过（之前的压力测试抓不到，纯属碰运气），
但它**仍然是统计性的检测，不是证明**。
**稳定性**：release 下 `shm_*` 重复 12 轮、asan 下 `shm_multiprocess_test` 并行重复 240 次，满载 CPU 下也通过。

**没有验证的**：
- **tsan 没有在本机跑过**（见 [网络库设计.md](网络库设计.md) §11.2）；真机上要补，重点看 `Commit` / `PrepareToSleep` 的两道栅栏、`write_idx` / `read_idx` 的 release / acquire 配对。
- **弱内存序的平台（ARM）没有跑过**：x86 的 TSO 会掩盖 release / acquire 写错的问题（上面存活的变异就是例子）。
- 看门狗（Agent 侧检测 `Stalled`、App 自检线程）、`Probe` / `Abort`（本表之外下面列的那些）**还没有实现，所以也没有测试**。
- **Agent 之间的链路只在同进程里测过**（两个 `EventLoopGroup`、本机回环的 TCP）；真实的跨机、链路断开重连后路由表的更新都没验证。
- **`AgentRouter` 与 `kill -9` 的组合**：路由器上"先标记、转发完再 Pop"只用"在 `Resolve` 里卡住、从 App 一侧看标记"证明了标记立着，没有像 `shm_retention_test` 那样对真实的路由器进程做 `kill -9` 注入。
- 真实部署环境没有验证：Agent 与 App 在不同容器 / Pod 里共享 UDS（K8s 里怎么挂卷、`SO_PEERCRED` 的 pid 在 pid 命名空间里是否可用）、memfd 的页在 cgroup 里怎么计费。
- `ShmChannelServer` 一个服务端只用一个 loop：容量上限没有测过，也没有按通道数压过。
- 性能数字只有 §10「实测」那一版（单连接、WSL2）；乒乓用例打印的往返时间只是顺带，不是结论。

**验证协商 / 路由 / 客户端时踩过的坑**：
- **处理器要比 group 活得久**：连接归 group 的 loop 持有，`~EventLoopGroup` 时还会关连接、回调处理器。测试夹具里把处理器声明在 group 之后，析构时处理器先没了，栈上对象析构后内存仍有效，**asan 看不见，debug 下偶发段错误（约 15%）**。
  库的契约也跟着收紧：`~ShmChannelServer` / `~AgentClient` 返回之后不会再有它的任何回调（等仅 UDS / 共享内存的连接回收完）。
- **`ConnectionHandle::Send` 有自己的背压**：跨线程发送方先在槽上预扣 `pending_bytes`，超过水位就按慢消费者拒绝；测试线程一口气投递 1.6 MB 会被拒（`Closed`），要按 IO 线程已处理的数量分批发。`ShmConnection` 写完环要把预扣还回去（`SendFromTask`），否则累计发送量过水位就被误关。
- **`NetModule::Shutdown` 必须在调 `App::Run` 的线程上调**（队列的消费者角色绑在那个线程上，断言会抓到），测试里让 `Run` 线程在 `Run` 返回之后自己调。
- **回包是新消息**：`hops` 从 0 起（我一开始断言成 2）。
- **等价变异不是用例漏洞**：`EN20`、`AR17` 这类改了写法但行为一样的变异，分析清楚就记下来，不硬凑用例。

**验证保留 / Reattach 时踩过的坑**：
- **析构时残留的任务在 `Impl` 销毁之后才执行（上一版就有，满载下 asan 抓到）**：事件分发阶段 `PostTeardown` 投递的任务可能排在 `~ShmChannelServer` 的同步任务之后，同步任务返回、`Impl` 销毁，这条任务才跑（堆释放后使用）。
  修法：析构里再 `CallSync` 一个空任务当栅栏（队列先进先出，它返回时前面的任务都已执行）。**release 下不会崩，只有 asan 加满载才暴露**。
- **先发回复、后改状态的顺序**：`Resume` 最初先发 `Reply`、再加 `app_epoch`，客户端一收到回复就检查 `Superseded()`，偶发误判被接管（32 核满载下才复现）。epoch 必须在回复之前加；之后发送失败也不回退（计数单调，跳过一个值无害）。
- **kill 的落点有系统性偏差**：测"Agent 被杀"时，触发 kill 的是"App 收到一条回显"，而回显恰恰是 Agent 提交的那一刻，所以 kill 永远落在"提交之后、Pop 之前"，"标记之后、提交之前"（那时会丢这一条）一次都采不到
  （改动前 25 次 kill 丢 0 条）。办法：Agent 处理器在两个窗口里各忙等 30µs 把它们撑大，kill 之前再按轮次错开几十微秒；现在 5 次 kill 全部落在窗口里，丢 2 到 4 条。
- **测试里读事件要等事件**：计数先于回调更新（`detached_total` 加一在 `OnDetached` 之前），测试看到计数就去读事件会读空；应当 `WaitUntil` 事件序列本身。
- **想让别的用户连一下**：Linux 的凭据是线程级的，在新线程里直接发 `SYS_setresuid`（不是 glibc 的 `setresuid`，它会广播给所有线程）就只改这个线程；`SO_PEERCRED` 取的是 `connect` 那一刻的。要 root，否则跳过。UDS 所在目录与 socket 文件要放开权限。
- **时间线类用例的窗口要宽**：保留 2 秒、1 秒窗口的用例在 32 核满载下才暴露出时间线算错（我自己算错了第二次保留的到期时刻，没改动的代码上就失败）。

**验证握手这一层时踩过的坑**：
- **"回到基线"当断言会空等**：用"fd 数回到基线"证明服务端关了连接，客户端一关、服务端还没来得及 `accept` 时计数也是基线，断言第一次检查就通过了——变异验证里它存活才暴露。
  这类断言必须先确认"已经偏离过基线"（等到服务端真的接受了连接）。
- **统计要先于对外可见的效果更新**：`Reject` 原先先发回复、后给 `rejected` 计数，客户端一收到回复就读统计会读到旧值；空闲机器上窗口小，32 核满载时才复现。已改成先计数再发。
- **窗口要卡紧**：测"把数据当断开"时，假 Agent 会在 5 秒后自己关连接，`WaitUntil` 给 10 秒就会被后来的 EOF 碰巧救过；窗口必须小于对端自己放弃的时间。
- **只有 sanitizer 才抓得到的东西要在 sanitizer 下验证变异**：release 下去掉越界检查、在回调栈里销毁对象，测试照样通过。

**写这类测试时踩过的坑**：不要在测试里绑核。线程会继承创建者的亲和性，同一个进程里前一个用例把主线程绑死，
后一个用例新建的锁步自旋线程就落在同一个核上，每次屏障交接都要等调度器抢占，慢到几乎不动（实测 2 万轮 20 秒都跑不完，分核时 0.6µs/轮）。

**以下是整体计划（含尚未实现的部分）**：

- **环的单元测试**：空、满、恰好满、批量发布与释放、记录跨镜像边界、`TooLarge`、游标不回绕（模拟接近 `UINT64_MAX`）。
- **多进程测试**：`fork` 出生产者与消费者经 memfd 通信，核对顺序、不丢、不重。
- **唤醒协议**：大量次数、随机延迟注入的压力测试，专门盯"消费者带着满队列睡死"；这块**必须在真机上跑 TSAN**（本机环境跑不了，见 [网络库设计.md](网络库设计.md) §11.2），并考虑用内存模型检查工具或 litmus 式测试补强。
- **崩溃注入**：`kill -9` 生产者或消费者于任意时刻，核对不会出现半条记录、游标不乱、遗言能被排空；Agent 被杀后，已 `Pop` 的不重发、没 `Pop` 的恢复后继续发。**毒包用例**：往 Tx 环里放一条会让 Agent 的处理路径崩溃的记录，核对新 Agent 通过 `agent_inflight_idx` 跳过它、不陷入崩溃循环，且之后的记录照常转发。
- **保留与恢复**：App 重启前后核对积压消息、`app_epoch` 加一、`resumed` 标志、保留期超时销毁、布局版本不符拒绝、`discard_retained` 重建。
- **Agent 崩溃与 Reattach**：App 活着时任意时刻 `kill -9` Agent，核对 App 不崩、Tx 环可继续写到满、Agent 重启后 Reattach 成功、游标连续、`consumer_sleeping` 被重置、过期消息被丢；再用伪造的 fd（不是 memfd、没有封印、头部损坏、游标越界、容量越过配额）逐项验证都被拒绝；两边同时死则得到全新通道。
- **控制消息与 `Abort`（暂缓，随 §6.6 一起做）**：主循环被人为卡死（死循环、持锁不放），控制线程仍能回 `Probe`，收到 `Abort` 后 `abort()` 留 core；打栈卡住时定时兜底仍能让进程退出；调试器附着时忽略；`abort_on_stall` 关闭时不动作；`SIGSTOP` 的 App 对 `Probe` 无回应，走升级路径。
- **看门狗**：对 App 发 `SIGSTOP`，环有积压时应在 N 秒后判 `Stalled`，环已满时应在 2 秒后判，`SIGCONT` 之后恢复；环为空时不应误报；App 自救线程（`framework/app` 的测试，不在 `base/shm`）在主循环被人为卡住超过 T 秒后应当 `abort()`，并且调试器附着时不触发。
- **容量参数**：Hello 里的容量不是 2 的幂、小于 1 MiB、超过上限、超过配额、恢复时与现有值不一致，都得到对应的 `Reject`，且不会留下半个通道。
- **不可信输入**：直接篡改共享内存里的游标和记录头（fuzz），核对 §7 的每一项校验都会触发 `Corrupt` 而不是崩溃或越界读写。
- **封印**：对端 `ftruncate` 被拒绝；没有封印的对照用例要能复现 `SIGBUS`（证明封印确实必要）。
- **基准**：§10，先做环的微基准（单进程两线程、双进程），再做 App → Agent → TCP 回环的整条路径。

## 13. 未决项与风险

- **记录对齐**：已定 64 字节（§4），**要用基准验证**：做 16 对 64 的 A/B，看低占用时的往返延迟收益是否大于空间浪费。
- **`ShmChannelServer` 的单 loop 上限**：全部通道的握手与 Tx 环消费都在一个 IO 线程上；通道多、流量大时要分到多个 loop（通道在握手后可以迁到别的 loop，但 eventfd 与 UDS 都要跟着迁）。
- **`EBUSY` 的重试**：已做（服务端先探旧连接，客户端 `busy_retry` 窗口内退避重试，§6.1）；窗口长度（默认 1 秒）是拍的，没有依据。
- **阻塞的 `ShmChannelClient::Connect` / `Reattach`** 只适合启动时或后台线程里调用；业务用的是非阻塞的 `AgentClient`（握手、探活、Reattach 都挂在 `EventLoop` 上，后台按退避重试）。
- **`AgentClient` 的 `Stop()` 与 `PrepareRestart()` 的语义要业务自己分清**：`Stop()` 发 `Bye`（Agent 立刻销毁队列，不保留），`PrepareRestart(ms)` 之后的 `Stop()` / 析构当作重启（保留 `ms`）；进程被 kill 则走默认的保留时长。
  热更流程要在退出前调 `PrepareRestart`，否则保留的是默认的 30 秒。
- **保留着的通道占着配额**：保留期里的通道算在 `max_channels` 与 `max_total_bytes` 里，且保留期默认 30 秒、`PrepareRestart` 最长 10 分钟；一个不断换 busid 来握手又消失的对端（身份合法的前提下）能把配额占满到保留到期。
  目前只有总量上限，没有按 uid 的上限；是否要加，要看部署里谁能连上这个 UDS。
- **Agent 的路由器是单线程的**：`io_threads = 1`，一个 Agent 的全部通道、握手、仅 UDS 的连接、Agent 之间的链路都在一个 IO 线程上。容量上限没有测过；要扩就得把对端表与路由表换成线程安全的、通道按 App 绑到固定的 loop（§10 目标里写过），并解决跨 loop 的转发。
- **控制面还没有**：路由表（`RouteTable`）的真实来源是服务发现；NodeId 与通道名的映射现在靠 `AgentRouterOptions::node_of`（默认通道名就是十进制 NodeId）；"远端 Node 经哪条 Agent 链路"现在要调用方自己登记（`StaticRouteTable::SetRemote`），链路也要自己建（`NetModule::AddIoServer` / `AddIoClient`）；
  Agent 之间链路断了、重连了，路由表里的句柄要有人更新。`Hash` 路由是取模，不是一致性哈希；Any 是轮询，不看负载与灰度标记（`gray_tag` 只透传）。
- **信封字段要和仓库外的维护者对一遍**（§10）：目前是我按调研结果定的；`NodeId` 当不透明的 `u32`；`hops` 上限 8；`remaining_ms` 的 `u32` 上限约 49.7 天。UDS / 链路上的帧没有入队时刻，排队时间记 0（所以只在共享内存环里的排队才会被扣掉，内核 socket 缓冲里的不扣）。
- **"环满"的可见性是尽力的**：`SendResult` 加了 `Full`（没发出、连接还开着），但业务线程跨线程发送的写环发生在 IO 线程上，调用方当场只能靠槽上的 `tx_full` 提示知道。提示置位之前已经投递的几条，以及提示每 2 ms 过期后放过去试的那一条，仍可能返回 `Ok` 却因环满被丢（只进 `tx_full_drops`）；所以 `Ok` 仍是"网络层已接收"，要必达走 RPC 的 deadline 与重试。
  `kFullHintWindow` 是常量（2 ms），没有测量依据：太长会把 Agent 刚腾出的空间白白让掉，太短等于没提示。**超过单条上限（`tx_too_large`）仍返回 `Ok` 只计数**，是调用方的编程错误而不是流量问题，没有为它加一档；要加的话和 `Full` 一样，值加在 `SendResult` 上、实现在 `ShmConnection::Write`。
- **`ShmConnection` 没有空闲检测与写队列水位**：写环是同步的，不排队；Agent 一侧发给慢 App 的消息环满就丢（`rx_full`），不会像 TCP 那样把慢消费者关掉。
- **仅 UDS 的数据通道的帧格式**要 Agent 与 App 两端一致（`IFrameCodecFactory`），握手里不协商；配错了表现为连上就协议错误。
- **不支持经 TCP 连 Agent**：`AddAgentClient` 的端点必须是 `unix://` / `unix-abstract://`（`NetModule::AddAgentClient` 与 `AgentClient::Start` 各查一次，Agent 一侧的 `ShmChannelServer` 也只收 UDS 端点）。
  这是有意的边界，不是遗漏；没有具体场景驱动之前不做。原因：
  - **认人靠 `SO_PEERCRED`**：Agent 用内核担保的 uid 做准入，保留着的队列只还给同一个 uid（`Hello` 里的 pid 只是自称）。TCP 上没有对应物，谁能连上端口谁就能自称任意 busid、冒充别人收消息、伪造信封的 `src`；
    要补就得加认证（令牌 / 签名 / TLS），库里目前都没有。
  - **跨机之后 Sidecar 的价值基本没了**：共享内存、保留与 Reattach 都用不上，剩下一条普通 TCP 连接加 Agent 的路由。只要传输的话 `AddClient("tcp://…")` 已经有；缺的是 Agent 一侧"这条连接是谁"的登记，也就是上面的认证问题。
  - **威胁模型会变**：§7 假设对端在本机、信任边界是 uid；TCP 监听把 Agent 变成网络服务，要重新考虑暴露面、握手限速与恶意握手。
  - **多机部署已经有办法**：一台机器一个 Agent，App 连本机 Agent，Agent 之间经 TCP 链路转发（`NetModule::AddIoServer` / `AddIoClient`，端点是 `tcp://` 或 `unix://`）。"有的机器没法部署 Agent"更干净的做法是那台机器也起一个 Agent。
  - **值得做的场景**：App 与 Agent 无法同机（容器间没法共享 UDS 卷、开发机连远端 Agent 联调、Agent 铺开之前先让 App 接入）。
    **最小方案**：TCP 只允许仅 UDS 语义（`kShmRequired` 配 TCP 报错）；`AgentServer` 加可选的 TCP 监听，握手沿用 `Hello` / `Reply`（定长，仅 UDS 时不带 fd），完成后接进现有的 `StreamConnection` 数据通道，busid 唯一性、配额、路由不动；
    `Hello` 加令牌或签名字段（握手版本要升）；保留与 Reattach 在 TCP 上不存在，文档写清；测试要有 asan 下的恶意握手用例。主要成本在认证方案，不在代码量。
- **是否需要自旋**：实验（§10「睡前自旋实验」）说明收益很大——单条在途往返 23.7 → 0.3 µs，64 条在途吞吐 +15%——但代价是每个自旋的 IO 线程占满一个核，且窗口要大于消息间隔才有用。
  没人提出个位数微秒的延迟要求之前不做；要做的话：作为 `EventLoopGroup` 的可选项（默认 0 = 关）、做成 loop 级的忙轮询而不是塞在回调里、最好带自适应（最近的消息间隔小于窗口才自旋）、两端都要开才有完整收益，容器里要考虑 CPU 限额。
- **保留时长的默认值**：暂定 30 秒（Tbuspp2 的 `mq_space_hb_expire` 默认也是 30 秒，笔记里写的）；受 §8 的容量限制。
- **过期（§4，方案 B 已定）**：每批 `Commit` 读一次时钟的开销要单独量（目标是摊到整批后可忽略）；取批内首条 `TryReserve` 的时刻作为整批的入队时刻，是否过于保守要看实测；握手校验单调时钟差的阈值（1 秒）；`remaining_ms` 是 u32，上限约 49.7 天，超过的由 RPC 层拒绝；信封的字段布局由仓库外定，要和它的维护者对一遍。
- **看门狗**：N = 10 秒、满环 2 秒、第二阶段 M = 30 秒、App 自救的 T = 30 秒都是初值，要按你们实际的帧长和重启窗口再调；**已定：卡死先由 App 自救线程管，Agent 只记指标加日志**（§6.4）；通知控制面、`Probe` / `Abort`（§6.6，暂缓）、`pidfd` 升级都是后续；是否加心跳待定。
- **（暂缓项）`Probe` / `Abort` 与控制线程**：App 里多一个库线程，与"业务单线程"的约定怎么对齐（它只碰原子变量）；`abort()` 前打栈的超时值；`pidfd` 跨命名空间发信号在目标环境里到底行不行。App 自救线程跑起来之后再评估是否还需要。
- **Reattach 的细节**：新 Agent 没有"接纳窗口"的概念（它不知道应该有多少通道回来，也就没有"到齐"这回事，只受配额约束）；Reattach 与新 `Attach` 在同一 busid 上竞争时先到先得（后到的 `EBUSY`；交来的是同一块内存则视为恢复）；
  身份校验目前只有 uid（保留着的队列只还给同一个 uid），要不要加别的（比如可执行文件、cgroup）没定。
- **是否允许两条环不同大小**：已允许，默认相同；Rx 是否该默认更大，要看真实流量。
- **K8s 部署**：UDS 路径怎么在 App 容器与 Agent 容器之间共享（emptyDir 还是 hostPath）；共享内存的内存限制与 cgroup 记账（§8）。
- **TSAN 本机跑不了**：并发协议只能在真机上验证，在那之前不要当作已验证。
- **基准只有第一版**：§10「实测」是单连接、单 IO 线程、WSL2 上的对比，没测多 App 并发、路由器扇入、满环行为，也不是裸机；§10 其余数字仍只是目标。

## 附：与 Tbuspp2 的对照（按你知识库里的笔记）

| 维度 | Tbuspp2（笔记） | 本设计 |
|---|---|---|
| 同机队列 | 每个 Endpoint 一个 OutQ 加一个 InQ，两个文件 `q_0.dat` / `q_1.dat`，默认各 5 MB，由 Agent 创建 | 一个 memfd 里两条环，默认各 16 MiB，由 Agent 创建，之后双方各持一份 fd，谁活着谁保管（§0） |
| 队列容量怎么定 | Agent 启动参数 `-inq_size_kb` / `-outq_size_kb`（笔记里是全局参数） | **App 握手时指定**，每个通道自己的值，不合法就拒绝（§6.1） |
| 信令 | App 到 Agent 的 TCP 信令连接 | UDS（`SCM_RIGHTS` 传 fd，`SO_PEERCRED` 取身份，EOF 作存活探针） |
| 队列跨重启保留 | App 重启：有（`reserve_secs`，默认 `mq_space_hb_expire` 30 秒）；Agent 重启时 App 怎么办，笔记没写 | App 重启：有（默认 30 秒，受环容量限制，§6、§8）；**Agent 重启：有**，App 把 fd 传回（§6.5） |
| 唤醒 | 笔记没写 | eventfd 加睡眠标志（§5；睡前自旋可选，默认关） |
| 内存来源 | `/dev/shm` 文件，空间不足会挂载失败 | memfd，无路径，容量由配额管（§7、§8） |
| 每 Agent 的 Endpoint 数 | 默认上限 100 | 由配额配置，不设固定值 |
| 路由 | 在 Agent 里，两级队列加三个插件钩子 | 同样在 Agent 里；目标是少一级队列（§10） |
| 有状态路由一致性 | GroupVersion 加两阶段提交（L1/L2/L3） | **不在本文范围** |
