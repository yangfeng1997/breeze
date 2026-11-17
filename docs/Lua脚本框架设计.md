# Lua 脚本框架设计（`src/lua`）

**状态：设计草案，框架层尚未实现。** 绑定库 `base/lua_binding` 已实现，本文只讨论“业务脚本怎么组织、怎么加载、怎么热更、主线程和 worker 怎么分工”。

绑定库的 API、错误模型、线程规则见 [Lua绑定库设计.md](Lua绑定库设计.md)。两层职责不同：

- **绑定库**：一个线程里的 `lua_State`、`Reference`、类型转换、类绑定、`Result`；
- **脚本框架**：应用模块生命周期、主线程脚本、worker 线程、任务队列、目录约定、热更策略。

---

## 1. 目标与边界

### 1.1 目标

- 每个进程有一个主线程 `LuaRuntime`（`LuaState` + `LuaLoader`），由 `LuaModule` 持有；
- 服务可以按需创建 `AsyncWorkerModule`，worker 线程里各有一个独立的 `LuaRuntime`（独立 `LuaState` + `LuaLoader`）；
- 主线程和 worker 的 Lua 代码、C++ 绑定、生命周期、热更完全隔离；
- Lua 业务代码按服务和 worker 类型分目录组织；
- 文件级热更、C++ 引用刷新有明确归属；
- **进程是 shard、部署、故障、资源的基本单位**；不引入进程内多 Shard 模型。

### 1.2 非目标

- 不在绑定库里实现模块加载、mtime、热更、线程池；
- 不做 C++ 原生代码热更；
- **不做单进程多 Shard**（见 §2.3）；一个进程只有一个主线程 `LuaState`；
- 不实现协议/codec/网络库；
- 不把 worker 做成第二个 `IModule` 实例来共享 Lua 状态。

### 1.3 依赖关系

```text
业务服务 C++ / Lua
        ↑
LuaModule / AsyncWorkerModule      ← 应用模块（框架层）
        ↑
LuaRuntime / LuaLoader             ← 每线程运行环境与模块加载（框架层）
        ↑
base/lua_binding                   ← 绑定库
        ↑
Lua 5.5.1
```

---

## 2. 总体架构

```text
App / Service
  │
  ├─ LuaModule (IModule)                       主线程
  │    ├─ LuaRuntime                           一个主线程运行环境
  │    │    ├─ LuaState                        一个主线程 state
  │    │    └─ LuaLoader                       主线程模块缓存/热更
  │    └─ on_start / on_tick / on_stop
  │
  └─ AsyncWorkerModule (IModule, 可选)
       ├─ WorkerGroup "ai"    (threads = 4)
       │    ├─ worker 0: LuaRuntime (LuaState + LuaLoader)
       │    ├─ worker 1: LuaRuntime (LuaState + LuaLoader)
       │    └─ ...
       └─ WorkerGroup "path"  (threads = 2)
            ├─ worker 0: LuaRuntime (LuaState + LuaLoader)
            └─ worker 1: LuaRuntime (LuaState + LuaLoader)
```

### 2.1 两条硬规则

1. **一个 OS 线程只能拥有并使用一个 `LuaState`**；state 在该线程创建、使用、销毁。
2. **主线程 state 和 worker state 不共享任何 Lua 对象**：不传 `Reference` / `LuaTable` / `LuaFunction`，只传任务 ID、函数名、序列化数据。

### 2.2 `LuaModule`、`LuaRuntime`、`LuaLoader` 的分工

| 类型 | 层级 | 职责 |
|---|---|---|
| `LuaModule` | 应用模块（`IModule`） | 主线程生命周期的 `Init/Start/Stop/Shutdown`，持有主线程 `LuaRuntime` |
| `LuaRuntime` | 普通对象（非 `IModule`） | 一个线程里的 Lua 运行环境：`LuaState` + `LuaLoader` + bindings + Start/Stop/Reload/Call |
| `LuaLoader` | 普通对象（非 `IModule`） | 该 state 的模块加载/缓存/依赖/热更 |

`LuaModule` 是应用模块，具有进程级生命周期和单例语义；worker 线程需要的是“一个线程里的 Lua 运行时”，不是新的应用模块。因此：

- 主线程：`LuaModule` 持有一个 `LuaRuntime`；
- worker 线程：每个线程在内部创建一个 `LuaRuntime`；
- `AsyncWorkerModule` 是线程池/队列的管理者，不嵌套 `LuaModule`，也不共享 `LuaRuntime`。

### 2.2.1 为什么引入 `LuaRuntime`

主线程和每个 worker 都是同一个固定配对：

```text
LuaState + LuaLoader
```

而且都要重复做：

- 配 `package.path`；
- 创建 state；
- 创建 loader；
- 注册该线程的 C++ bindings；
- 加载 root；
- `Call` / `Get` 转发；
- reload。

把这一组收进一个**很薄的** `LuaRuntime`，比在每个 worker 线程里手写两三个局部变量更不容易出错：

```text
LuaRuntime
  ├─ LuaState       先构造，后析构
  ├─ LuaLoader      后构造，先析构
  └─ bindings
```

必须守住的边界：

- `LuaRuntime` **不是** `IModule`；
- 不管应用模块的 `Init/Start/Stop/Shutdown`；
- 不管 worker 线程池、job 队列、任务路由；
- 不做多 Shard；
- 不跨线程共享；
- 不负责类实例迁移，只调用 loader 的 reload / `__reload` 钩子。

`LuaModule` 是应用模块；`LuaRuntime` 是一个线程里的 Lua 运行环境；`LuaLoader` 是这个 runtime 的模块缓存/热更器。三层各管一件事，不互相越界。

### 2.3 明确不做单进程多 Shard（架构决策）

本设计**明确不做**“一个进程内多个独立游戏世界 Shard，每个 Shard 一个主 `LuaState`/事件循环”的模型。

这不是“暂缓实现”，而是**不预留该抽象**：不提供每 Shard 一个 `LuaModule` 的容器，不提供进程内 Shard 路由，不提供“先按单 Shard 写、以后加 Shard”的兼容层。否则代码会同时背上进程内路由和跨进程路由两套复杂度。

当前模型固定为：

- 一个进程一个主线程；
- 一个主线程一个 `LuaModule`；
- 一个主线程一个主 `LuaState`；
- worker 的 state 数 = 实际 Lua worker 线程数；
- 多 Shard 通过**多开进程/多服务实例**实现，不在进程内切分。

#### 为什么绝不做

1. **跨 Shard 通信并不能真正省掉 IPC**

   - 一个集群里还有 router、gateway、db、cache、跨服、中心服务；
   - 这些本来就要走网络/IPC；
   - 单进程多 Shard 只优化“恰好同进程的那部分 Shard”，整体仍然需要完整的网络层、序列化、服务发现；
   - 为了局部优化，引入第二套进程内 Shard 路由，得不偿失。
2. **部署复杂度不会降低，反而可能翻倍**

   - 进程编排（systemd/K8s/supervisor）已经是成熟能力；
   - 单进程多 Shard 仍然要处理服务发现、路由、滚动发布；
   - 进程内还要额外处理 Shard 分配、健康检查、故障隔离；
   - 最终得到“外部服务路由 + 进程内 Shard 路由”两套体系，不是一套。
3. **共享 C++ 数据只是局部收益**

   - 只有同进程、同 Shard 组的数据能直接共享；
   - 跨进程、跨机器仍然要序列化；
   - 共享内存还要额外处理生命周期、锁、一致性和崩溃恢复；
   - 不能因此省掉跨进程协议。
4. **故障域变大**

   - 单进程多 Shard 共享进程内存、GC、文件句柄、C++ 运行时；
   - 一个 Shard 的 Lua panic、C++ 崩溃、OOM 会带走所有 Shard；
   - 进程级 Shard 的爆炸半径只有一个 Shard，kill/restart 语义简单。
5. **滚动更新和热更粒度更差**

   - C++ 更新本来就需要重启进程；
   - 进程级 Shard 可以按进程灰度、暂停、回滚；
   - 单进程多 Shard 改 C++ 必须整进程重启，所有 Shard 一起中断；
   - 即使只做 Lua 热更，进程内多 Shard 还要处理 Shard 间版本不一致、全局资源重置、协程/定时器归属；
   - 进程级 Shard 可以把“发布单位 = 故障单位 = 资源单位 = 热更单位”统一起来。
6. **资源收益很小**

   - 少几个进程省下的地址空间/启动开销，相对实体状态、Lua state、网络缓冲、GC 来说很小；
   - 只有小规模/单机/嵌入式部署才比较明显；
   - 对大型游戏服务器，这点资源换故障域和更新粒度，不划算。
7. **大规模集群需要的是进程级可控性**

   - 进程是调度、扩缩容、限流、监控、灰度、回滚的天然单位；
   - 进程内 Shard 需要自己实现一套调度和生命周期，成熟度不如进程编排；
   - 大型集群的稳定性来自“单位清晰、故障可控”，不是来自“少几个进程”。
8. **和我们的目标不匹配**

   - 本项目目标是高性能 C++/Lua 游戏服务器：C++ 主导、Lua 业务、需要稳定热更和滚动更新；
   - 这些需求指向“进程级 Shard + 每进程一个主 LuaState + 可选 worker 线程”；
   - 单进程多 Shard 只适合 skynet 式 actor/service 运行时、极小规模单体、或跨 Shard 交互极端密集且能接受单进程故障域的特殊场景；这些都不是本项目目标。

#### 对比


| 需求          | 单进程多 Shard                     | 一进程一 Shard（本设计）   |
| --------------- | ------------------------------------ | ---------------------------- |
| 跨 Shard 通信 | 部分本地，整体仍要网络/IPC         | 统一网络/IPC               |
| 部署/发布     | 外部路由 + 进程内 Shard 路由，两套 | 进程即单位，一套编排       |
| 共享 C++ 数据 | 仅进程内，跨进程仍序列化           | 不承诺跨进程共享           |
| 故障隔离      | 一个进程崩带走所有 Shard           | 只影响一个 Shard           |
| C++ 更新      | 整进程重启，所有 Shard 中断        | 按进程滚动重启             |
| Lua 热更      | 要处理 Shard 间版本/状态一致性     | 每进程独立 reload，可灰度  |
| 资源占用      | 少几个进程                         | 多几个进程，换取隔离和可控 |
| 大规模集群    | 自定义调度，复杂                   | 进程编排，成熟             |

#### 扩容方式

需要更多 Shard 时：

- 多开进程/服务实例；
- 由集群层做服务发现、路由、Shard 分配、滚动发布；
- 每个进程仍然只有一个主 `LuaModule`/主 `LuaState`；
- `AsyncWorkerModule` 仍只做进程内异步任务，不承担 Shard 职责。

如果未来业务真的需要 skynet 式 actor/service 运行时，那是**另一套架构决策**，需要重新设计，不是给当前 `LuaModule` 加一个 `vector<LuaState>` 就能解决的。

---

## 3. 目录结构

```text
src/lua/
  framework/                    -- 所有服务/线程共享的 Lua 框架
    loader.lua                  -- 文件级 reload
    base/                       -- 基础工具、配置、日志
    kernel/                     -- 服务、事件、定时器
    network/                    -- RPC/连接封装
    worker/                     -- worker 公共逻辑

  server/                       -- 各进程服务
    scenesvr/
      main.lua                  -- 主线程 root
      service.lua               -- 服务对象
      player.lua
      worker/
        ai/
          main.lua              -- ai worker root
          jobs/
        path/
          main.lua              -- path worker root
          jobs/
        common/                 -- 本服务 worker 共享代码

    gateway/
      main.lua                  -- 主线程 root，无 worker
      session.lua

    login/
      main.lua

  base/                         -- 跨服务共享的游戏层
    entity.lua
    component.lua

  config/                       -- Excel 导出的配置
```

### 3.1 路径

```text
package.path = "src/lua/?.lua;src/lua/?/init.lua"
```

示例：

```lua
require("framework.loader")
require("server.scenesvr.service")
require("server.scenesvr.worker.ai.main")
```

### 3.2 放置原则


| 代码                   | 位置                                      |
| ------------------------ | ------------------------------------------- |
| 所有服务共享的框架     | `src/lua/framework/`                      |
| 跨服务共享的游戏逻辑   | `src/lua/base/`                           |
| 某服务主线程专用       | `src/lua/server/<service>/`               |
| 某服务某类 worker 专用 | `src/lua/server/<service>/worker/<type>/` |
| worker 之间共享        | `src/lua/server/<service>/worker/common/` |

不要在 `framework/` 放服务特有逻辑，也不要在服务目录里复制框架代码。

---

## 4. 模块生命周期

### 4.1 `LuaModule`（主线程）

```cpp
class LuaModule : public IModule {
public:
    bool Init() override;       // 保存配置；不加载脚本
    bool Start() override;      // runtime_.Start()：绑定、path、main.lua
    void Stop() override;       // 调用 on_stop、runtime_.Stop()
    void Shutdown() override;   // 释放资源

private:
    LuaRuntime runtime_;        // 主线程 state + loader + bindings
};
```

建议：

- `Init` 只做配置和依赖检查；
- `Start` 顺序由 `LuaRuntime::Start()` 统一完成：
  1. 创建/配置 `LuaState`；
  2. `AddPath(...)`；
  3. 注册主线程 C++ bindings；
  4. 创建 `LuaLoader`；
  5. 加载 `main.lua`；
  6. 调用入口（可选 `on_start`）；
- `Stop` 先调用 Lua 的 `on_stop`，再 `runtime_.Stop()`；
- `Start` 失败要让模块启动失败，不能半启动。

### 4.2 `AsyncWorkerModule`（worker）

```cpp
class AsyncWorkerModule : public IModule {
public:
    bool Init() override;       // 解析 worker 配置；不启动线程
    bool Start() override;      // 启动线程，等待全部 ready
    void Stop() override;       // 停止、join
    void Shutdown() override;   // 释放队列/配置

private:
    struct Worker {
        JobQueue queue;          // 每个 worker 自己的队列，不共享
        std::jthread thread;
    };

    struct WorkerGroup {
        std::string name;
        std::string script;
        BindingProfile bindings;
        std::vector<Worker> workers;
    };

    std::vector<WorkerGroup> groups_;
    std::atomic<bool> stopping_{false};
};
```

worker 线程入口：

```cpp
void AsyncWorkerModule::WorkerMain(WorkerGroup& group, Worker& worker) {
    // 1. 在本线程创建 LuaRuntime（内部创建 LuaState + LuaLoader）
    LuaRuntime runtime({
        .name = group.name,
        .root = group.script,
        .package_path = "src/lua/?.lua;src/lua/?/init.lua",
        .register_bindings = [&](LuaStateView lua) {
            group.bindings.Apply(lua);
        },
    });

    // 2. runtime 负责 path、绑定、loader、加载 worker root
    if (!runtime.Start()) {
        MarkWorkerFailed(worker);
        return;
    }

    // 3. 处理任务
    while (!stopping_) {
        auto job = worker.queue.Pop();
        if (!job) break;

        if (job->kind == JobKind::Reload) {
            if (!runtime.Reload().Ok()) {
                LogReloadError(group.name);
            }
            continue;
        }

        runtime.Call<void>("on_job", job->id, job->payload);
    }
}   // loader、state 在这里依次析构，lua_close 在本线程
```

热更广播：

```cpp
void AsyncWorkerModule::BroadcastReload() {
    for (auto& group : groups_) {
        for (auto& worker : group.workers) {
            worker.queue.Push(Job{JobKind::Reload});
        }
    }
}
```

### 4.3 启动/停止顺序

```text
Init:
  LuaModule.Init
  AsyncWorkerModule.Init（只解析配置）

Start:
  LuaModule.Start（主线程脚本先起）
  AsyncWorkerModule.Start（worker 起，等待 ready）

Stop:
  AsyncWorkerModule.Stop（先停 worker，join）
  LuaModule.Stop（再停主线程）

Shutdown:
  逆序释放
```

原因：

- worker 不依赖主线程 Lua，但主线程通常先启动便于统一错误处理；
- 停止时 worker 先停，避免业务还在处理任务时主 state 先关闭；
- 所有 worker state 必须在 join 前由 worker 线程自己关闭。

### 4.4 启动失败与超时

- `AsyncWorkerModule::Start` 为每个 worker 记录 ready/failed；
- 等待所有 ready 或超时；
- 任意 worker 加载失败：停止已启动的 worker，`Start` 返回失败；
- 不让模块处于“部分 worker 在跑、部分没起”的状态。

---

## 5. 主线程 Lua

### 5.1 root 约定

`src/lua/server/scenesvr/main.lua`：

```lua
local service = require("server.scenesvr.service")

function on_start()
    service:start()
end

function on_tick(dt)
    service:update(dt)
end

function on_stop()
    service:stop()
end
```

`LuaModule` 通过 `LuaRuntime` 负责：

```cpp
runtime_.Start();                 // 内部 RunFile(main_script_)
runtime_.Call<void>("on_start");
```

帧循环：

```cpp
runtime_.Call<void>("on_tick", dt);
```

### 5.2 入口用名字调用

- 主线程入口优先 `CallGlobal` / `Call("on_tick", ...)`；
- 不要长期缓存 `LuaFunction`；
- 这样 root 重新定义函数后，下一次调用自然生效；
- 必须缓存时（热路径），热更后要重新取引用。

### 5.3 root 只做三件事

1. 配置/require 模块；
2. 定义入口函数；
3. 把服务对象挂到全局或返回表。

不要把业务写进 root。

---

## 6. worker 线程与类型

### 6.1 类型 = WorkerGroup

worker 类型由 `WorkerGroupSpec` 描述：

```cpp
struct WorkerGroupSpec {
    std::string name;          // "ai" / "path" / "io"
    int threads = 1;
    std::string script;        // 该类型的 worker root
    BindingProfile bindings;   // 该类型可用的 C++ API
};
```

示例：

```cpp
workers.Configure({
    {.name = "ai",   .threads = 4,
     .script = "src/lua/server/scenesvr/worker/ai/main.lua",
     .bindings = BindingProfile::WorkerAI},
    {.name = "path", .threads = 2,
     .script = "src/lua/server/scenesvr/worker/path/main.lua",
     .bindings = BindingProfile::WorkerPath},
});
```

含义：

- 每个 group 是一条配置/绑定边界；
- 每个 worker 有自己的线程 + 队列 + `LuaRuntime`/`LuaState`，**不共享队列**；
- 每个 worker 加载该 group 的 `script`；
- 不同 group 可以用不同 C++ 绑定和沙箱；
- 一个 group 的故障不阻塞另一个 group。

### 6.2 一个 worker 的脚本

`src/lua/server/scenesvr/worker/ai/main.lua`：

```lua
local jobs = require("server.scenesvr.worker.ai.jobs")

function on_job(id, payload)
    local ok, result = pcall(jobs.run, payload)
    worker_result(id, ok, result)
end
```

worker 只暴露少量入口，例如：

- `on_job(id, payload)`；
- `on_reload()`（可选）。

不要把主线程的 `on_start` / `on_tick` 放进 worker root。

### 6.3 任务协议

```text
main → worker:
  { kind, job_id, group, func, payload }

kind = normal | reload

worker → main:
  { job_id, ok, result }
```

- `kind=reload` 是热更控制任务；worker 在 job 边界执行，不调用 Lua `on_job`；
- `func` 是 worker root 里的函数名，例如 `"on_job"`；
- `payload` 是字符串/字节/可序列化数据；
- 不传 `LuaFunction` / `LuaTable` / `Reference`；
- 主线程用 `job_id` 查回调：

```lua
callbacks = {}

function submit(payload, cb)
    local id = next_id()
    callbacks[id] = cb
    worker_submit("ai", id, "on_job", payload)
end

function on_worker_result(id, ok, result)
    local cb = callbacks[id]
    callbacks[id] = nil
    if cb then cb(ok, result) end
end
```

### 6.4 任务路由

- 每个 worker 一个队列，提交时必须选一个 `worker.queue`；
- **无状态 worker**：任意 worker 都可以；
- **有状态 worker**：按 key 路由到固定 worker，例如：

```text
worker_index = hash(player_id) % group.workers.size()
worker = group.workers[worker_index]
worker.queue.Push(job)
```

否则同一个玩家的任务可能落到不同 state，`local cache` 不一致。

热更同理：`BroadcastReload()` 必须给**每个** worker 队列各投递一个 `ReloadJob`，不能只往一个共享队列塞一个。

### 6.5 简化方案：一个 root 按 kind 分发

如果类型之间不需要不同 C++ 绑定/沙箱，可以只用一个 group、一个 root：

```lua
local jobs = {
    ai = require("...ai"),
    path = require("...path"),
}

function on_job(id, kind, payload)
    local ok, result = pcall(jobs[kind].run, payload)
    worker_result(id, ok, result)
end
```

优点是简单；缺点是所有类型共享同一组 worker 和绑定（队列仍按 worker 分开），类型之间没有隔离。

### 6.6 每 worker 不同配置（可选）

如果将来需要像 quanta 一样“每个 worker 名字不同、entry 不同”，可以把 `WorkerGroupSpec` 扩展为 per-worker：

```cpp
struct WorkerSpec {
    std::string name;      // ai-0 / ai-1
    std::string script;    // 可覆盖 group 默认
};
```

当前先按 group 配置即可。

---

## 7. C++ 绑定 profile

### 7.1 三类绑定


| profile                        | 主线程 | worker | sandbox |
| -------------------------------- | -------- | -------- | --------- |
| 网络/DB/RPC                    | ✓     | ✗     | ✗      |
| 实体/服务调用                  | ✓     | ✗     | ✗      |
| 定时器                         | ✓     | 可选   | ✗      |
| 纯计算/只读数据                | ✓     | ✓     | ✓      |
| `worker_result` / `worker_log` | ✗     | ✓     | ✗      |
| `os` / `io`                    | ✓     | 慎用   | ✗      |

### 7.2 实现方式

```cpp
struct BindingProfile {
    using ApplyFn = std::function<void(LuaStateView)>;
    ApplyFn apply;

    void Apply(LuaStateView lua) const { apply(lua); }
};
```

- 主线程 profile 注册网络、DB、实体、服务；
- worker profile 只注册 worker 允许的 API；
- sandbox profile 用 `SandboxOptions` + 最小库白名单；
- **不要在 worker 里注册主线程独占 API**，否则 worker 脚本可能访问非线程安全对象。

---

## 8. 文件级热更

### 8.1 归属

热更在框架层，不在绑定库：

- `LuaState` / `CallGlobal` / `LuaFunction` 由绑定库提供；
- `LuaRuntime` 持有一个 state + 一个 loader，暴露 `Start/Stop/Reload/Call`；
- mtime、依赖图、安全点、实例迁移由 `LuaLoader` 提供；
- `LuaRuntime::Reload()` 只是把调用转给它的 `LuaLoader`。

> 命名说明：本文的 `LuaLoader` 是**每个 `lua_State` 一个的模块加载/缓存/热更器**，不创建/拥有 state，也不提供 `Call` / `GetGlobal`；它和 nfshmx 的 `NFILuaLoader`（LuaContext + load/call 封装）不是同一个东西。`NFILuaLoader` 那一层对应本文的 `LuaRuntime`（state + load/call + bindings）。

### 8.2 模块约定

- 业务模块返回一个表；
- 模块函数通过返回表访问状态；
- 不长期缓存其他模块的旧表；
- 类实例通过 `__reload` 迁移；
- 主线程入口按名字调用。

### 8.3 Loader 职责

```text
LuaLoader
  ├─ 记录 name -> { path, mtime, value, deps }
  ├─ import(name) / require 接管
  ├─ 扫描 mtime 变化
  ├─ 依赖图 + 拓扑顺序
  ├─ 按顺序 load/execute
  └─ 失败记录错误，不中断 worker 循环
```

### 8.4 安全点

- 主线程：在一帧结束、Lua 栈为空时 reload；
- worker：在 job 之间 reload；
- 不在 Lua 调用栈中间 reload；
- 不在协程 yield 期间 reload。

### 8.5 主线程 reload

```text
tick:
  on_tick(dt)
  如果到达检查周期:
    runtime_.Reload()               // 内部委托 LuaLoader
  如果 root 入口变了:
    重新 CallGlobal("on_tick", ...)  // 按名字，自动用新函数
```

### 8.6 worker reload

采用 nfshmx/quanta 的轻量方式：

- 每个 worker `LuaRuntime` 有自己的 `LuaLoader`；
- `AsyncWorkerModule::BroadcastReload()` 给**每个 worker 自己的队列**投递一个 `ReloadJob`；
- worker 在 job 边界执行 `runtime.Reload()`，不调用 Lua 业务函数；
- **不暂停 group、不等 in-flight、不做 barrier、不做回滚**；
- reload 失败只记录错误，worker 继续用当前已加载的代码处理后续 job；
- 主线程和 worker 版本允许短暂不一致，但 job 协议要稳定；
- 有状态 worker 需要 `__reload` 或重启 worker；
- 大版本/协议变更：停 group → 重启 worker/进程，而不是在线逐个迁移。

### 8.7 C++ 引用

- 主线程 C++ 缓存的 `LuaFunction` 不会自动更新；
- 热更后重新 `GetFunction`，或入口改成按名字调用；
- worker 的 `LuaFunction` 只存在于 worker state，不跨线程；
- 热更不影响 C++ 类绑定/布局；C++ 变更走重启。

---

## 9. 错误与线程规则

### 9.1 错误

- 所有脚本调用返回 `Result`；
- 主线程错误：记录并返回给调用方；
- worker 错误：返回 `{job_id, ok=false, error}`，不要影响主线程；
- worker 禁止 `CallUnprotected`，避免 longjmp 穿越线程栈；
- panic 默认 abort；worker 应把可捕获错误限制在 pcall 内。

### 9.2 线程

- `LuaRuntime` 必须在 owner 线程创建/使用/销毁；它内部持有 `LuaState` + `LuaLoader`；
- 不在主线程创建 worker `LuaRuntime` 后 move/交给 worker 线程；
- `LuaLoader` 与它所属的 `LuaState` 同生命周期；
- 不跨线程传 `Reference` / `LuaTable` / `LuaFunction`；
- 只传 ID、函数名、序列化数据；
- worker 和主线程共享的 C++ 服务必须线程安全或只读。

---

## 10. 可观测性

至少记录：

- 每个 worker group 的线程数、ready 状态；
- 队列深度、任务延迟、失败数；
- reload 版本、reload 耗时、失败原因；
- 主线程/worker 的 Lua 内存（自定义 allocator 统计）；
- worker panic/异常退出次数；
- 停止时未完成任务数。

---

## 11. 与其他框架对照


| 框架            | worker/并发单元                            | LuaState 归属                                           | 类型/脚本指定                                                  |
| ----------------- | -------------------------------------------- | --------------------------------------------------------- | ---------------------------------------------------------------- |
| **nfshmxframe** | `NFCLuaThreadModule` + work/tcp/loop actor | 每个 `NFCLuaScriptComponent` 一个 `LuaIntf::LuaContext` | 各 actor 共用 `init.lua`，类型差异由 C++ 任务类型/入口函数体现 |
| **quanta**      | `lworker::scheduler` + 多个 `worker`       | 每个 `worker` 一个 `kit_state`                          | 命名 worker + 各自 env/conf；`QUANTA_ENTRY` 可不同             |
| **skynet**      | service 是并发单元                         | 每个 service 一个 `LuaState`                            | 类型 = service 脚本；多实例 = 多 service                       |
| **KBEngine**    | 组件脚本线程（Python）                     | 每个组件一个脚本线程                                    | Base/Cell 组件，不是 worker 类型                               |

本设计的对应关系：


| 本设计              | nfshmx                             | quanta               |
| --------------------- | ------------------------------------ | ---------------------- |
| `LuaModule`         | `NFCLuaThreadModule`（主脚本）     | 主服务 Lua           |
| `AsyncWorkerModule` | `NFCLuaThreadModule` + actor pool  | `lworker::scheduler` |
| `WorkerGroup`       | work/tcp/loop actor 类型           | 命名 worker          |
| worker `LuaRuntime` | `NFCLuaScriptComponent` 的 context | `worker::m_lua`      |

---

## 12. 配置示例

### 12.1 scenesvr

```cpp
struct SceneServiceConfig {
    std::string main_script = "src/lua/server/scenesvr/main.lua";
    std::vector<WorkerGroupSpec> workers = {
        {"ai",   4, "src/lua/server/scenesvr/worker/ai/main.lua",
         BindingProfile::WorkerAI},
        {"path", 2, "src/lua/server/scenesvr/worker/path/main.lua",
         BindingProfile::WorkerPath},
    };
};
```

### 12.2 gateway

```cpp
struct GatewayConfig {
    std::string main_script = "src/lua/server/gateway/main.lua";
    std::vector<WorkerGroupSpec> workers = {};   // 没有 worker
};
```

gateway 不创建 `AsyncWorkerModule`，或创建但 groups 为空。

### 12.3 配置文件形式

也可以从 Lua/JSON 配置读：

```lua
return {
    main_script = "src/lua/server/scenesvr/main.lua",
    worker_groups = {
        { name = "ai",   threads = 4, script = "src/lua/server/scenesvr/worker/ai/main.lua" },
        { name = "path", threads = 2, script = "src/lua/server/scenesvr/worker/path/main.lua" },
    },
}
```

C++ 只负责把配置转成 `WorkerGroupSpec`。

---

## 13. 测试计划

- `LuaRuntime` 构造/析构顺序（`LuaLoader` 先于 `LuaState` 析构）；
- `LuaModule` 启动/停止/重复 Start 失败；
- 主线程 root 加载失败时模块启动失败；
- 每个 worker 线程的 `LuaRuntime`/`LuaState` 不同（脚本里打印/记录 state 地址）；
- worker 之间、主线程与 worker 之间的 `LuaLoader` 缓存互不影响；
- 主线程和 worker 不共享全局变量；
- worker group 路由正确（有状态按 key 路由）；
- 队列满时的背压/拒绝；
- worker 加载失败记录错误，不阻塞其他 worker；
- worker job 错误返回主线程，不影响其他 worker；
- 主线程 reload、worker reload；reload 失败只记录错误，worker 继续处理 job；
- 每个 worker 的队列独立，`ReloadJob` 能到达每个 worker；
- 停止顺序：worker join 后再关主 state；
- asan/tsan 下无跨线程引用。

---

## 14. 非目标与后续

### 14.1 明确不做

- **单进程多 Shard**（明确架构决策，见 §2.3；扩容靠多进程/多服务实例）；
- C++ 原生热更；
- worker 动态扩缩容（先固定线程数）；
- 绑定库内的模块加载/热更；
- 跨线程 Lua 引用。

### 14.2 后续可加

- worker group 动态扩缩容；
- per-worker 脚本/env（quanta 命名 worker 风格）；
- 更细的 binding profile/权限；
- worker 任务优先级/隔离；
- loader 的依赖图与实例迁移完善。

---

## 15. 文件清单（设计中的框架层）

```text
src/cpp/.../lua_module.h/.cpp          LuaModule（IModule，主线程）
src/cpp/.../lua_runtime.h/.cpp         LuaRuntime（state + loader + bindings）
src/cpp/.../lua_loader.h/.cpp          LuaLoader（C++ 实现；也可用 Lua 实现）
src/cpp/.../async_worker_module.h/.cpp AsyncWorkerModule（IModule，worker pool）
src/cpp/.../binding_profile.h          main/worker/sandbox 绑定集合
src/cpp/.../worker_group.h             WorkerGroupSpec/队列/路由
src/lua/framework/loader.lua           LuaLoader 的 Lua 实现（二选一）
src/lua/server/<service>/main.lua      主线程 root
src/lua/server/<service>/worker/<type>/main.lua
```

具体实现放框架层，不放进 `base/lua_binding`；绑定库只暴露 `LuaState`、`Result`、`RunFile`、`CallGlobal`、`LuaFunction`。
