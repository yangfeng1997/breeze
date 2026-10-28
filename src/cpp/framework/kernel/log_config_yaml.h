// LoggerConfig 的 yaml 绑定。绑定放在这一层，而不是 base/logger 或 base/config：
// 两个 base 库互不依赖，认识两边的只有用到它们的 framework。
//
//   log:
//     output: file                  # file | stdout | stderr
//     name: gatesvr_1
//     level: info                   # trace | debug | info | warn | error | fatal | off
//     file:  { dir: ../log, retain_hours: 168, max_file_bytes: 268435456, max_files_per_hour: 4 }
//     async: { enabled: true, queue_size: 16384, threads: 1, overflow: block }   # block | drop_oldest
//     flush: { level: warn, interval_sec: 1 }
//
// 没写的键保持 LoggerConfig 里的默认值；yaml 键 snake_case，C++ 字段驼峰。

#pragma once

#include "config.h"
#include "logger.h"

namespace breeze
{

namespace detail
{
inline constexpr const char* kLevelNames = "trace | debug | info | warn | error | fatal | off";
inline std::string LevelName(const Level& l) { return std::string(ToString(l)); }
} // namespace detail

template <>
struct ConfigTraits<FileOptions> : StructTraits<FileOptions>
{
    static FieldTable<FileOptions> MakeFields()
    {
        return {
            Field("dir", &FileOptions::dir),
            Field("retain_hours", &FileOptions::retainHours),
            Field("max_file_bytes", &FileOptions::maxFileBytes),
            Field("max_files_per_hour", &FileOptions::maxFilesPerHour),
        };
    }
};

template <>
struct ConfigTraits<AsyncOptions> : StructTraits<AsyncOptions>
{
    static FieldTable<AsyncOptions> MakeFields()
    {
        return {
            Field("enabled", &AsyncOptions::enabled),
            Field("queue_size", &AsyncOptions::queueSize).Range(1, 1u << 24),
            Field("threads", &AsyncOptions::threads).Range(1, 64),
            Field("overflow", &AsyncOptions::overflow)
                .Enum({{"block", Overflow::Block}, {"drop_oldest", Overflow::DropOldest}}),
        };
    }
};

template <>
struct ConfigTraits<FlushOptions> : StructTraits<FlushOptions>
{
    static FieldTable<FlushOptions> MakeFields()
    {
        return {
            Field("level", &FlushOptions::level).Parse(ParseLevel, detail::kLevelNames, detail::LevelName),
            Field("interval_sec", &FlushOptions::intervalSec),
        };
    }
};

template <>
struct ConfigTraits<LoggerConfig> : StructTraits<LoggerConfig>
{
    static FieldTable<LoggerConfig> MakeFields()
    {
        return {
            Field("output", &LoggerConfig::output)
                .Enum({{"file", Output::File}, {"stdout", Output::Stdout}, {"stderr", Output::Stderr}}),
            Field("name", &LoggerConfig::name),
            Field("level", &LoggerConfig::level).Parse(ParseLevel, detail::kLevelNames, detail::LevelName),
            Field("pattern", &LoggerConfig::pattern),
            Field("file", &LoggerConfig::file),
            Field("async", &LoggerConfig::async),
            Field("flush", &LoggerConfig::flush),
        };
    }
};

} // namespace breeze
