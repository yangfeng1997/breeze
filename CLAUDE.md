# CLAUDE.md

`breeze` 的全局索引。改动跨包约定、目录职责或构建链前先读本文件；
`src/core/app/` 的细节见 [src/core/app/CLAUDE.md](src/core/app/CLAUDE.md)。

## 项目

游戏服务端（GameServer）。Go 1.25，module `project`，远端 `yangfeng1997/breeze`。
`src/core/app` 是自研进程骨架，各 `*svr` 是它的消费者。

## 目录

- `src/core/app` — 进程骨架：模块注册 → 拓扑排序 → 启动编排 → 事件驱动主循环。
  **只负责启动，不负责停机** —— 没有任何 Stop/Shutdown/信号处理
- `src/server/*svr` — 服务进程，`main` 只做装配，实现放 `internal/`
- `docs/` — 设计文档。服务间通信的基线是
  [docs/Actor模型与RPC设计.md](docs/Actor模型与RPC设计.md)：实现 RPC 或 actor 层前先读它
- `scrpits/` — Windows 环境脚本（**目录名拼写如此，勿"修正"**）
- `build/`、`run/` — 构建产物，不进版本库

## 服务

已实现的只有 `testsvr`：不做业务，是其余 svr 的**装配模板**
（见 [src/server/testsvr/main.go](src/server/testsvr/main.go)）。

`battlesvr` / `dbsvr` / `gatesvr` / `lobbysvr` / `loginsvr` / `matchsvr` /
`namesvr` / `onlinesvr` / `routeagent` / `routesvr` / `src/pkg` 都是**空目录**。
git 不追踪空目录 —— clone 后不存在，只表示规划中的拆分。

## 构建链（未完成）

`scrpits/setup_windows_env.ps1` 负责装环境：git / Python+PyYAML / Go / protoc /
protoc-gen-go / .NET runtime（Luban 用）。etcd、redis、mongo 复用远端
`tencent.codepeak.cn`，不装本地。

脚本尾注给出的构建命令：

```
python scripts/config.py --env dev --world-id 2 --out run
python scripts/build.py --out run --build build
cd run && ./startall.sh
```

**这三个入口都还不存在** —— `scrpits/` 下目前只有 setup 脚本本身，构建链待补。

## 注释规范

一律简体中文；极简、精准、有参考性；禁止成片冗余。

只写代码读不出的东西 —— 约束、代价、反直觉点、失败模式，并附可跳转锚点
（文件、符号、测试名）。不复述代码逻辑，不写过渡句与总结段。
[`src/core/app/CLAUDE.md`](src/core/app/CLAUDE.md) 与 `testsvr/main.go` 是本规范的范本。
