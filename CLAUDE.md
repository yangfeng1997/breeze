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
  `src/go/core/app` 实现，语义以后者的 CLAUDE.md 为准
- `src/*/gen` — 生成代码输出目录，不进版本库
- `docs/` — 设计文档。服务间通信的基线是
  [docs/Actor模型与RPC设计.md](docs/Actor模型与RPC设计.md)：实现 RPC 或 actor 层前先读它
- `scripts/` — Windows 环境脚本（**目录名拼写如此，勿"修正"**）
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
