# CLAUDE.md

`breeze` 的全局索引。改动跨包约定、目录职责或构建链前先读本文件；
`src/go/core/app/` 的细节见 [src/go/core/app/CLAUDE.md](src/go/core/app/CLAUDE.md)。

## 项目

游戏服务端（GameServer）。Go 1.25，module `project`，远端 `yangfeng1997/breeze`。
`src/go/core/app` 是自研进程骨架，各 `*svr` 是它的消费者。

## 目录

- `src/go/core/app` — 进程骨架：模块注册 → 拓扑排序 → 启动编排 → 事件驱动主循环。
  **只负责启动，不负责停机** —— 没有任何 Stop/Shutdown/信号处理
- `src/go/server/*svr` — 服务进程，`main` 只做装配，实现放 `internal/`。
  `go.mod` 在 `src/go/`，import 路径不带 `src/go` 前缀（`project/core/app`）；
  Go 命令要在 `src/go/` 下跑，或用 `go -C src/go ...`
- `src/cpp` — C++ 侧，`CMakeLists.txt` 在此。**所有 `#include` 都从 `src/cpp` 起算**（`#include "base/net/endpoint/endpoint.h"`），`src/cpp` 是唯一的 include 根，各库不配自己的 include 目录，扁平名编译不过。目录不拦，**include 约定由 `tools/check_includes.py` 守着**（`includes_test`）：`base/` 不能 include `framework/`；net 之外的非测试代码只许 include net 的公开头清单（`PUBLIC_HEADERS`），新增公开头要加进清单。`framework/app` 对照 Go 版
  `src/go/core/app` 实现，语义以后者的 CLAUDE.md 为准。
  `base/queue` 是 SPSC/MPSC 无锁队列（包 moodycamel，公开 API 不暴露其类型），网络库的底座；
  `base/buffer` 是 `SharedBuffer`（引用计数的不可变字节块切片，业务回调的消息类型）与 `Buffer`（带读写游标的缓冲），只依赖标准库，网络库与业务都用；
  `base/thread`（仅 Linux）是 `Thread` / `this_thread` / `WaitGroup` / `SpinLock` / `ThreadPool`，创建失败返回 errno、库内不抛异常，设计与来源见 [docs/线程基础库.md](docs/线程基础库.md)（**tsan 同样没在本机跑过**）；
  `base/assert`（仅 Linux）是 `ASSERT_*` 防御式断言（Debug 失败 abort，Release 走防御式控制流并按调用点限流记日志），`base/backtrace`（仅 Linux）是 `Backtrace` 调用栈文本化（独立库，不依赖日志与断言）；
  `base/defer`（`DEFER`）、`base/singleton`（`Singleton` / `ThreadLocalSingleton`）、`base/noncopyable`（`NonCopyable`）各一个目录；`base/string` 是字符串工具；`base/random` 是 xoshiro256**（有可复现的 `Random` 对象与线程局部的全局生成器），
  `base/hash` 是 `MurmurHash64A`（**结果稳定**的字节哈希，分片 / 路由用，种子是强类型 `MurmurSeed`）、`base/vartype` 是 `VarType`（字符串存放、按需转数字 / 布尔，转换是严格的、失败不会悄悄得 0；带单槽转换缓存，**const 方法也会写缓存，不能多线程同时读同一个对象**；`TryAsVector` / `TryAsMap` 拆 `"1|2|3"`、`"12=34|45=78"`，默认分隔符 `|` 与 `=`、可用宏覆盖）、`base/time_record` 是 `TimeRecord` / `TIME_RECORD`（秒表，析构时可选地写日志，支持慢操作阈值），
  `base/uri` 是 `Uri`（RFC 3986 的 URI 引用解析与还原，"没有"与"空"分开，不做规范化）与 `PercentEncode` / `PercentDecode` / `ParseQuery`，
  设计与来源见 [docs/基础工具库.md](docs/基础工具库.md)；
  `base/fsm` 是 `fsm::Machine`（编译期状态机：转移与事件都是延迟执行、先进先出，**状态的成员函数要定义在所有状态之后**），见 [docs/状态机.md](docs/状态机.md)；
  `base/time` 是 `TimeUtil`（纯函数日期计算，支持"每天凌晨 5 点刷新"的游戏日）、`Now`（帧缓存时钟、GM 偏移 / 冻结）
  与 `PeriodicWindow`（周期开放窗口：从 start 起每隔 interval 开一次、持续 duration，半开区间，纯函数、`now` 传 `Now::Sec()`；
  **`duration == 0` 是空窗口不是"无限"，永久开放写 `kForever`**），
  `base/timer` 是 `TimerWheel`（分层时间轮，O(1) 添加 / 取消 / 重置、空转跳过），**定时器要用单调时钟 `Now::SteadyMs()` 驱动，别用墙钟**，见 [docs/时间与定时器.md](docs/时间与定时器.md)；
  `base/metrics` 是 `MessageStats`（按消息 id / 名字统计条数与字节、周期写日志，**非线程安全，一线程一实例**，`Tick` 传单调时钟），见 [docs/指标统计.md](docs/指标统计.md)；
  `base/event` 是 `EventBus`（类型安全的**同步**发布 / 订阅，回调里可以随意订阅 / 退订 / `Clear` / 嵌套发布，非线程安全）与 `ScopedSubscription`，见 [docs/事件总线.md](docs/事件总线.md)；
  `base/container` 是 `FlatHashMap` / `FlatHashSet`（Swiss table）与 `LinkedHashMap` / `LinkedHashSet`（保持插入顺序，可当 LRU），
  **插入可能让迭代器 / 引用失效、元素的移动构造必须 noexcept、`at()` 找不到键就 abort**，见 [docs/基础容器库.md](docs/基础容器库.md)；
  这几个库的 **tsan 同样没在本机跑过**，容器 bench 默认不构建：`cmake --preset release -DBREEZE_BUILD_CONTAINER_BENCH=ON`；
  `base/net` 是 Reactor 网络库（仅 Linux）：`Endpoint` / `SharedBuffer` / `Buffer` / `IFrameCodec` 等基础件，
  `EventLoop` / `IConnection` / `ConnectionHandle` / `EventLoopGroup` / `TcpServer` / `UdsServer` / `TcpClient` / `UdsClient`（基类 `StreamServer` / `StreamClient`，含重连）已完整，
  `RUdpServer` / `RUdpClient`（可靠 UDP：KCP 之上的消息通道，凭控制面发的一次性票据握手、**不自动重连**、**v1 没有加密 / 不支持迁移 / 一个服务端只用一个 loop**，
  KCP 来自 vcpkg，库是单独的 `breeze_net_rudp`，设计与威胁模型见 [docs/网络库设计.md](docs/网络库设计.md) §4.14，**UDP 不通时的回退（TCP 兜底、竞速、失败记忆）是业务的事，库不做，建议见 §4.15**，基线见 §9.7、优化见 §9.8、TCP / UDS / UDP / muduo 的完整压测数据见 §9.9：回环上相对裸 UDP 每条消息多约 0.8µs，空闲会话不占 CPU（静止的会话不参与定时器），在途数据在等重传的会话只被检查、不被推进（`rudp_updates` 统计真正推进的次数），每会话约 5.5KB，**容量上限是单 loop**），
  `framework/net` 有 `MainLoopExecutor`（IO 线程事件回投 App 主循环，带入口背压）与 `NetModule`（`udp://` 的 `AddServer` 与 `AddRUdpServer` 是 rudp 服务端；rudp 客户端用 `AddRUdpClient`，**不随模块启动**，业务拿到票据后自己 `Start()`）；
  **`NetModule` 的停机由 main 负责**：`App::Run` 返回后在同一个线程调 `NetModule::Shutdown()`。
  `base/net` 的目录**只按概念归类，不做访问控制**：`loop/`（EventLoop / EventLoopGroup、连接表、统计）、`socket/`（`UniqueFd`、地址互转）、`endpoint/`、`codec/`、`connection/`（`IConnection` 接口、句柄、回调、`INetServer` / `INetClient`）、`stream/`（TCP / UDS 共用的字节流：公开的 `StreamServer` / `StreamClient` 基类，`stream/internal/` 里是 `StreamConnection` / `Acceptor` / `Connector` 等实现）；每种传输一个目录，端点 / 选项 / 服务端 / 客户端与它私有的实现放在一起：`tcp/`（`TcpServer` `TcpClient` `TcpEndpoint` `TcpOptions` `tcp_socket`）、`uds/`（`UdsServer` `UdsClient` `UdsEndpoint` `uds_socket`），`rudp/`（`RUdpServer` `RUdpClient` `RUdpEndpoint` `RUdpTicket` `RUdpOptions`、线格式 `rudp_protocol.h`，`rudp/internal/` 里是 `RUdpConnection` `KcpSession` `UdpSocket` `TicketTable`）；`connection/` 里的 `INetServer` / `INetClient` 是传输无关的服务端 / 客户端接口，`NetModule` 经它们持有；`testing/` 放测试公共件、集成测试与不抛异常的检查脚本，顶层没有散放文件；TCP 专有的选项（keepalive 等）在 `TcpOptions`，UDS 的选项类型里没有；`IConnection` 是纯接口（只有虚函数、不能带数据成员），具体类 `StreamConnection` 在 `stream/internal/`；依赖方向与命名规则见 [docs/网络库设计.md](docs/网络库设计.md) §2 / §4.13；
  **tsan 没有在 WSL 本机跑过**（沙箱里起不来），上真机前见 [docs/网络库设计.md](docs/网络库设计.md) §11.2；
  基线 bench 默认不构建：`cmake --preset release -DBREEZE_BUILD_BENCH=ON`（联网拉 muduo 源码），跑 `src/cpp/bench/run_baseline.sh`（1 / 64 / 10000 连接闭环）与 `run_scale.sh`（1000 / 5000 / 10000 / 100000 连接的定速负载、内存、建连、拐点，`--rate`）（`TRANSPORTS="tcp udp"` 选传输，udp 是 rudp 的基线；`net_bench --mem-probe N` 测每连接内存）；
  `tsan` / `asan` 预设跑消毒器（`cmake --preset tsan`），`tsan.supp` 只压制 moodycamel 的 fence 误报。
  **本机没有 `protoc` 时不要跑全量构建**（`breeze_config_schemas` 要它），按 target 构建：`cmake --build build/cpp --target <名>`
- `src/*/gen/<类别>/` — 生成代码输出目录，不进版本库；一类生成物一个子目录（现有 `config/`，策划表等以后各占一个）。
  C++ 从 `src/cpp` 起算：`#include "gen/config/common_config.h"`。`python3 scripts/configure.py` 一次生成全部
  并配置 cmake；C++ 构建在 schema 变动时也会自动重生成自己那份
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
- `docs/` — 设计文档。服务间通信的基线是
  [docs/Actor模型与RPC设计.md](docs/Actor模型与RPC设计.md)：实现 RPC 或 actor 层前先读它。
  C++ 网络库（`base/queue` / `base/net` / `framework/net`）的设计基线是
  [docs/网络库设计.md](docs/网络库设计.md)：动网络库前先读它
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
