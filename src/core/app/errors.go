// 本文件定义 App 的哨兵错误与错误组装助手，统一 Build/Init/Start 的错误串格式。

package app

import (
	"errors"
	"fmt"
)

var (
	// ErrPhase 表示生命周期方法调用顺序非法，例如未 Build 就 Init。
	ErrPhase = errors.New("invalid lifecycle phase")

	// ErrEmptyName 表示进程名或模块名为空。
	ErrEmptyName = errors.New("empty name")

	// ErrDuplicateModule 表示注册了重名模块。
	ErrDuplicateModule = errors.New("duplicate module name")

	// ErrUnknownDep 表示 DependsOn 引用了未注册的模块名。
	ErrUnknownDep = errors.New("unknown dependency")

	// ErrSelfDep 表示模块依赖了自己。
	ErrSelfDep = errors.New("module depends on itself")

	// ErrCycle 表示模块依赖成环，无法拓扑排序。
	ErrCycle = errors.New("dependency cycle")

	// ErrNilFactory 表示注册了 nil 工厂。
	ErrNilFactory = errors.New("nil module factory")

	// ErrNilModule 表示工厂返回了 nil 模块。
	ErrNilModule = errors.New("nil module")
)

// wrapApp 给错误加上进程名前缀，保留 %w 链供 errors.Is 判定。
func wrapApp(appName string, err error) error {
	return fmt.Errorf("%s: %w", appName, err)
}

// errAt 组装"某次工厂调用出错"的错误串：进程名 + 工厂序号 + 可选细节 + 哨兵。
// 工厂序号即注册序，用于在模块名还拿不到时报出位置。
func errAt(appName string, i int, err error, detail ...string) error {
	prefix := fmt.Sprintf("%s: factory #%d", appName, i)
	if len(detail) > 0 && detail[0] != "" {
		prefix += ": " + detail[0]
	}
	return fmt.Errorf("%s: %w", prefix, err)
}

// stateErr 组装阶段错序错误，报出当前阶段与期望阶段。
func (a *App) stateErr(method string, want lifecycleState) error {
	return fmt.Errorf("%s: %s: current %s, want %s: %w", a.name, method, a.state, want, ErrPhase)
}

// moduleErr 组装"某模块在某阶段失败"的错误串。
func (a *App) moduleErr(method, module string, err error) error {
	return fmt.Errorf("%s: module %s: %s: %w", a.name, module, method, err)
}
