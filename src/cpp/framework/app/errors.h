// 错误码与 Result。对照 src/go/core/app/errors.go。
//
// Go 靠 errors.Is 沿 %w 链判定哨兵；C++ 没有链，改用 Errc 判定，msg 已带进程名/模块名前缀。

#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace breeze
{

enum class Errc
{
    Phase,           // 生命周期调用错序，如未 Build 就 Init
    EmptyName,       // 进程名或模块名为空
    DuplicateModule, // 重名模块
    UnknownDep,      // DependsOn 引用了未注册的模块
    SelfDep,         // 模块依赖自己
    Cycle,           // 依赖成环
    NilFactory,      // 注册了空工厂
    NilModule,       // 工厂返回了空模块
    Module,          // 模块自己的 Init/Start 失败，原因在 msg
};

struct Error
{
    Errc code;
    std::string msg;
};

template <class T = void>
using Result = std::expected<T, Error>;

inline std::unexpected<Error> Fail(Errc code, std::string msg)
{
    return std::unexpected<Error>(Error{code, std::move(msg)});
}

constexpr std::string_view Describe(Errc c) noexcept
{
    switch (c)
    {
        case Errc::Phase:
            return "invalid lifecycle phase";
        case Errc::EmptyName:
            return "empty name";
        case Errc::DuplicateModule:
            return "duplicate module name";
        case Errc::UnknownDep:
            return "unknown dependency";
        case Errc::SelfDep:
            return "module depends on itself";
        case Errc::Cycle:
            return "dependency cycle";
        case Errc::NilFactory:
            return "nil module factory";
        case Errc::NilModule:
            return "nil module";
        case Errc::Module:
            return "module failed";
    }
    return "unknown";
}

} // namespace breeze
