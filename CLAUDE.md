# CLAUDE.md

`breeze` 的全局索引。改动跨包约定、目录职责或构建链前先读本文件；
`src/go/core/app/` 的细节见 [src/go/core/app/CLAUDE.md](src/go/core/app/CLAUDE.md)。

## 项目

游戏服务端（GameServer）。Go 1.25，module `project`，远端 `yangfeng1997/breeze`。
`src/go/core/app` 是自研进程骨架，各 `*svr` 是它的消费者。

C++ 侧**格式化一律用 `fmt::format`**（`fmt` 是 vcpkg 直接依赖），不用 `std::format`：libstdc++ 的实现比 fmt 慢，仓库里只保留一套格式化实现，`fmt::formatter` 是唯一的自定义格式化入口。

## 目录

- `src/go/core/app` — 进程骨架：模块注册 → 拓扑排序 → 启动编排 → 事件驱动主循环。
  **只负责启动，不负责停机** —— 没有任何 Stop/Shutdown/信号处理
- `src/go/server/*svr` — 服务进程，`main` 只做装配，实现放 `internal/`。
  `go.mod` 在 `src/go/`，import 路径不带 `src/go` 前缀（`project/core/app`）；
  Go 命令要在 `src/go/` 下跑，或用 `go -C src/go ...`
- `src/cpp` — C++ 侧，`CMakeLists.txt` 在此。**所有 `#include` 都从 `src/cpp` 起算**（`#include "base/net/endpoint/endpoint.h"`），`src/cpp` 是唯一的 include 根，各库不配自己的 include 目录，扁平名编译不过。目录不拦，**include 约定由 `tools/check_includes.py` 守着**（`includes_test`）：`base/` 不能 include `framework/`；net 之外的非测试代码只许 include net 的公开头清单（`PUBLIC_HEADERS`），新增公开头要加进清单。`framework/app` 对照 Go 版
  `src/go/core/app` 实现，语义以后者的 CLAUDE.md 为准。
  `base/queue` 是 SPSC/MPSC 无锁队列（包 moodycamel，公开 API 不暴露其类型），网络库的底座；**`PeakApprox` 是近似下界**（首次与每 1024 次批量出队才用 `size_approx` 采样，其余用取出条数，**别改成每次都采**：它要遍历生产者 / 全部块，随队列深度增长，会让消费者越慢越积压，对照实验见 [docs/Actor与线程模型设计.md](docs/Actor与线程模型设计.md) §9）；批量出队一次取 256 条左右最快，长寿生产线程要用 `ProducerToken`；
  `base/buffer` 是 `SharedBuffer`（引用计数的不可变字节块切片，业务回调的消息类型）与 `Buffer`（带读写游标的缓冲），只依赖标准库，网络库与业务都用；
  `base/thread`（仅 Linux）是 `Thread` / `this_thread` / `WaitGroup` / `SpinLock` / `ThreadPool`，创建失败返回 errno、库内不抛异常，设计与来源见 [docs/线程基础库.md](docs/线程基础库.md)（**tsan 同样没在本机跑过**）；
  `base/assert`（仅 Linux）是 `ASSERT_*` 防御式断言（Debug 失败 abort，Release 走防御式控制流并按调用点限流记日志），`base/backtrace`（仅 Linux）是 `Backtrace` 调用栈文本化（独立库，不依赖日志与断言）；
  `base/defer`（`DEFER`）、`base/singleton`（`Singleton` / `ThreadLocalSingleton`）、`base/noncopyable`（`NonCopyable`）各一个目录；`base/string` 是字符串工具（含严格的 `Base64Decode`——拒绝空白 / 非规范填充位，握手头是不可信输入——与可分段喂入的 `Utf8Validator`）；`base/random` 是 xoshiro256**（有可复现的 `Random` 对象与线程局部的全局生成器），
  `base/hash` 是 `MurmurHash64A`（**结果稳定**的字节哈希，分片 / 路由用，种子是强类型 `MurmurSeed`；另有 `Sha1`，**只给 WebSocket 握手用**，已不抗碰撞，别挪作他用）、`base/vartype` 是 `VarType`（字符串存放、按需转数字 / 布尔，转换是严格的、失败不会悄悄得 0；带单槽转换缓存，**const 方法也会写缓存，不能多线程同时读同一个对象**；`TryAsVector` / `TryAsMap` 拆 `"1|2|3"`、`"12=34|45=78"`，默认分隔符 `|` 与 `=`、可用宏覆盖）、`base/time_record` 是 `TimeRecord` / `TIME_RECORD`（秒表，析构时可选地写日志，支持慢操作阈值），
  `base/uri` 是 `Uri`（RFC 3986 的 URI 引用解析与还原，"没有"与"空"分开，不做规范化）与 `PercentEncode` / `PercentDecode` / `ParseQuery`，
  设计与来源见 [docs/基础工具库.md](docs/基础工具库.md)；
  `base/fsm` 是 `fsm::Machine`（编译期状态机：转移与事件都是延迟执行、先进先出，**状态的成员函数要定义在所有状态之后**），见 [docs/状态机.md](docs/状态机.md)；
  `base/time` 是 `TimeUtil`（纯函数日期计算，支持"每天凌晨 5 点刷新"的游戏日）、`Now`（帧缓存时钟、GM 偏移 / 冻结）
  与 `PeriodicWindow`（周期开放窗口：从 start 起每隔 interval 开一次、持续 duration，半开区间，纯函数、`now` 传 `Now::Sec()`；
  **`duration == 0` 是空窗口不是"无限"，永久开放写 `kForever`**），
  `base/timer` 是 `TimerWheel`（分层时间轮，O(1) 添加 / 取消 / 重置、空转跳过），**定时器要用单调时钟 `Now::SteadyMs()` 驱动，别用墙钟**，见 [docs/时间与定时器.md](docs/时间与定时器.md)；
  `base/metrics` 是 `MessageStats`（按消息 id / 名字统计条数与字节、周期写日志，**非线程安全，一线程一实例**，`Tick` 传单调时钟），见 [docs/指标统计.md](docs/指标统计.md)；
  `base/event` 是 `EventBus`（类型安全的**同步**发布 / 订阅，回调里可以随意订阅 / 退订 / `Clear` / 嵌套发布，非线程安全）与 `ScopedSubscription`，见 [docs/事件总线.md](docs/事件总线.md)；
  `base/lua_binding` 是 Lua 5.5.1 绑定库（`breeze_lua_binding`，小型编译库 + 头文件模板，依赖 vcpkg 的 `lua` 与 `fmt`）：链式 `Class<T>` 构建器 + luakit 风格键值糖，默认冒号调用零分配，错误不抛 C++ 异常（两段式 trampoline + `std::expected<T, Error>`，核心可在 `-fno-exceptions` 下编译）；C++→Lua 默认 `lua_pcall`，`CallUnprotected` / `CallGlobalUnprotected` 是显式 `lua_call` 逃生门（报错会 longjmp，确认不报错才用）；可选 `breeze_lua_binding_ext`（table/string/os 工具）、`sandbox.h`（内存 / 指令 / 时间预算）、`coroutine.h`。debug / asan / tsan 预设经 vcpkg overlay 链接带 `LUA_USE_APICHECK` 的 Lua；debug、asan+ubsan、tsan、`-fno-exceptions` 下测试均通过（全仓库统一 C++26）。**`Reference`/`LuaTable`/`LuaFunction` 必须在创建它的线程析构，异步线程只传 ID、不传 Reference；库不实现跨线程释放队列；它们不可拷贝，需要独立引用时用显式 `Clone()`。** **不属于绑定库的职责一律不做**：跨线程 Reference 队列、协程超时定时器、sandbox/coroutine 的 hook 复用器、C++ 栈采集、二进制 codec 都归框架/运行时/诊断库。绑定层设计与实测见 [docs/Lua绑定库设计.md](docs/Lua绑定库设计.md)；框架层（`LuaModule` / `AsyncWorkerModule` / worker 类型 / 文件级热更）见 [docs/Lua脚本框架设计.md](docs/Lua脚本框架设计.md)；**框架层明确不做单进程多 Shard，扩容靠多进程/多服务实例。**
  `base/container` 是 `FlatHashMap` / `FlatHashSet`（Swiss table）与 `LinkedHashMap` / `LinkedHashSet`（保持插入顺序，可当 LRU），
  **插入可能让迭代器 / 引用失效、元素的移动构造必须 noexcept、`at()` 找不到键就 abort**，见 [docs/基础容器库.md](docs/基础容器库.md)；
  这几个库的 **tsan 同样没在本机跑过**，容器 bench 默认不构建：`cmake --preset release -DBREEZE_BUILD_CONTAINER_BENCH=ON`；
  `base/lua_pb`（库 `breeze_lua_pb`）是 Lua 侧的 protobuf 编解码：**只支持 proto3 与 Lua 5.5.1**，三层 `wire.h`（varint/zigzag/定长 + 带上界 Reader + 可回填长度 Writer）/ `schema`（自带 wire reader 读 `FileDescriptorSet`，不链 libprotobuf）/ `codec`（Lua table ↔ wire）；**Schema 编译后不可变、可跨线程只读共享，每个 `lua_State` 一个 `Codec`；`Codec` 必须先于它绑定的 `lua_State` 关闭而析构**；热路径只用 raw API，核心无 C++ 异常（`std::expected`，`-fno-exceptions` 下测试通过）；编码用 `lua_next` + per-state 指针哈希 + 位图、按字段号序输出；**描述符走方案 b：构建期 `proto/*.proto` 经 `breeze_proto_descriptor` 产出 `run/proto/*.pb`，运行期由框架读一次交给 `Schema::Load`，库不碰文件系统**；已知限制：uint64 ≥ 2^63 回绕成 `lua_Integer`、repeated 空洞检测是尽力而为（Lua `#` 未定义）、嵌套上限 100（与 protobuf C++ / upb 一致）。绑定层已接上：`OpenPb` 注册 `require("pb")` 的 `pb.encode` / `pb.decode`（Codec 放 userdata、`__gc` 里析构，所以不用宿主排生命周期），**失败抛 Lua 错误**——生产路径请用 C++ API 拿 `std::expected`，别走 Lua 表面；七个用例（wire / schema / noexc / codec / hostile / binding / binding_noexc）在 `-DLUA_USE_APICHECK` 的 Lua 上也全过，**asan / tsan 已在本机 WSL 复核过**（WSL2 Ubuntu 22.04 + PPA 的 g++-14 `-std=c++26`：asan+ubsan 五个用例全干净、tsan 的多线程用例无竞争报告；但**仓库预设（vcpkg + clang）在本机仍跑不了**，因为 WSL 里没有 vcpkg、系统 clang 只有 14）；基准 `bench/lua_pb_bench.cpp`（`-DBREEZE_BUILD_LUA_PB_BENCH=ON`，要用 release 预设），与 starwing lua-protobuf 的并排数字、建表预分配的校准结论、以及 `lua_pushexternalstring` 为什么不做，都在 §21。见 [docs/Lua_pb库设计.md](docs/Lua_pb库设计.md)；
  `base/shm`（仅 Linux）是 Sidecar 同机通道的底座：封印过的 memfd 里两条单向 SPSC 字节环 + eventfd（`ShmChannel` / `RingProducer` / `RingConsumer`），记录 64 字节对齐、环做镜像映射，**对端写的每个字节都当不可信输入**（接手校验必须对"对端还活着"安全，`RingStateValid` 有推导）；唤醒靠睡眠标志加 `seq_cst` 栅栏（Dekker），**测试里不要绑核**（亲和性会被新线程继承，锁步用例会慢到卡死）；**tsan 同样没在本机跑过，ARM 也没跑过**（x86 看不出 release / acquire 写错）；性能只有一版单连接的对比压测（`bench/agent_bench.cpp`，`-DBREEZE_BUILD_AGENT_BENCH=ON`，跑法 `bench/run_agent.sh`；另有 `bench/ring_bench.cpp`（`bench/run_ring.sh`）比共享内存环与进程内队列、`bench/wake_bench.cpp`（`bench/run_wake.sh`）比唤醒机制，结果与局限见设计文档 §10「实测」）；**睡前自旋没有实现**（临时副本里的实验：单条在途往返 23.7 → 0.3 µs，但每个自旋的 IO 线程占满一个核，取舍见设计文档 §10 / §13）；环控制块的 `consumer_stats.empty_wakeups` 统计"读到旧计数、白醒一次"；握手与服务端 / 客户端在 `base/net/agent/shm/`（库 `breeze_net_shm`，UDS 传 fd 的封装 `stream/internal/fd_passing` 在 `breeze_net` 里）：`ShmChannelServer`（Agent，收 `EventLoopGroup&`，回调里只管在预算内搬空 Tx 环，`WokeUp` / `PrepareToSleep` / 让出由库做；全部通道在 accept 所在的一个 loop 上）与 `ShmChannelClient`（App，`Connect` 是阻塞的，只在启动时调）；**保留 / 恢复（App 重启）、Reattach（Agent 重启）、控制消息 `PrepareRestart` / `Bye` 已实现**（UDS 断开进 `Detached`，默认保留 30 秒，遗言照常排空；业务回调里的映射要以 `ShmServerChannel::id()` 为键，不要只用 busid）；**握手时协商传输方式也实现了**（`UdsOnly` / `ShmPreferred` 给不了就退化 / `ShmRequired`，仅 UDS 时那条 UDS 交给 `StreamConnection` 当数据通道，两种传输共用通道名与配额；App 侧开关 `net.agent_shm_enabled`）；`base/net/agent/`（库 `breeze_net_agent`）有信封 `EnvelopeHead`（64 字节，前 48 字节是路由区，相对剩余毫秒）、`AgentRouter`（Agent 的路由转发，**单线程，`io_threads` 必须是 1**）、`AgentServer`、`AgentClient`（非阻塞，共享内存做成 `ShmConnection`，**Agent 掉线连接不关、后台 Reattach**）；`NetModule::AddAgentClient` / `AddAgentServer` / `AddIoServer` / `AddIoClient` 已接入（**`NetModule::Shutdown` 必须在调 `App::Run` 的线程上调**）；控制面（路由表真实来源）、多 loop 的 Agent、`framework/app` 的自检线程还没做；**测试里处理器必须比 EventLoopGroup 活得久**（group 析构还会回调它，asan 看不见、debug 下偶发崩溃）；握手对 App 与 Agent 双方都不信（fd 数量与种类、布局互相核对），**这一层的测试必须也在 asan 下跑**（在回调栈里销毁自己、越界写槽位、析构后残留的任务，release 下测不出来）；设计与验证状态见 [docs/共享内存IPC设计.md](docs/共享内存IPC设计.md) §11 / §12；
  `base/net` 是 Reactor 网络库（仅 Linux）：`Endpoint` / `SharedBuffer` / `Buffer` / `IFrameCodec` 等基础件，
  `EventLoop` / `IConnection` / `ConnectionHandle` / `EventLoopGroup` / `TcpServer` / `UdsServer` / `TcpClient` / `UdsClient`（基类 `StreamServer` / `StreamClient`，含重连）已完整，
  `RUdpServer` / `RUdpClient`（可靠 UDP：KCP 之上的消息通道，凭控制面发的一次性票据握手、**不自动重连**、**v1 没有加密 / 不支持迁移 / 一个服务端只用一个 loop**，
  KCP 来自 vcpkg，库是单独的 `breeze_net_rudp`，设计与威胁模型见 [docs/网络库设计.md](docs/网络库设计.md) §4.14，**UDP 不通时的回退（TCP 兜底、竞速、失败记忆）是业务的事，库不做，建议见 §4.15**，基线见 §9.7、优化见 §9.8、TCP / UDS / UDP / muduo 的完整压测数据见 §9.9：回环上相对裸 UDP 每条消息多约 0.8µs，空闲会话不占 CPU（静止的会话不参与定时器），在途数据在等重传的会话只被检查、不被推进（`rudp_updates` 统计真正推进的次数），每会话约 5.5KB，**容量上限是单 loop**），
  **跨线程 `ConnectionHandle::Send` 有批内合并**（`EventLoop::RunTasks` 向前看，相邻的同连接 `Send` 经 `IConnection::SendBatchFromTask` 合成一次 `sendmsg`；主循环对同一连接连发 K 条时吞吐 +72%～+122%，普通回显不受影响，设计与实测见设计文档 §4.6 / §9.12；改这条路径要跑 `net_bench --impl breeze-mainloop --fanout K` 做 K=1 的 A/B，**单条 Send 必须走原函数体**）；`framework/net` 有 `MainLoopExecutor`（IO 线程事件回投 App 主循环，带入口背压）与 `NetModule`（`udp://` 的 `AddServer` 与 `AddRUdpServer` 是 rudp 服务端；rudp 客户端用 `AddRUdpClient`，**不随模块启动**，业务拿到票据后自己 `Start()`）；
  **`NetModule` 的停机由 main 负责**：`App::Run` 返回后在同一个线程调 `NetModule::Shutdown()`。
  `base/net` 的目录**只按概念归类，不做访问控制**：`loop/`（EventLoop / EventLoopGroup、连接表、统计）、`socket/`（`UniqueFd`、地址互转）、`endpoint/`、`codec/`、`connection/`（`IConnection` 接口、句柄、回调、`INetServer` / `INetClient`）、`stream/`（TCP / UDS 共用的字节流：公开的 `StreamServer` / `StreamClient` 基类，`stream/internal/` 里是 `StreamConnection` / `Acceptor` / `Connector` 等实现）；每种传输一个目录，端点 / 选项 / 服务端 / 客户端与它私有的实现放在一起：`tcp/`（`TcpServer` `TcpClient` `TcpEndpoint` `TcpOptions` `tcp_socket`）、`uds/`（`UdsServer` `UdsClient` `UdsEndpoint` `uds_socket`），`rudp/`（`RUdpServer` `RUdpClient` `RUdpEndpoint` `RUdpTicket` `RUdpOptions`、线格式 `rudp_protocol.h`，`rudp/internal/` 里是 `RUdpConnection` `KcpSession` `UdpSocket` `TicketTable`）；`agent/`（Agent 的信封 / 路由 / 服务端 / 客户端，`agent/shm/` 是它的共享内存传输层，编成单独的 `breeze_net_shm`）；`ws/`（WebSocket：`WsServerCodecFactory` / `WsClientCodecFactory` 是 codec 工厂，交给 `TcpServer` / `TcpClient` 即是 ws://，叠 TLS 层即是 wss://；`ws/internal/` 是帧与 HTTP 升级的纯函数实现；**不协商任何 extension**，`OnClosed` 的 `sys_errno` 带 WebSocket 关闭码；`on_upgrade` 会被多个 IO 线程并发调用，必须线程安全；服务端发出的帧头只依赖长度，payload 仍零拷贝共享，客户端出站才逐条拷贝加掩码；libFuzzer 目标 `ws_decode_fuzz` / `ws_http_fuzz` 默认不构建：`-DBREEZE_BUILD_FUZZ=ON -DBREEZE_SANITIZE=fuzzer-no-link,address,undefined`，clang 才有，见 `ws/fuzz/`）；`tls/`（TLS：`TlsContext::CreateServer` / `CreateClient` 产出 `IStreamLayerFactory`，交给 `StreamServerOptions::tls` / `StreamClientOptions::tls`，再配 WebSocket 的 codec 工厂即 wss://；库 `breeze_net_tls`，**BoringSSL 只在这里 PRIVATE 链接，公开头里没有 openssl 的头文件**；`BREEZE_BUILD_NET_TLS` 默认开）。**字节流层 / 握手阶段**（设计见 [docs/网络库设计.md](docs/网络库设计.md) §4.16）：`StreamConnection` 是基类，`PlainStreamConnection` 与 `LayeredStreamConnection` 各管一条读写路径，**明文热路径不为 TLS 多一次判断**——改 `stream_connection.cpp` 里的热函数（`HandleRead` / `SendImpl` / `HandleWrite` / `DecodeAll`）之后要重跑反汇编对比，别只看测试通过（`DecodeAll` 是 `always_inline` 版本，加防御代码时曾因此被编译器放弃内联）；`OnConnected` 要等所有握手来源完成，握手失败没有 `OnClosed`、只记 `NetStats::handshake_failed[Timeout/Io/Tls/Upgrade]`，客户端的 `on_connect_failed` 收到的是固定映射的 errno（不是 TLS 库的错误码）；层出错或收到 close_notify 时连接关闭之前会尽力把层里待发的字节（alert / close_notify）写出去一次。**TLS 的几个坑**：服务端握手完成时必须做一次零字节 `SSL_write`，BoringSSL 才会把 session ticket 发出去（否则只读连接永远发不出 ticket、重连恢复不了）；错误队列是线程本地的，但 BoringSSL 在每个 `SSL_*` 入口自己清，所以我们不调 `ERR_clear_error()`（换 OpenSSL 必须补）；groups 只开 X25519 + P-256（不跟 BoringSSL 默认去选后量子混合）；服务端证书建议 ECDSA P-256；TLS 1.3 + mTLS 时客户端会先 `OnConnected` 再被拒；`verify_peer` 为真时必须给 `server_name`；`NetModule` 的 scheme 已接：`tcp://` / `tls://` / `ws://host:port[/path]` / `wss://…`；ws/wss 用**不带 codec 工厂**的 `AddServer` / `AddClient`（给了工厂是错误），配置在 `net.proto` 的 `tls` / `ws` 两段（已同步 `common.yaml`），TLS 上下文在 `NetModule::Init` 建一次，TLS 实现按 `BREEZE_BUILD_NET_TLS` **链接期选择**（OFF 时是明确报错的桩）；**证书热更 `NetModule::ReloadTls()`**（重读 `net.tls` 的文件、原子替换，新连接用新证书、老连接不受影响，失败保留旧的；不监听文件）；BoringSSL 是 vcpkg 的 `tls` feature（默认开），不要它用 `release-notls` 预设。`connection/` 里的 `INetServer` / `INetClient` 是传输无关的服务端 / 客户端接口，`NetModule` 经它们持有；`testing/` 放测试公共件、集成测试与不抛异常的检查脚本，顶层没有散放文件；TCP 专有的选项（keepalive 等）在 `TcpOptions`，UDS 的选项类型里没有；`IConnection` 是纯接口（只有虚函数、不能带数据成员），具体类 `StreamConnection` 在 `stream/internal/`；依赖方向与命名规则见 [docs/网络库设计.md](docs/网络库设计.md) §2 / §4.13；
  **tsan 没有在 WSL 本机跑过**（沙箱里起不来），上真机前见 [docs/网络库设计.md](docs/网络库设计.md) §11.2；
  基线 bench 默认不构建：`cmake --preset release -DBREEZE_BUILD_BENCH=ON`（联网拉 muduo 源码；`--transport tls|ws|wss` 只有 `--impl breeze`，证书进程内现造，需 `BREEZE_BUILD_NET_TLS=ON`，实测见设计文档 §9.11），跑 `src/cpp/bench/run_baseline.sh`（1 / 64 / 10000 连接闭环）与 `run_scale.sh`（1000 / 5000 / 10000 / 100000 连接的定速负载、内存、建连、拐点，`--rate`）（`TRANSPORTS="tcp udp"` 选传输，udp 是 rudp 的基线；`net_bench --mem-probe N` 测每连接内存）；
  `tsan` / `asan` 预设跑消毒器（`cmake --preset tsan`），`tsan.supp` 只压制 moodycamel 的 fence 误报。
  **本机没有 `protoc` 时不要跑全量构建**（`breeze_config_schemas` 要它），按 target 构建：`cmake --build build/cpp --target <名>`
- `src/*/gen/<类别>/` — 生成代码输出目录，不进版本库；一类生成物一个子目录（现有 `config/`，策划表等以后各占一个）。
  C++ 从 `src/cpp` 起算：`#include "gen/config/common_config.h"`。`python3 scripts/configure.py` 一次生成全部
  并配置 cmake；C++ 构建在 schema 变动时也会自动重生成自己那份
- `proto/` — IDL 源文件（手写 `.proto`，RPC 服务与消息定义），**只放源，不放生成物**；配置 schema 仍在 `config/schema/`。
  构建期经 CMake 函数 `breeze_proto_descriptor` 用 protoc 产出 `run/proto/*.pb`（`FileDescriptorSet`，`/run/` 不进版本库），
  运行期由框架读一次交给 `base/lua_pb`；protoc 按 `-DBREEZE_PROTOC` > PATH 查找，目录为空则不建目标。生成器（`tools/protoc-gen-svcstub`）还没开工
- `config/schema` — 配置 schema（`*.proto`，自定义 option 在 `options.proto`），配置结构的事实源；
  配置数据本身仍是 YAML：`config/common.yaml`（全量，带本机开发默认值，单独可读）+ `config/env/{dev,test,prod}.yaml`
  （后者覆盖前者；env 文件只能覆盖 schema 里标了 `env_value` 的字段，且要全部写出，`cluster.env` 必须与文件名一致；
  密钥用 `${ENV:}` 注入，只有 dev 有默认值）。
  C++ 框架的 `ConfigModule`（`src/cpp/framework/kernel/config_module.h`）只加载 `--config` 传入的**一个**文件，
  不隐式找 env / 业务文件；业务自己的 yaml 由业务自己 `Config::Load`。
  `common_config_test` 会读这些真实文件，改 schema 或 yaml 后对不上会失败。`tools/configgen` 从 schema 生成 C++ / Go / JSON Schema（需要 `protoc`），
  用法与 schema 格式见 [tools/configgen/CLAUDE.md](tools/configgen/CLAUDE.md)。目前有 `common.proto`
  （cluster / etcd / redis / mongo，另引用 `net.proto` 的 `net` 段：网络库，字段全有生产默认值，段本身非 required）；
  生成器自带样例在 `tools/configgen/testdata/schema/`
- `docs/` — 设计文档。C++ 侧主循环与 RPC 的设计基线是
  [docs/Actor与线程模型设计.md](docs/Actor与线程模型设计.md)（文件名保留历史路径，内容已重写）：**设计阶段，没有代码**；只有 main loop 直接执行，**不做 Actor / ActorSystem / 进程内多 Shard / 在线迁移**；`Dispatcher` 把网络消息投 main loop，阻塞/计算走 `ThreadPool` + `Offload`；RPC 一律异步（`CallHandle` / `ReplyHandle`），IDL 用 gRPC schema 写法 + 自研生成器 + libprotobuf，客户端命令与内部 RPC 用 service 级 `audience` 区分；main loop 实体由 `EntityManager` 管理并自动登记；扩容靠多进程/微服务。旧的 Actor/Shard 设计在 [docs/Actor与线程模型设计-废弃.md](docs/Actor与线程模型设计-废弃.md)，**仅历史备份，不要参考**。
  C++ 网络库（`base/queue` / `base/net` / `framework/net`）的设计基线是
  [docs/网络库设计.md](docs/网络库设计.md)：动网络库前先读它；
  Sidecar 的同机共享内存通道（`memfd` + 两条 SPSC 环 + `eventfd`；**`base/shm`、握手与传输协商、保留与 Reattach、控制消息、信封与 Agent 路由转发、`AgentClient`、`NetModule` 接入已实现，控制面、多 loop 的 Agent、`framework/app` 的自检线程尚未实现**）见
  [docs/共享内存IPC设计.md](docs/共享内存IPC设计.md)；
  App ↔ Agent 的通道设计（控制面/数据面拆分、`ShmAgentChannel` 主线程零拷贝、`UdsAgentChannel` IO 线程 + `MainLoopInbox`、**握手时确定 transport、不做运行中降级**）见
  [docs/Agent通道设计.md](docs/Agent通道设计.md)；
  RPC/IDL 的运行时分层与 C++/Lua 混合 codec（`RpcModule` 管 RPC 语义、`MessageModule` 管消息号/派发、payload 按 service/method 选 C++ protobuf 或 `base/lua_pb`）见
  [docs/RPC与IDL设计.md](docs/RPC与IDL设计.md)；
  Lua 侧的 protobuf 编解码库 `base/lua_pb`（自研，**只支持 proto3 与 Lua 5.5.1**；三层 `wire`/`schema`/`codec`；Schema 不可变可跨线程只读共享、每 LuaState 一个 `Codec`；编码遍历 = `lua_next` + per-state 指针哈希 + 位图、按字段号序输出；核心无 C++ 异常；**描述符走方案 b：构建期 `proto/*.proto` → `run/proto/*.pb`，运行期由框架读一次交给 `Schema::Load`，库不碰文件系统**）见
  [docs/Lua_pb库设计.md](docs/Lua_pb库设计.md)
- `scripts/` — `configure.py`（生成代码 + 配置 cmake，只用标准库）与 Windows 环境脚本（**目录名拼写如此，勿"修正"**）
- `build/`、`run/` — 构建产物，不进版本库

## 服务

已实现的只有 `testsvr`：不做业务，是其余 svr 的**装配模板**
（见 [src/go/server/testsvr/main.go](src/go/server/testsvr/main.go)）。

`battlesvr` / `dbsvr` / `gatesvr` / `lobbysvr` / `loginsvr` / `matchsvr` /
`namesvr` / `onlinesvr` / `routeagent` / `routesvr` / `src/go/pkg` 都是**空目录**。
git 不追踪空目录 —— clone 后不存在，只表示规划中的拆分。

## 构建链（未完成）

`scripts/setup_windows_env.ps1` 负责装环境：git / Python+PyYAML / Go / protoc /
protoc-gen-go / .NET runtime（Luban 用）。etcd、redis、mongo 复用远端
`tencent.codepeak.cn`，不装本地。

脚本尾注给出的构建命令：

```
python scripts/config.py --env dev --world-id 2 --out run
python scripts/build.py --out run --build build
cd run && ./startall.sh
```

**这三个入口都还不存在** —— `scripts/` 下目前只有 setup 脚本本身，构建链待补。

## 注释规范

一律简体中文；极简、精准、有参考性；禁止成片冗余。

只写代码读不出的东西 —— 约束、代价、反直觉点、失败模式，并附可跳转锚点
（文件、符号、测试名）。不复述代码逻辑，不写过渡句与总结段。
[`src/go/core/app/CLAUDE.md`](src/go/core/app/CLAUDE.md) 与 `testsvr/main.go` 是本规范的范本。
