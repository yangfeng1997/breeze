# CLAUDE.md

本文件是 `src/core/app/` 的局部索引。改进程骨架、模块接口或生命周期顺序前，先读本文件。

## 上级入口

- [../../../CLAUDE.md](../../../CLAUDE.md)
- [../CLAUDE.md](../CLAUDE.md)

## 目录定位

- Go package：`app`，import 路径 `project/src/core/app`。
- 进程级骨架：模块注册 → 依赖拓扑排序 → 生命周期编排（含失败回滚）。
- 只做编排：不监听信号、不做 RPC 装配，只依赖标准库。`Run` 会阻塞等调用方给的
  ctx，但信号由各 `src/server/*svr` 的 `main.go` 用 `signal.NotifyContext` 提供 ——
  框架里没有 `os/signal`。

## 主要文件

- [`app.go`](app.go) — `App` 结构体、`New`、`Option`、`Build`、`Run`，以及 `depsSort` 依赖排序
- [`module.go`](module.go) — `Module` 接口（模块方实现的契约）
- [`lifecycle.go`](lifecycle.go) — `lifecycleState` 与 Init/Start/Stop/Shutdown 及回滚
- [`errors.go`](errors.go) — 哨兵错误与错误串组装助手

## 快速读法

- 查装配与校验看 `Build`：工厂在此才被调用，重名/缺依赖/成环都在这里报。
- 查生命周期看 `Init` / `Start` / `Stop` / `Shutdown`。
- 查启动失败怎么收拾看 `rollbackInit`（Init 失败）与 `rollbackStart`（Start 失败）。
- 查"为什么顺序是这样"看 [`app.go`](app.go) 的 `depsSort`，以及 `app_test.go` 里的顺序断言。

## 工作规则

- **`App` 是具体结构体，不是接口**。生命周期只有这一种实现，模块集合的差异由各 svr
  注册的 `Module` 决定。接口只用在有真实多实现的地方 —— 本包里的那一处是 `Module`。
  不要为 `App` 抽接口，也不要加 `I` 前缀。
- **绝不可以在持有 `App.mu` 的情况下调用模块方法**：模块回调里会调
  `App().GetModule(...)`，那会重新加锁并自锁。加锁只用于读写 `state` / `modulesArr` / `modulesMap`。
- **只有 `lifecycleState` 一个生命周期枚举，没有逐模块状态**。模块状态是由 `lifecycleState` 加上
  "Init 全部跑完才开始 Start"这条不变量推出来的：已启动的必然是 `modulesArr[0, i)` 前缀，
  只 Init 未启动的必然是 `(i, n)` 后缀。**若将来改成边 Init 边 Start 的交错生命周期，
  这个前提就不成立，回滚必须改回按模块记账** —— `lifecycle.go` 的 `rollbackStart`
  注释里也写明了这一点。
- **Start 返回错误的模块，框架不得对它调用 Stop/Shutdown**。`Module.Start` 的注释约定
  该模块返回错误前必须自清理，再补一刀会二次释放。`app_test.go` 的
  `TestStartFailureRollback` 钉住了这条。
- **关机分两趟**：所有模块先逆序收到 `Stop` 信号，再逆序 `Shutdown` 等待。正常路径
  （`Stop()` 然后 `Shutdown()`）与失败回滚都是这个形状，总耗时取决于最慢的模块而不是
  各模块之和。不要改成逐模块 `Stop+Shutdown`。
- **`Shutdown` 必须能处理"只 Init 过、从未 Start"的状态**，因为 Init 失败与 Start 失败
  都会走到这里。
- **一切 `Shutdown` 调用之前，进程 ctx 必须已经 cancel**。正常路径靠 `Stop()` 末尾，
  **回滚路径（`rollbackInit` / `rollbackStart`）必须自己先调 `a.cancel()`** ——
  这里曾经漏过，后果是模块的 `Shutdown()` 等一个永远不会 done 的 ctx，直接卡死。
  三条路径分别由 `TestStopCancelsContextBeforeShutdown` /
  `TestInitRollbackCancelsContext` / `TestStartRollbackCancelsContext` 钉住。
  **新增任何调用 `Shutdown` 的路径，都要先确认 cancel 已经发生。**
- 顺序语义（"无依赖按注册顺序初始化"、"Stop/Shutdown 严格逆序"）靠测试固定，
  改 `depsSort` 或回滚顺序时必须同步 `app_test.go` 的断言。
- `Module` 接口的签名与注释由调用方约定，改动前先确认所有 `*svr` 模块实现。
