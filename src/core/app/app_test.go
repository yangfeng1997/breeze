package app

import (
	"context"
	"errors"
	"log/slog"
	"slices"
	"testing"
)

// recorder 按调用先后记录 "模块名.阶段"，测试据此断言精确序列。
type recorder struct {
	calls []string
}

func (r *recorder) add(module, step string) {
	r.calls = append(r.calls, module+"."+step)
}

func (r *recorder) snapshot() []string {
	return slices.Clone(r.calls)
}

// spec 描述一个待注册的假模块。
type spec struct {
	name     string
	deps     []string
	initErr  error
	startErr error
}

// fakeModule 只记账、不做实事。
type fakeModule struct {
	name     string
	app      *App
	deps     []string
	rec      *recorder
	initErr  error
	startErr error

	// ctxCanceledAtShutdown 记录 Shutdown 被调用时进程 ctx 是否已经 cancel。
	// 框架承诺在一切 Shutdown 之前 cancel，否则模块的 Shutdown 等待自有 goroutine
	// 时会永久阻塞——那个 goroutine 可能正 select 在 App.Context() 上。
	ctxCanceledAtShutdown bool

	// shutdownCtxErr / shutdownCtxHasDeadline 记录 Shutdown 收到的预算 ctx 的状态。
	// 它必须与 App.Context() 相反：一个是"还剩多少预算"，一个是"进程正在退出"。
	shutdownCtxErr         error
	shutdownCtxHasDeadline bool
}

func (m *fakeModule) Name() string        { return m.name }
func (m *fakeModule) App() *App           { return m.app }
func (m *fakeModule) DependsOn() []string { return m.deps }

func (m *fakeModule) Init() error {
	m.rec.add(m.name, "Init")
	return m.initErr
}

func (m *fakeModule) Start() error {
	m.rec.add(m.name, "Start")
	return m.startErr
}

func (m *fakeModule) Stop() { m.rec.add(m.name, "Stop") }

func (m *fakeModule) Shutdown(ctx context.Context) {
	m.rec.add(m.name, "Shutdown")
	m.ctxCanceledAtShutdown = m.app.Context().Err() != nil
	m.shutdownCtxErr = ctx.Err()
	_, m.shutdownCtxHasDeadline = ctx.Deadline()
}

// harness 按 spec 顺序注册假模块，并保留实例供查询。
type harness struct {
	rec   *recorder
	mods  map[string]*fakeModule
	specs []spec
}

func newHarness(specs ...spec) *harness {
	return &harness{rec: &recorder{}, mods: make(map[string]*fakeModule), specs: specs}
}

// newApp 造一个 App 并把 spec 全部注册进去。日志丢弃，避免污染测试输出。
func (h *harness) newApp(name string) *App {
	a := New(name, WithLogger(slog.New(slog.DiscardHandler)))
	for _, s := range h.specs {
		a.RegisterModule(func(app *App) Module {
			m := &fakeModule{
				name:     s.name,
				app:      app,
				deps:     s.deps,
				rec:      h.rec,
				initErr:  s.initErr,
				startErr: s.startErr,
			}
			h.mods[s.name] = m
			return m
		})
	}
	return a
}

// started 走完 Build→Init→Start，测试里最常见的开场。
func (h *harness) started(t *testing.T, name string) *App {
	t.Helper()
	a := h.newApp(name)
	if err := a.Build(); err != nil {
		t.Fatalf("Build() error = %v, want nil", err)
	}
	if err := a.Init(); err != nil {
		t.Fatalf("Init() error = %v, want nil", err)
	}
	if err := a.Start(); err != nil {
		t.Fatalf("Start() error = %v, want nil", err)
	}
	return a
}

func assertCalls(t *testing.T, got, want []string) {
	t.Helper()
	if !slices.Equal(got, want) {
		t.Errorf("calls = %q, want %q", got, want)
	}
}

func TestLifecycleOrder(t *testing.T) {
	// lobby 依赖 db，db 注册在 lobby 之后 —— 拓扑序必须把 db 提到 lobby 前面，
	// 而 gate 与 db 互不依赖，保持注册序。
	h := newHarness(
		spec{name: "gate"},
		spec{name: "lobby", deps: []string{"db"}},
		spec{name: "db"},
	)
	a := h.started(t, "test")
	a.Stop()
	a.Shutdown()

	assertCalls(t, h.rec.snapshot(), []string{
		"gate.Init", "db.Init", "lobby.Init",
		"gate.Start", "db.Start", "lobby.Start",
		"lobby.Stop", "db.Stop", "gate.Stop",
		"lobby.Shutdown", "db.Shutdown", "gate.Shutdown",
	})
}

func TestGetModule(t *testing.T) {
	h := newHarness(
		spec{name: "a"},
		spec{name: "b", deps: []string{"c"}},
		spec{name: "c"},
	)
	a := h.newApp("test")
	if err := a.Build(); err != nil {
		t.Fatalf("Build() error = %v, want nil", err)
	}

	if m := a.GetModule("c"); m == nil || m.Name() != "c" {
		t.Errorf("GetModule(c) = %v, want the c module", m)
	}
	if m := a.GetModule("nope"); m != nil {
		t.Errorf("GetModule(nope) = %v, want nil", m)
	}
}

func TestFactoryReceivesApp(t *testing.T) {
	h := newHarness(spec{name: "gate"}, spec{name: "db"})
	a := h.newApp("test")
	if err := a.Build(); err != nil {
		t.Fatalf("Build() error = %v, want nil", err)
	}

	if got := h.mods["gate"].App(); got != a {
		t.Errorf("module App() = %v, want the App it was built with", got)
	}
	// 模块应当能用 App() 取到兄弟模块。
	got := h.mods["gate"].App().GetModule("db")
	if got != Module(h.mods["db"]) {
		t.Errorf("App().GetModule(db) = %v, want the db module", got)
	}
}

func TestInitFailureRollback(t *testing.T) {
	errBoom := errors.New("boom")
	h := newHarness(
		spec{name: "a"},
		spec{name: "b", initErr: errBoom},
		spec{name: "c"},
	)
	a := h.newApp("test")
	if err := a.Build(); err != nil {
		t.Fatalf("Build() error = %v, want nil", err)
	}

	err := a.Init()
	if !errors.Is(err, errBoom) {
		t.Errorf("Init() error = %v, want wraps %v", err, errBoom)
	}
	// b 的 Init 已自行清理，框架不碰它；c 从未 Init，也不该被碰。
	assertCalls(t, h.rec.snapshot(), []string{"a.Init", "b.Init", "a.Shutdown"})

	// 回滚后 App 进终态，后续调用全是 no-op。
	a.Stop()
	a.Shutdown()
	assertCalls(t, h.rec.snapshot(), []string{"a.Init", "b.Init", "a.Shutdown"})
}

func TestStartFailureRollback(t *testing.T) {
	errBoom := errors.New("boom")
	h := newHarness(
		spec{name: "a"},
		spec{name: "b"},
		spec{name: "c", startErr: errBoom},
		spec{name: "d"},
	)
	a := h.newApp("test")
	if err := a.Build(); err != nil {
		t.Fatalf("Build() error = %v, want nil", err)
	}
	if err := a.Init(); err != nil {
		t.Fatalf("Init() error = %v, want nil", err)
	}

	err := a.Start()
	if !errors.Is(err, errBoom) {
		t.Errorf("Start() error = %v, want wraps %v", err, errBoom)
	}
	// 逆序回滚，且分两趟走，与正常路径的 Stop→Shutdown 形状一致：
	//   d 只 Init 过，只 Shutdown；c 的 Start 已自行清理，框架不得再碰；
	//   b、a 已 Start，先统统收到 Stop 信号，再逆序等待 Shutdown。
	// 两趟而非逐模块 Stop+Shutdown，是为了让所有模块先并行收到退出信号，
	// 总耗时取决于最慢的模块，而不是各模块耗时之和。
	assertCalls(t, h.rec.snapshot(), []string{
		"a.Init", "b.Init", "c.Init", "d.Init",
		"a.Start", "b.Start", "c.Start",
		"d.Shutdown",
		"b.Stop", "a.Stop",
		"b.Shutdown", "a.Shutdown",
	})

	// 回滚后 App 已是终态，再调 Stop/Shutdown 全是 no-op。
	a.Stop()
	a.Shutdown()
	assertCalls(t, h.rec.snapshot(), []string{
		"a.Init", "b.Init", "c.Init", "d.Init",
		"a.Start", "b.Start", "c.Start",
		"d.Shutdown",
		"b.Stop", "a.Stop",
		"b.Shutdown", "a.Shutdown",
	})
}

// cancelOnStart 在 Start 里取消外部 ctx，用来确定性地结束 Run 的等待阶段，
// 不依赖 sleep，也不需要把 Run 丢进 goroutine。
type cancelOnStart struct {
	app    *App
	rec    *recorder
	cancel context.CancelFunc
}

func (m *cancelOnStart) Name() string        { return "canceler" }
func (m *cancelOnStart) App() *App           { return m.app }
func (m *cancelOnStart) DependsOn() []string { return nil }

func (m *cancelOnStart) Init() error {
	m.rec.add("canceler", "Init")
	return nil
}

func (m *cancelOnStart) Start() error {
	m.rec.add("canceler", "Start")
	m.cancel()
	return nil
}

func (m *cancelOnStart) Stop()                        { m.rec.add("canceler", "Stop") }
func (m *cancelOnStart) Shutdown(ctx context.Context) { m.rec.add("canceler", "Shutdown") }

func TestRunFullLifecycle(t *testing.T) {
	h := newHarness(spec{name: "a"}, spec{name: "b"})
	a := h.newApp("test")

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	a.RegisterModule(func(app *App) Module {
		return &cancelOnStart{app: app, rec: h.rec, cancel: cancel}
	})

	if err := a.Run(ctx); err != nil {
		t.Fatalf("Run() error = %v, want nil", err)
	}

	assertCalls(t, h.rec.snapshot(), []string{
		"a.Init", "b.Init", "canceler.Init",
		"a.Start", "b.Start", "canceler.Start",
		"canceler.Stop", "b.Stop", "a.Stop",
		"canceler.Shutdown", "b.Shutdown", "a.Shutdown",
	})
}

func TestRunReturnsStartupError(t *testing.T) {
	boom := errors.New("boom")
	h := newHarness(spec{name: "a"}, spec{name: "b", initErr: boom})
	a := h.newApp("test")

	// ctx 永不取消：Run 若没在启动失败时返回，这个测试会直接挂住。
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	if err := a.Run(ctx); !errors.Is(err, boom) {
		t.Errorf("Run() error = %v, want wraps %v", err, boom)
	}
}

// 框架承诺在一切 Shutdown 之前 cancel 进程 ctx。正常路径由 Stop 末尾负责。
func TestStopCancelsContextBeforeShutdown(t *testing.T) {
	h := newHarness(spec{name: "a"}, spec{name: "b"})
	a := h.started(t, "test")
	a.Stop()
	a.Shutdown()

	for _, name := range []string{"a", "b"} {
		if !h.mods[name].ctxCanceledAtShutdown {
			t.Errorf("%s: Shutdown 被调用时 ctx 还没 cancel，等它的 goroutine 会一直卡住", name)
		}
	}
}

// 回滚路径曾经漏掉 cancel：启动失败后 ctx 到进程结束都不会 done，
// 而回滚恰恰是会调 Shutdown 的那条路。
func TestInitRollbackCancelsContext(t *testing.T) {
	h := newHarness(spec{name: "a"}, spec{name: "b", initErr: errors.New("boom")})
	a := h.newApp("test")
	if err := a.Build(); err != nil {
		t.Fatalf("Build() error = %v, want nil", err)
	}
	if err := a.Init(); err == nil {
		t.Fatal("Init() error = nil, want non-nil")
	}

	if !h.mods["a"].ctxCanceledAtShutdown {
		t.Error("Init 回滚：a 的 Shutdown 被调用时 ctx 还没 cancel")
	}
}

func TestStartRollbackCancelsContext(t *testing.T) {
	h := newHarness(spec{name: "a"}, spec{name: "b", startErr: errors.New("boom")})
	a := h.newApp("test")
	if err := a.Build(); err != nil {
		t.Fatalf("Build() error = %v, want nil", err)
	}
	if err := a.Init(); err != nil {
		t.Fatalf("Init() error = %v, want nil", err)
	}
	if err := a.Start(); err == nil {
		t.Fatal("Start() error = nil, want non-nil")
	}

	if !h.mods["a"].ctxCanceledAtShutdown {
		t.Error("Start 回滚：a 的 Shutdown 被调用时 ctx 还没 cancel")
	}
}

func TestStopAndShutdownIdempotent(t *testing.T) {
	h := newHarness(spec{name: "a"}, spec{name: "b"})
	a := h.started(t, "test")

	a.Stop()
	a.Stop()
	a.Shutdown()
	a.Shutdown()

	// 每个模块的每个阶段只能被调一次。
	assertCalls(t, h.rec.snapshot(), []string{
		"a.Init", "b.Init",
		"a.Start", "b.Start",
		"b.Stop", "a.Stop",
		"b.Shutdown", "a.Shutdown",
	})
}

func TestStopBeforeStart(t *testing.T) {
	h := newHarness(spec{name: "a"}, spec{name: "b"})
	a := h.newApp("test")
	if err := a.Build(); err != nil {
		t.Fatalf("Build() error = %v, want nil", err)
	}
	if err := a.Init(); err != nil {
		t.Fatalf("Init() error = %v, want nil", err)
	}

	// 没有任何模块 Start 过，Stop 不需要发信号；Shutdown 仍要释放 Init 期资源。
	a.Stop()
	a.Shutdown()

	assertCalls(t, h.rec.snapshot(), []string{
		"a.Init", "b.Init",
		"b.Shutdown", "a.Shutdown",
	})
}

func TestPhaseErrors(t *testing.T) {
	h := newHarness(spec{name: "a"})
	a := h.newApp("test")

	if err := a.Init(); !errors.Is(err, ErrPhase) {
		t.Errorf("Init() before Build error = %v, want %v", err, ErrPhase)
	}
	if err := a.Start(); !errors.Is(err, ErrPhase) {
		t.Errorf("Start() before Build error = %v, want %v", err, ErrPhase)
	}
	if err := a.Build(); err != nil {
		t.Fatalf("Build() error = %v, want nil", err)
	}
	if err := a.Build(); !errors.Is(err, ErrPhase) {
		t.Errorf("second Build() error = %v, want %v", err, ErrPhase)
	}
	if err := a.Start(); !errors.Is(err, ErrPhase) {
		t.Errorf("Start() before Init error = %v, want %v", err, ErrPhase)
	}
}

func TestRegisterAfterBuildPanics(t *testing.T) {
	h := newHarness(spec{name: "a"})
	a := h.newApp("test")
	if err := a.Build(); err != nil {
		t.Fatalf("Build() error = %v, want nil", err)
	}

	defer func() {
		if recover() == nil {
			t.Error("RegisterModule() after Build did not panic")
		}
	}()
	a.RegisterModule(func(*App) Module { return nil })
}

func TestBuildErrors(t *testing.T) {
	tests := []struct {
		name    string
		appName string
		specs   []spec
		raw     func(a *App)
		wantErr error
	}{
		{
			name:    "进程名为空",
			appName: "",
			wantErr: ErrEmptyName,
		},
		{
			name:    "模块名为空",
			appName: "test",
			specs:   []spec{{name: ""}},
			wantErr: ErrEmptyName,
		},
		{
			name:    "重名模块",
			appName: "test",
			specs:   []spec{{name: "a"}, {name: "a"}},
			wantErr: ErrDuplicateModule,
		},
		{
			name:    "nil 工厂",
			appName: "test",
			raw:     func(a *App) { a.RegisterModule(nil) },
			wantErr: ErrNilFactory,
		},
		{
			name:    "工厂返回 nil",
			appName: "test",
			raw:     func(a *App) { a.RegisterModule(func(*App) Module { return nil }) },
			wantErr: ErrNilModule,
		},
		{
			name:    "依赖缺失",
			appName: "test",
			specs:   []spec{{name: "a", deps: []string{"z"}}},
			wantErr: ErrUnknownDep,
		},
		{
			name:    "依赖自己",
			appName: "test",
			specs:   []spec{{name: "a", deps: []string{"a"}}},
			wantErr: ErrSelfDep,
		},
		{
			name:    "依赖成环",
			appName: "test",
			specs:   []spec{{name: "a", deps: []string{"b"}}, {name: "b", deps: []string{"a"}}},
			wantErr: ErrCycle,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			h := newHarness(tt.specs...)
			a := h.newApp(tt.appName)
			if tt.raw != nil {
				tt.raw(a)
			}
			if err := a.Build(); !errors.Is(err, tt.wantErr) {
				t.Errorf("Build() error = %v, want %v", err, tt.wantErr)
			}
		})
	}
}

// modulesOf 按给定顺序造出假模块，供 depsSort 直接消费。
func modulesOf(specs ...spec) []Module {
	out := make([]Module, 0, len(specs))
	for _, s := range specs {
		out = append(out, &fakeModule{name: s.name, deps: s.deps})
	}
	return out
}

func TestDepsSort(t *testing.T) {
	tests := []struct {
		name    string
		specs   []spec
		want    []string
		wantErr error
	}{
		{
			name:  "空表",
			specs: nil,
			want:  []string{},
		},
		{
			name:  "无依赖保持注册序",
			specs: []spec{{name: "a"}, {name: "b"}, {name: "c"}},
			want:  []string{"a", "b", "c"},
		},
		{
			name:  "依赖强制重排",
			specs: []spec{{name: "c", deps: []string{"a"}}, {name: "a"}},
			want:  []string{"a", "c"},
		},
		{
			name:  "无依赖并列取注册序",
			specs: []spec{{name: "a"}, {name: "b", deps: []string{"c"}}, {name: "c"}},
			want:  []string{"a", "c", "b"},
		},
		{
			name:  "链式依赖",
			specs: []spec{{name: "a", deps: []string{"b"}}, {name: "b", deps: []string{"c"}}, {name: "c"}},
			want:  []string{"c", "b", "a"},
		},
		{
			name:  "重复依赖只算一次入度",
			specs: []spec{{name: "b", deps: []string{"a", "a"}}, {name: "a"}},
			want:  []string{"a", "b"},
		},
		{
			name:  "空依赖名忽略",
			specs: []spec{{name: "a", deps: []string{""}}},
			want:  []string{"a"},
		},
		{
			name:    "依赖缺失",
			specs:   []spec{{name: "a", deps: []string{"z"}}},
			wantErr: ErrUnknownDep,
		},
		{
			name:    "依赖自己",
			specs:   []spec{{name: "a", deps: []string{"a"}}},
			wantErr: ErrSelfDep,
		},
		{
			name:    "成环",
			specs:   []spec{{name: "a", deps: []string{"b"}}, {name: "b", deps: []string{"a"}}},
			wantErr: ErrCycle,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got, err := depsSort(modulesOf(tt.specs...))
			if tt.wantErr != nil {
				if !errors.Is(err, tt.wantErr) {
					t.Fatalf("depsSort() error = %v, want %v", err, tt.wantErr)
				}
				return
			}
			if err != nil {
				t.Fatalf("depsSort() error = %v, want nil", err)
			}
			names := namesOf(got)
			if !slices.Equal(names, tt.want) {
				t.Errorf("depsSort() = %q, want %q", names, tt.want)
			}
		})
	}
}
