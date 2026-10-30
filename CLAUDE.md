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
- `src/cpp` — C++ 侧，`CMakeLists.txt` 在此。`framework/app` 对照 Go 版
  `src/go/core/app` 实现，语义以后者的 CLAUDE.md 为准。
  `base/queue` 是 SPSC/MPSC 无锁队列（包 moodycamel，公开 API 不暴露其类型），网络库的底座；
  `base/buffer` 是 `SharedBuffer`（引用计数的不可变字节块切片，业务回调的消息类型）与 `Buffer`（带读写游标的缓冲），只依赖标准库，网络库与业务都用；
  `base/thread`（仅 Linux）是 `Thread` / `this_thread` / `WaitGroup` / `SpinLock` / `ThreadPool`，创建失败返回 errno、库内不抛异常，设计与来源见 [docs/线程基础库.md](docs/线程基础库.md)（**tsan 同样没在本机跑过**）；
  `base/assert`（仅 Linux）是 `ASSERT_*` 防御式断言（Debug 失败 abort，Release 走防御式控制流并按调用点限流记日志）与 `Backtrace`；
  `base/util` 是 `DEFER` 与 `Singleton`；`base/string` 是字符串工具；`base/random` 是 xoshiro256**（有可复现的 `Random` 对象与线程局部的全局生成器），
  设计与来源见 [docs/基础工具库.md](docs/基础工具库.md)；
  `base/time` 是 `TimeUtil`（纯函数日期计算，支持"每天凌晨 5 点刷新"的游戏日）与 `Now`（帧缓存时钟、GM 偏移 / 冻结），
  `base/timer` 是 `TimerWheel`（分层时间轮，O(1) 添加 / 取消 / 重置、空转跳过），**定时器要用单调时钟 `Now::SteadyMs()` 驱动，别用墙钟**，见 [docs/时间与定时器.md](docs/时间与定时器.md)；
  `base/container` 是 `FlatHashMap` / `FlatHashSet`（Swiss table）与 `LinkedHashMap` / `LinkedHashSet`（保持插入顺序，可当 LRU），
  **插入可能让迭代器 / 引用失效、元素的移动构造必须 noexcept、`at()` 找不到键就 abort**，见 [docs/基础容器库.md](docs/基础容器库.md)；
  这几个库的 **tsan 同样没在本机跑过**，容器 bench 默认不构建：`cmake --preset release -DBREEZE_BUILD_CONTAINER_BENCH=ON`；
  `base/net` 是 Reactor 网络库（仅 Linux）：`Endpoint` / `SharedBuffer` / `Buffer` / `FrameCodec` 等基础件，
  `EventLoop` / `Connection` / `ConnectionHandle` / `EventLoopGroup` / `NetServer` / `NetClient`（含重连）已完整，
  `framework/net` 有 `MainLoopExecutor`（IO 线程事件回投 App 主循环，带入口背压）与 `NetModule`；
  **`NetModule` 的停机由 main 负责**：`App::Run` 返回后在同一个线程调 `NetModule::Shutdown()`。
  `base/net/internal/` 是不公开的实现，测试按需加私有 include 路径；
  **tsan 没有在 WSL 本机跑过**（沙箱里起不来），上真机前见 [docs/网络库设计.md](docs/网络库设计.md) §11.2；
  基线 bench 默认不构建：`cmake --preset release -DBREEZE_BUILD_BENCH=ON`（联网拉 muduo 源码），跑 `src/cpp/bench/run_baseline.sh`；
  `tsan` / `asan` 预设跑消毒器（`cmake --preset tsan`），`tsan.supp` 只压制 moodycamel 的 fence 误报。
  **本机没有 `protoc` 时不要跑全量构建**（`breeze_config_schemas` 要它），按 target 构建：`cmake --build build/cpp --target <名>`
- `src/*/gen/<类别>/` — 生成代码输出目录，不进版本库；一类生成物一个子目录（现有 `config/`，策划表等以后各占一个）。
  C++ 的 include 根是 `src/cpp/gen`：`#include "config/common_config.h"`。`python3 scripts/configure.py` 一次生成全部
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
