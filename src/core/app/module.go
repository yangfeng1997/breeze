package app

// Module 是一个可被 App 装配、调度并驱动生命周期的功能单元，给 APP 赋予功能扩充
// 模块通过工厂函数注册，App 在 Build 阶段把自身递进工厂，模块自行保存
type Module interface {
	Name() string
	App() *App
	Init() error
	Start() error
	Stop()
	Shutdown()
	DependsOn() []string // DependsOn 返回该模块依赖的模块名，Build 阶段据此做拓扑排序
}
