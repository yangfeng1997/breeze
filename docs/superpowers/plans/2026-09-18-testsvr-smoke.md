# testsvr 冒烟服务实施计划

依据：`docs/superpowers/specs/2026-09-18-testsvr-smoke-design.md`

每个任务做完就跑一次该任务的验证命令，绿了再进下一个。**不要一次写完再验证**——
这片代码的价值全在"顺序和退出"上，攒到最后一起调会分不清是哪一层错了。

## 任务一览

| # | 产出 | 验证 |
|---|---|---|
| 1 | `internal/smoke/smoke.go`（beat 骨架） | `go build ./...` |
| 2 | `ticker.go` / `heartbeat.go` / `worker.go` | `go build ./...` |
| 3 | `smoke_test.go`（两个用例） | `go test ./...` |
| 4 | `main.go`（装配与信号） | `go build` + `go vet` |
| 5 | 端到端手工冒烟 | 看日志顺序 + Ctrl+C |
| 6 | 全量回归 | 四条命令全绿 |

任务 3 排在 4 前面是故意的：先把并发退出契约用自动化测试钉住，再上进程装配。
这样任务 5 手工冒烟若出问题，能确定是信号接线的问题，不是模块本身的问题。

---

## 任务 1：beat 骨架

**新建** `src/server/testsvr/internal/smoke/smoke.go`

包名 `smoke`。内容按 spec 的「beat 骨架」一节：

- `beat` 结构体：`app` / `name` / `step` / `extra` / `done` / `exited` / `stopOnce` / `wg` / `mu` / `n`
- `newBeat(a *app.App, name string, step time.Duration, extra func() []any) beat`
  —— `done` 和 `exited` 都在这里创建
- `Name()` / `App()` 取值；`DependsOn()` 返回 `nil`；`Init()` 返回 `nil`
- `Start()`：`wg.Add(1)` + goroutine，select 三分支（`done` 主路径 / `Context()` 兜底 / 定时器）
- `Stop()`：`stopOnce.Do(func() { close(b.done) })`
- `Shutdown()`：`wg.Wait()`
- `tick()`：加锁自增，拼 `attrs`，`extra` 非 nil 就 append，最后 `Logger().Info("tick", attrs...)`

**三个容易写错的地方**，写完对一遍：

1. goroutine 里两个 defer 的顺序必须是 `defer b.wg.Done()` 在前、`defer close(b.exited)` 在后。
   LIFO 使得 `exited` 先关——任务 3 的断言依赖这个顺序。
2. `tick()` 里读 `n` 必须先解锁再打日志，不要持锁调 `Logger()`。
3. `beat` 自己不实现 `Module`（`DependsOn`/`Init` 返回零值也只是为了被 embed），
   真正的 `DependsOn`/`Init` 由具体模块覆盖。

**验证**：`go build ./...` 通过（此时包还没人用，能编译即可），`gofmt -l .` 无输出。

---

## 任务 2：三个模块

**新建** `ticker.go`、`heartbeat.go`、`worker.go`，都在 `internal/smoke/` 下。

每个文件都是「内部构造函数 + 导出工厂」两层，见 spec 的「模块的构造：两层」：

```go
func newTicker(a *app.App, step time.Duration) *ticker
func NewTicker(a *app.App) app.Module   // 调 newTicker(a, 500*time.Millisecond)
```

- **ticker.go** — `ticker` 嵌 `beat`；`count() int` 带锁读 `n`；`extra` 传 `nil`
- **heartbeat.go** — 同上，生产周期 `700ms`，无 `count()`
- **worker.go** — `worker` 嵌 `beat` 且带 `ticker *ticker` 字段；`newWorker` 里挂
  `extra` 闭包读 `w.ticker.count()`；覆盖 `DependsOn() → []string{"ticker"}`；
  `Init()` 用 `a.GetModule("ticker")` + 类型断言 `m.(*ticker)`

**验证**：`go build ./...` 通过，`gofmt -l .` 无输出。

此时还没有测试也还没有 main，编译通过就够——行为正确性在任务 3 验。

---

## 任务 3：测试

**新建** `src/server/testsvr/internal/smoke/smoke_test.go`，包名 `smoke`（内部测试，
不是 `smoke_test` 外部包——要用到未导出的类型和构造函数）。

先写一个共用的小助手：

```go
// waitFor 轮询直到 cond 为真，超时则 Fatal。
func waitFor(t *testing.T, what string, cond func() bool)
```

用 1 秒 deadline、1ms 间隔。**不要用固定 `sleep`**。

### `TestWorkerReadsTicker`

```go
a := app.New("smoke-test")
tk := newTicker(a, 5*time.Millisecond)
wk := newWorker(a, 5*time.Millisecond)
a.Register(func(*app.App) app.Module { return tk })
a.Register(func(*app.App) app.Module { return wk })

// Build → Init → Start
waitFor(t, "ticker 打点", func() bool { return tk.count() > 0 })
waitFor(t, "worker 打点", func() bool { return wk.count() > 0 })
// Stop + Shutdown 收尾
```

断言点：`Init` 阶段 worker 能拿到 ticker。若拿不到，`Init` 返回错误 → `Build/Init`
直接失败 → 用例走不到断言，也算失败。

### `TestShutdownStopsGoroutines`

`Build → Init → Start` 后，**先 `waitFor` 确认两个 goroutine 都真在跑**
（`tk.count() > 0 && wk.count() > 0`），再 `Stop()` + `Shutdown()`，然后非阻塞断言：

```go
select {
case <-tk.exited:
default:
	t.Fatal("Shutdown 返回后 ticker 的 goroutine 仍在运行")
}
```

先确认"在跑"再断言"停了"——否则 goroutine 根本没起来，用例也会通过。

**验证**：`go test ./src/server/testsvr/... -v` 两个用例 PASS，且 `go test ./...`
里 `app` 包原有 16 个用例不受影响。

可选：`go test -race ./src/server/testsvr/...`。Windows 上 `-race` 需要 cgo 与 gcc，
装了就跑，没装跳过即可，不要为它去装工具链。

---

## 任务 4：main.go

**新建** `src/server/testsvr/main.go`，包名 `main`，内容按 spec 的「main.go」一节。

- `signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)` + `defer stop()`
- `app.New("testsvr")`
- 三个 `Register`，**顺序必须是 worker → heartbeat → ticker**（这是 spec 里设计过的，
  用来同时演示「有依赖被重排」和「无依赖保持注册序」）
- `a.Run(ctx)` 返回非 nil 时 `slog.Error` + `os.Exit(1)`

**验证**：`go build ./...`、`go vet ./...` 通过。

---

## 任务 5：端到端手工冒烟

**先编译成二进制再跑，不要用 `go run`。**

```bash
go build -o build/testsvr.exe ./src/server/testsvr
./build/testsvr.exe
```

`go run` 会把 Ctrl+C 同时发给它自己和子进程，两者抢着退出，你看到的可能不是应用
自己的优雅退出路径——退出码也不可信。编译出二进制，信号就只打在应用上。

**检查三件事**：

1. 启动后日志里有 `app built ... modules=3`，且 `module inited` 出现顺序为
   **`heartbeat → ticker → worker`**（worker 注册在最前却排到最后）。
2. 三个模块的 `INFO tick` 交错出现，worker 那行带 `ticker=N` 字段且 N 在增长。
3. 按 Ctrl+C：
   - `module stopped` 出现的顺序是 **`worker → ticker → heartbeat`**
   - 紧接着 `module shutdown` 也是同一逆序
   - 进程退出，**退出码 0**（`echo $?` / `$LASTEXITCODE`）
   - 没有挂起、没有 panic

**这一步无法脚本化**：Windows 上没法给另一个进程发 Ctrl+C（`taskkill /F` 是强杀，
走不到信号路径），所以只能人眼看。任务 3 的测试已经把 Stop/Shutdown 的语义钉住了，
这里验的是**信号接线**这一段——`signal.NotifyContext` 有没有真的把 Ctrl+C 接到 ctx 上。

实施时试过用 `GenerateConsoleCtrlEvent` 从一个助手进程真发 Ctrl+C：API 返回成功，
但 testsvr 毫无反应、继续打点。所以那条路在非交互环境下无效，**不要以为它验过了**——
这一条只能由人在真实终端里按 Ctrl+C 确认。

---

## 任务 6：全量回归

```bash
gofmt -l .                              # 无输出
go build ./...                          # 通过
go vet ./...                            # 通过
go test -count=1 ./...                  # 全绿，app 包 16 个用例 + smoke 2 个
```

对照 spec 的「验收标准」五条逐条确认。

---

## 已知限制（不要试图在本计划内解决）

- Windows 上收不到 `syscall.SIGTERM`，只有 Ctrl+C（`os.Interrupt`）。保留 SIGTERM 是为
  将来上 Linux，现在验不了。
- 没有退出超时：`Module.Shutdown` 没有 ctx 参数，模块卡住就永远等下去。`app` 包的注释
  已记录，等真实模块出现再补。
- testsvr 不参与 `startall.sh`（该脚本不存在）。
- `docs/` 与整个项目都不是 git 仓库，本计划与 spec 只落盘，没有版本历史。
