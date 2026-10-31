cmake_minimum_required(VERSION 3.31)
project(breeze LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # clangd 靠它找标准库与 -std

# 不用 C++ modules。开着的话 g++ 的 -fmodules-ts 依赖扫描遇到路径里的中文
# （仓库在 "源代码" 下）会报 "cannot determine '#include' translation"。
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# 二进制里不留构建机的绝对路径（含中文目录名）：__FILE__ 与 std::source_location 都按源码根相对化。
# 只影响这两处，不影响调试信息，gdb 找源码照常。日志里 %s 本来就只取文件名。
add_compile_options(-fmacro-prefix-map=${CMAKE_SOURCE_DIR}/=)

# 消毒器：-DBREEZE_SANITIZE=thread|address,undefined 等，见 CMakePresets 的 tsan / asan。
# 必须在所有 add_library / add_executable 之前，否则不作用于它们。
set(BREEZE_SANITIZE "" CACHE STRING "sanitizer 列表（-fsanitize= 的值）；留空则不开")
if(BREEZE_SANITIZE)
  add_compile_options(-fsanitize=${BREEZE_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${BREEZE_SANITIZE})
endif()
# 依赖无锁库的测试在 tsan 下要带压制清单（原因见 tsan.supp）；其它预设下为空，set_tests_properties 无副作用。
set(BREEZE_TEST_ENV "")
if(BREEZE_SANITIZE MATCHES "thread")
  set(BREEZE_TEST_ENV "TSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tsan.supp")
endif()

find_package(Threads REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(concurrentqueue CONFIG REQUIRED)
find_package(kcp CONFIG REQUIRED)                # rudp 内部用（ARQ，只在 breeze_net_rudp 里 PRIVATE 链接）
find_package(readerwriterqueue CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)  # configgen 是 Python 脚本

# 日志的编译期级别：Release 只剥离 TRACE，保留 DEBUG —— 线上排查常要临时打开 DEBUG，
# 编译期就去掉的话只能重新发版；TRACE 最啰嗦最热，剥掉没有代价。Debug 全保留。
# 运行时级别另由 LoggerConfig::level / SetLevel 控制。
# **只对用到日志宏的库/可执行文件调用**；测试目标不调用，保持全保留，否则断言 DEBUG 输出的用例会失败。
function(breeze_apply_log_level target)
  target_compile_definitions(${target} PRIVATE
    $<$<CONFIG:Release>:BREEZE_LOG_ACTIVE_LEVEL=BREEZE_LOG_LEVEL_DEBUG>)
endfunction()

set(APP_DIR framework/app)
set(ARGS_DIR base/args)
set(CONFIG_DIR base/config)
set(CONFIGGEN_DIR ${CMAKE_SOURCE_DIR}/../../tools/configgen)
set(CONFIG_SCHEMA_ROOT ${CMAKE_SOURCE_DIR}/../../config/schema)  # 真实 schema 与 options.proto
set(BREEZE_GEN_DIR ${CMAKE_SOURCE_DIR}/gen)  # 生成代码根目录，不进版本库
# 所有 include 都从 src/cpp 起算：#include "base/net/endpoint/endpoint.h"、#include "gen/config/net_config.h"。
# 不给各个库配自己的 include 目录，所以头文件名不必全局唯一，写成扁平名也编译不过。
include_directories(${CMAKE_SOURCE_DIR})
set(LOGGER_DIR base/logger)
set(KERNEL_DIR framework/kernel)
set(QUEUE_DIR base/queue)
set(BUFFER_DIR base/buffer)
set(THREAD_DIR base/thread)
set(ASSERT_DIR base/assert)
set(BACKTRACE_DIR base/backtrace)
set(DEFER_DIR base/defer)
set(SINGLETON_DIR base/singleton)
set(NONCOPYABLE_DIR base/noncopyable)
set(STRING_DIR base/string)
set(RANDOM_DIR base/random)
set(TIME_DIR base/time)
set(TIMER_DIR base/timer)
set(CONTAINER_DIR base/container)
set(EVENT_DIR base/event)
set(HASH_DIR base/hash)
set(TIME_RECORD_DIR base/time_record)
set(METRICS_DIR base/metrics)
set(VARTYPE_DIR base/vartype)
set(URI_DIR base/uri)
set(FSM_DIR base/fsm)
set(NET_DIR base/net)
# net 的目录只用来按概念归类，不做访问控制：loop/（事件循环：EventLoop / EventLoopGroup、连接表、
# IPollable、统计）、socket/（UniqueFd、Endpoint 与 sockaddr 互转、按协议族建 socket）、endpoint/（地址与对端信息）、
# codec/（分帧）、connection/（连接及其回调 / 句柄 / 选项，INetServer / INetClient）、stream/（TCP / UDS 共用的字节流：
# 服务端 / 客户端基类，以及 Acceptor / Connector / StreamConnection 等实现）、tcp/、uds/、rudp/（每种传输一个目录：
# 端点、选项、服务端、客户端，以及各自专有的 socket 选项 / 会话 / 票据）。stream、rudp 的实现文件比公开文件多，放在各自的
# internal/ 里与公开的服务端 / 客户端 / 端点 / 选项分开看，同样不做访问控制。外部代码只该 include 公开头，
# 由 tools/check_includes.py 守着。
# 测试公共件（net_test_util.h）与跨组件的集成测试，只给测试用。
set(NET_TESTING_DIR ${NET_DIR}/testing)
set(NET_MODULE_DIR framework/net)

# CLI11 只 PRIVATE 链接：公开头文件不含它，使用方看不到这个依赖。
# internal/ 里的 args_impl.h 含 CLI11，所以只能 PRIVATE 链接；使用方只该 include args_parser.h。
add_library(breeze_args ${ARGS_DIR}/args_parser.cpp)
target_link_libraries(breeze_args PRIVATE CLI11::CLI11)
target_compile_options(breeze_args PRIVATE -Wall -Wextra)

# breeze_config_schema(<target> SCHEMA_DIR <dir> [GEN_SUBDIR <名>] [INCLUDE <dir>...])
# 用 tools/configgen 把目录下所有 schema（*.proto，不含 options.proto）生成 C++ 头文件，不进版本库。
# <target> 是 INTERFACE 库，自带 breeze_config：
#   给了 GEN_SUBDIR：生成到 ${BREEZE_GEN_DIR}/<名>/（src/cpp/gen/<名>/），从 src/cpp 起算，
#                    用 #include "gen/<名>/<schema 文件名>_config.h"。scripts/configure.py 也写这个目录。
#   没给：生成到构建目录，include 根就是它，用 #include "<schema 文件名>_config.h"（测试用的样例 schema 走这条）。
# schema 改了会自动重新生成；内容没变不重写文件，不会触发无谓的重编。
# options.proto（自定义 option）固定在 ${CONFIG_SCHEMA_ROOT}，会自动加进 proto 搜索路径。
# protoc 按 环境变量 PROTOC > PATH 上的 protoc / protoc.exe 查找；也可以 -DBREEZE_PROTOC=<路径> 指定。
set(BREEZE_PROTOC "" CACHE FILEPATH "protoc 的路径；留空则由 configgen 自己找")
function(breeze_config_schema target)
  cmake_parse_arguments(A "" "SCHEMA_DIR;GEN_SUBDIR" "INCLUDE" ${ARGN})
  file(GLOB schemas CONFIGURE_DEPENDS ${A_SCHEMA_DIR}/*.proto)
  list(FILTER schemas EXCLUDE REGEX "/options\\.proto$")
  if(A_GEN_SUBDIR)
    set(gen_dir ${BREEZE_GEN_DIR}/${A_GEN_SUBDIR})
    set(include_root "")
  else()
    set(gen_dir ${CMAKE_CURRENT_BINARY_DIR}/gen/${target})
    set(include_root ${gen_dir})
  endif()
  set(outputs)
  foreach(s ${schemas})
    get_filename_component(n ${s} NAME_WE)
    list(APPEND outputs ${gen_dir}/${n}_config.h)
  endforeach()
  set(extra_args)
  foreach(inc ${CONFIG_SCHEMA_ROOT} ${A_INCLUDE})
    list(APPEND extra_args -I ${inc})
  endforeach()
  if(BREEZE_PROTOC)
    list(APPEND extra_args --protoc ${BREEZE_PROTOC})
  endif()
  add_custom_command(OUTPUT ${outputs}
    COMMAND Python3::Interpreter ${CONFIGGEN_DIR}/configgen.py --schema-dir ${A_SCHEMA_DIR} ${extra_args}
            --cpp-out ${gen_dir}
    DEPENDS ${schemas} ${CONFIG_SCHEMA_ROOT}/options.proto ${CONFIGGEN_DIR}/configgen.py
    COMMENT "configgen: ${A_SCHEMA_DIR} -> ${gen_dir}"
    VERBATIM)
  add_custom_target(${target}_gen DEPENDS ${outputs})
  add_library(${target} INTERFACE)
  add_dependencies(${target} ${target}_gen)
  if(include_root)
    target_include_directories(${target} INTERFACE ${include_root})
  endif()
  target_link_libraries(${target} INTERFACE breeze_config)
endfunction()

# yaml-cpp 同理只 PRIVATE：config.h 不含它。
add_library(breeze_config ${CONFIG_DIR}/config.cpp)
target_link_libraries(breeze_config PRIVATE yaml-cpp::yaml-cpp)
target_compile_options(breeze_config PRIVATE -Wall -Wextra)

# spdlog 同理只 PRIVATE：logger.h / logger_macros.h 不含它，internal/ 里的头含它。
add_library(breeze_logger ${LOGGER_DIR}/logger.cpp)
target_link_libraries(breeze_logger PRIVATE spdlog::spdlog)
target_compile_options(breeze_logger PRIVATE -Wall -Wextra)

# 无锁队列：模板必须在头文件里，所以 moodycamel 经 INTERFACE 传给使用方（与上面 PRIVATE 的 CLI11 不同）。
# 公开 API 不出现 moodycamel 的类型，以后换实现使用方不动。
add_library(breeze_queue INTERFACE)
target_link_libraries(breeze_queue INTERFACE
  concurrentqueue::concurrentqueue readerwriterqueue::readerwriterqueue Threads::Threads)

# 通用字节缓冲：SharedBuffer（引用计数的不可变字节块上的切片）与 Buffer（带读写游标的缓冲）。不认识网络，业务也能直接用；
# 只依赖标准库，不限平台。shared_buffer.cpp 用日志报编程错误，所以 breeze_logger 只 PRIVATE，使用方看不到这个依赖。
add_library(breeze_buffer ${BUFFER_DIR}/shared_buffer.cpp ${BUFFER_DIR}/buffer.cpp)
target_link_libraries(breeze_buffer PRIVATE breeze_logger)
target_compile_options(breeze_buffer PRIVATE -Wall -Wextra)
breeze_apply_log_level(breeze_buffer)

# 网络库只支持 Linux（epoll、UDS、SO_PEERCRED），其它平台不构建。
set(BREEZE_BUILD_NET OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(BREEZE_BUILD_NET ON)
endif()

# 小工具：DEFER、Singleton、NonCopyable。都只有头文件，只依赖标准库，不限平台。
add_library(breeze_defer INTERFACE)
add_library(breeze_singleton INTERFACE)
add_library(breeze_noncopyable INTERFACE)

# 时间：TimeUtil（纯函数的日期计算）、Now（帧缓存时钟、可模拟的逻辑时间）与 PeriodicWindow（周期开放窗口，只有头文件）。
# 用 localtime_r（POSIX），不限 Linux。
add_library(breeze_time ${TIME_DIR}/time_util.cpp ${TIME_DIR}/now.cpp)
target_compile_options(breeze_time PRIVATE -Wall -Wextra)

# 耗时记录（TimeRecord）：单调时钟的秒表，可选地在析构时写日志。logger 只 PRIVATE：公开头不含它。
add_library(breeze_time_record ${TIME_RECORD_DIR}/time_record.cpp)
target_link_libraries(breeze_time_record PRIVATE breeze_logger)
target_compile_options(breeze_time_record PRIVATE -Wall -Wextra)

# 分层时间轮（TimerWheel）：只依赖标准库，不限平台。
add_library(breeze_timer ${TIMER_DIR}/timer_wheel.cpp)
target_compile_options(breeze_timer PRIVATE -Wall -Wextra)

# 容器：FlatHashMap / FlatHashSet（Swiss table）、LinkedHashMap / LinkedHashSet（保持插入顺序）。只有头文件，只依赖标准库。
add_library(breeze_container INTERFACE)

# 消息统计（MessageStats）：按 id / 名字统计条数与字节，周期写日志。非线程安全。
# 公开头含 flat_hash_map.h，所以 breeze_container 是 PUBLIC；logger 只 PRIVATE：公开头不含它。
add_library(breeze_metrics ${METRICS_DIR}/message_stats.cpp)
target_link_libraries(breeze_metrics PUBLIC breeze_container PRIVATE breeze_logger)
target_compile_options(breeze_metrics PRIVATE -Wall -Wextra)

# 稳定的字节哈希（MurmurHash64A）：只有头文件，只依赖标准库，不限平台。
add_library(breeze_hash INTERFACE)

# 有限状态机（fsm::Machine）：只有头文件，只依赖标准库（用 cxxabi 给状态名 demangle），不限平台。
add_library(breeze_fsm INTERFACE)

# URI 解析（RFC 3986）与百分号编解码：只依赖标准库，不限平台。
add_library(breeze_uri ${URI_DIR}/uri.cpp)
target_compile_options(breeze_uri PRIVATE -Wall -Wextra)

# 弱类型值（VarType）：字符串存放、按需转数字 / 布尔。只有头文件，依赖 breeze_string 的严格数字解析。
add_library(breeze_vartype INTERFACE)
target_link_libraries(breeze_vartype INTERFACE breeze_string)

# 字符串工具（Split / Join / Trim / 数字解析……）：只依赖标准库，不限平台。
add_library(breeze_string ${STRING_DIR}/string_util.cpp)
target_compile_options(breeze_string PRIVATE -Wall -Wextra)

# 伪随机数（xoshiro256**）：getrandom 取熵、pthread_atfork 让 fork 后重新播种，只支持 Linux，与网络库同一个开关。
if(BREEZE_BUILD_NET)
  add_library(breeze_random ${RANDOM_DIR}/random_util.cpp)
  target_compile_options(breeze_random PRIVATE -Wall -Wextra)
endif()

# 调用栈文本化（Backtrace）：libgcc 的 _Unwind_Backtrace + dladdr，只支持 Linux，所以与网络库同一个开关。
# 不依赖日志。dladdr 在较老的 glibc 里要 -ldl，所以 CMAKE_DL_LIBS 是 PUBLIC。
if(BREEZE_BUILD_NET)
  add_library(breeze_backtrace ${BACKTRACE_DIR}/backtrace.cpp)
  target_link_libraries(breeze_backtrace PUBLIC ${CMAKE_DL_LIBS})
  target_compile_options(breeze_backtrace PRIVATE -Wall -Wextra)
endif()

# 防御式断言（ASSERT_*）。失败时用 breeze_backtrace 留调用栈，经默认 logger 输出，所以两者都是 PRIVATE：公开头不含它们。
# 断言库自己不分 Debug / Release：模式由使用方翻译单元的 NDEBUG（或 BREEZE_ASSERT_FATAL）在宏展开时决定。
if(BREEZE_BUILD_NET)
  add_library(breeze_assert ${ASSERT_DIR}/assert_macros.cpp)
  target_link_libraries(breeze_assert PRIVATE breeze_backtrace breeze_logger)
  target_compile_options(breeze_assert PRIVATE -Wall -Wextra)
  breeze_apply_log_level(breeze_assert)

  # 事件总线：只有头文件。递归发布超限用 ASSERT_RET_VOID 处理，所以依赖 breeze_assert（仅 Linux）。
  add_library(breeze_event INTERFACE)
  target_link_libraries(breeze_event INTERFACE breeze_assert)
endif()

if(BREEZE_BUILD_NET)
  # 线程基础库：Thread（创建失败返回 errno、不抛异常）、this_thread（缓存 tid / 线程名 / 是否主线程）、WaitGroup、SpinLock、
  # ThreadPool。用到 pthread 命名、亲和性与 gettid，只支持 Linux，所以与网络库同一个开关。
  # breeze_logger 只 PRIVATE：thread.cpp 等用日志报编程错误，使用方看不到这个依赖。
  add_library(breeze_thread
    ${THREAD_DIR}/thread.cpp
    ${THREAD_DIR}/current_thread.cpp
    ${THREAD_DIR}/wait_group.cpp
    ${THREAD_DIR}/thread_pool.cpp)
  target_link_libraries(breeze_thread PUBLIC Threads::Threads PRIVATE breeze_logger)
  target_compile_options(breeze_thread PRIVATE -Wall -Wextra)
  breeze_apply_log_level(breeze_thread)

  # 日志只 PRIVATE 链接，使用方看不到这个依赖。
  add_library(breeze_net
    ${NET_DIR}/endpoint/endpoint.cpp
    ${NET_DIR}/codec/length_prefix_codec.cpp
    ${NET_DIR}/connection/connection_handle.cpp
    ${NET_DIR}/loop/event_loop_group.cpp
    ${NET_DIR}/loop/connection_table.cpp
    ${NET_DIR}/loop/group_registry.cpp
    ${NET_DIR}/loop/event_loop.cpp
    ${NET_DIR}/socket/unique_fd.cpp
    ${NET_DIR}/socket/socket_address.cpp
    ${NET_DIR}/stream/stream_server.cpp
    ${NET_DIR}/stream/stream_client.cpp
    ${NET_DIR}/stream/internal/stream_socket.cpp
    ${NET_DIR}/stream/internal/stream_socket_config.cpp
    ${NET_DIR}/stream/internal/acceptor.cpp
    ${NET_DIR}/stream/internal/loop_server.cpp
    ${NET_DIR}/stream/internal/connector.cpp
    ${NET_DIR}/stream/internal/stream_connection.cpp
    ${NET_DIR}/tcp/tcp_socket.cpp
    ${NET_DIR}/tcp/tcp_endpoint.cpp
    ${NET_DIR}/tcp/tcp_server.cpp
    ${NET_DIR}/tcp/tcp_client.cpp
    ${NET_DIR}/uds/uds_socket.cpp
    ${NET_DIR}/uds/uds_endpoint.cpp
    ${NET_DIR}/uds/uds_server.cpp
    ${NET_DIR}/uds/uds_client.cpp
    ${NET_DIR}/rudp/rudp_endpoint.cpp)
  # breeze_queue 是 PUBLIC：event_loop.h 里有 MpscQueue；breeze_buffer 同理：公开头里有 SharedBuffer / Buffer；
  # breeze_thread：event_loop_thread.h 里有 Thread，测试与 bench 要用。
  target_link_libraries(breeze_net PUBLIC Threads::Threads breeze_queue breeze_buffer breeze_thread PRIVATE breeze_logger)
  target_compile_options(breeze_net PRIVATE -Wall -Wextra)
  breeze_apply_log_level(breeze_net)

  # rudp 单独一个库：KCP 只在这里 PRIVATE 链接，不用 rudp 的使用方（只用 TCP / UDS）不背这个依赖。
  # 端点（RUdpEndpoint）仍在 breeze_net：它只是地址类型，NetModule 解析端点字符串要用。
  add_library(breeze_net_rudp
    ${NET_DIR}/rudp/rudp_protocol.cpp
    ${NET_DIR}/rudp/rudp_server.cpp
    ${NET_DIR}/rudp/rudp_client.cpp
    ${NET_DIR}/rudp/internal/ticket_table.cpp
    ${NET_DIR}/rudp/internal/udp_socket.cpp
    ${NET_DIR}/rudp/internal/kcp_session.cpp
    ${NET_DIR}/rudp/internal/rudp_connection.cpp)
  target_link_libraries(breeze_net_rudp PUBLIC breeze_net PRIVATE kcp::kcp breeze_container breeze_logger)
  target_compile_options(breeze_net_rudp PRIVATE -Wall -Wextra)
  breeze_apply_log_level(breeze_net_rudp)
endif()

add_library(breeze_app
  ${APP_DIR}/app.cpp
  ${APP_DIR}/lifecycle.cpp
  ${APP_DIR}/loop.cpp
  ${APP_DIR}/module.cpp)
target_link_libraries(breeze_app PUBLIC Threads::Threads PRIVATE breeze_logger)
target_compile_options(breeze_app PRIVATE -Wall -Wextra)
breeze_apply_log_level(breeze_app)

enable_testing()
add_executable(app_test ${APP_DIR}/app_test.cpp)
target_link_libraries(app_test PRIVATE breeze_app breeze_logger)
add_test(NAME app_test COMMAND app_test)

add_executable(args_test ${ARGS_DIR}/args_test.cpp)
target_link_libraries(args_test PRIVATE breeze_args)
add_test(NAME args_test COMMAND args_test)

# 测试要直接喂 HourlyFileSink（base/logger/internal/），所以也直接链接 spdlog，使用方则不行。
add_executable(logger_test ${LOGGER_DIR}/logger_test.cpp)
target_link_libraries(logger_test PRIVATE breeze_logger spdlog::spdlog)
add_test(NAME logger_test COMMAND logger_test)

# 真实的 schema（config/schema/）：生成并编译，schema 改坏了构建就失败。
breeze_config_schema(breeze_config_schemas SCHEMA_DIR ${CONFIG_SCHEMA_ROOT} GEN_SUBDIR config)

# 内核模块：依赖 App（Module 基类）与 Logger；LoggerConfig 出现在 LogModule 的构造函数里，故 PUBLIC。
# ConfigModule 的头文件里有 ArgsParser 与生成的 CommonConfig，所以 breeze_args / breeze_config_schemas 也是 PUBLIC。
add_library(breeze_kernel ${KERNEL_DIR}/log_module.cpp ${KERNEL_DIR}/config_module.cpp)
# log_config_yaml.h 同时用到 Logger 与 Config 的类型，所以两者都是 PUBLIC。
target_link_libraries(breeze_kernel PUBLIC breeze_app breeze_logger breeze_config breeze_args breeze_config_schemas)
target_compile_options(breeze_kernel PRIVATE -Wall -Wextra)
breeze_apply_log_level(breeze_kernel)

# framework/net：把 base/net 的连接事件回投 App 主循环（MainLoopExecutor）并装配成模块（NetModule）。
# net.proto 生成的头文件经 breeze_config_schemas 传递。网络库只支持 Linux。
if(BREEZE_BUILD_NET)
  add_library(breeze_net_module ${NET_MODULE_DIR}/main_loop_executor.cpp ${NET_MODULE_DIR}/net_module.cpp)
  target_link_libraries(breeze_net_module PUBLIC breeze_net breeze_net_rudp breeze_kernel breeze_queue
                        breeze_config_schemas)
  target_compile_options(breeze_net_module PRIVATE -Wall -Wextra)
  breeze_apply_log_level(breeze_net_module)
endif()

add_executable(log_module_test ${KERNEL_DIR}/log_module_test.cpp)
target_link_libraries(log_module_test PRIVATE breeze_kernel)
add_test(NAME log_module_test COMMAND log_module_test)

add_executable(config_module_test ${KERNEL_DIR}/config_module_test.cpp)
target_link_libraries(config_module_test PRIVATE breeze_kernel)
target_compile_definitions(config_module_test PRIVATE BREEZE_CONFIG_DIR="${CONFIG_SCHEMA_ROOT}/..")
add_test(NAME config_module_test COMMAND config_module_test)

# 编译期剥离本身的测试：该 TU 自己定义 BREEZE_LOG_ACTIVE_LEVEL，与构建类型无关。
add_executable(logger_strip_test ${LOGGER_DIR}/logger_strip_test.cpp)
target_link_libraries(logger_strip_test PRIVATE breeze_logger)
add_test(NAME logger_strip_test COMMAND logger_strip_test)

# 鲁棒性（队列溢出、多后台线程、多线程压力、Shutdown 并发、滚动并发）与时区/夏令时。
# 都要直接用 internal/ 里的 sink 和测试工具，所以与 logger_test 一样直接链接 spdlog。
add_executable(logger_robust_test ${LOGGER_DIR}/logger_robust_test.cpp)
target_link_libraries(logger_robust_test PRIVATE breeze_logger spdlog::spdlog Threads::Threads)
add_test(NAME logger_robust_test COMMAND logger_robust_test)

# TZ 环境变量只能在没有其它线程时改，所以单独一个进程。
add_executable(logger_tz_test ${LOGGER_DIR}/logger_tz_test.cpp)
target_link_libraries(logger_tz_test PRIVATE breeze_logger spdlog::spdlog)
add_test(NAME logger_tz_test COMMAND logger_tz_test)

add_executable(config_test ${CONFIG_DIR}/config_test.cpp)
target_link_libraries(config_test PRIVATE breeze_config)
add_test(NAME config_test COMMAND config_test)

add_executable(log_config_yaml_test ${KERNEL_DIR}/log_config_yaml_test.cpp)
target_link_libraries(log_config_yaml_test PRIVATE breeze_kernel)
add_test(NAME log_config_yaml_test COMMAND log_config_yaml_test)

# 生成器自己的测试。样例 schema 覆盖全部特性。
#   configgen_cpp_test：schema → 生成的头文件 → base/config 运行时，端到端
#   configgen_py_test ：schema 校验、确定性输出、JSON Schema，以及生成的 Go 代码能 vet / 跑通（需要 protoc）
breeze_config_schema(configgen_sample SCHEMA_DIR ${CONFIGGEN_DIR}/testdata/schema)
add_executable(configgen_cpp_test ${CONFIGGEN_DIR}/testdata/configgen_cpp_test.cpp)
target_link_libraries(configgen_cpp_test PRIVATE configgen_sample)
add_test(NAME configgen_cpp_test COMMAND configgen_cpp_test)

add_test(NAME configgen_py_test
  COMMAND Python3::Interpreter -m unittest test_configgen -v
  WORKING_DIRECTORY ${CONFIGGEN_DIR})

# include 约定（设计 §4.13）：base/ 不能依赖 framework/；外部代码只 include net 的公开头（测试与 bench 不受限）。
# 整个 src/cpp 是 include 根，目录不拦，靠这个脚本守。
add_test(NAME includes_test
  COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/../../tools/check_includes.py ${CMAKE_SOURCE_DIR})

add_executable(common_config_test ${KERNEL_DIR}/common_config_test.cpp)
target_link_libraries(common_config_test PRIVATE breeze_config_schemas)
# 测试读仓库里真实的 config/common.yaml 与 config/env/*.yaml
target_compile_definitions(common_config_test PRIVATE BREEZE_CONFIG_DIR="${CONFIG_SCHEMA_ROOT}/..")
add_test(NAME common_config_test COMMAND common_config_test)

# 队列只有头文件，测试 TU 就是编译它们的地方，所以这里开 -Wall -Wextra。
add_executable(queue_test ${QUEUE_DIR}/queue_test.cpp)
target_link_libraries(queue_test PRIVATE breeze_queue)
target_compile_options(queue_test PRIVATE -Wall -Wextra)
add_test(NAME queue_test COMMAND queue_test)
set_tests_properties(queue_test PROPERTIES ENVIRONMENT "${BREEZE_TEST_ENV}")

# 头文件库，测试 TU 就是编译它们的地方，所以这里开 -Wall -Wextra。singleton_test 要 fork，只在 Linux 上构建。
add_executable(defer_test ${DEFER_DIR}/defer_test.cpp)
target_link_libraries(defer_test PRIVATE breeze_defer)
target_compile_options(defer_test PRIVATE -Wall -Wextra)
add_test(NAME defer_test COMMAND defer_test)

add_executable(noncopyable_test ${NONCOPYABLE_DIR}/noncopyable_test.cpp)
target_link_libraries(noncopyable_test PRIVATE breeze_noncopyable)
target_compile_options(noncopyable_test PRIVATE -Wall -Wextra)
add_test(NAME noncopyable_test COMMAND noncopyable_test)

if(BREEZE_BUILD_NET)
  add_executable(random_util_test ${RANDOM_DIR}/random_util_test.cpp)
  target_link_libraries(random_util_test PRIVATE breeze_random Threads::Threads)
  target_compile_options(random_util_test PRIVATE -Wall -Wextra)
  add_test(NAME random_util_test COMMAND random_util_test)
  set_tests_properties(random_util_test PROPERTIES ENVIRONMENT "${BREEZE_TEST_ENV}")

  add_executable(singleton_test ${SINGLETON_DIR}/singleton_test.cpp)
  target_link_libraries(singleton_test PRIVATE breeze_singleton Threads::Threads)
  target_compile_options(singleton_test PRIVATE -Wall -Wextra)
  add_test(NAME singleton_test COMMAND singleton_test)
  set_tests_properties(singleton_test PROPERTIES ENVIRONMENT "${BREEZE_TEST_ENV}")
endif()

# 两个测试都会改 TZ 环境变量（setenv + tzset），所以各自独立成进程，且用例里不起会读时区的线程。
foreach(t time_util_test now_test)
  add_executable(${t} ${TIME_DIR}/${t}.cpp)
  target_link_libraries(${t} PRIVATE breeze_time)
  target_compile_options(${t} PRIVATE -Wall -Wextra)
  add_test(NAME ${t} COMMAND ${t})
  set_tests_properties(${t} PROPERTIES ENVIRONMENT "${BREEZE_TEST_ENV}")
endforeach()

# 与 Now 配合的用例会冻结 / 偏移 Now（进程级状态），所以独立成进程。
add_executable(periodic_window_test ${TIME_DIR}/periodic_window_test.cpp)
target_link_libraries(periodic_window_test PRIVATE breeze_time)
target_compile_options(periodic_window_test PRIVATE -Wall -Wextra)
add_test(NAME periodic_window_test COMMAND periodic_window_test)
set_tests_properties(periodic_window_test PROPERTIES ENVIRONMENT "${BREEZE_TEST_ENV}")

add_executable(time_record_test ${TIME_RECORD_DIR}/time_record_test.cpp)
target_link_libraries(time_record_test PRIVATE breeze_time_record)
target_compile_options(time_record_test PRIVATE -Wall -Wextra)
add_test(NAME time_record_test COMMAND time_record_test)

add_executable(message_stats_test ${METRICS_DIR}/message_stats_test.cpp)
target_link_libraries(message_stats_test PRIVATE breeze_metrics)
target_compile_options(message_stats_test PRIVATE -Wall -Wextra)
add_test(NAME message_stats_test COMMAND message_stats_test)

foreach(t timer_wheel_test timer_wheel_model_test)
  add_executable(${t} ${TIMER_DIR}/${t}.cpp)
  target_link_libraries(${t} PRIVATE breeze_timer)
  target_compile_options(${t} PRIVATE -Wall -Wextra)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# flat_hash_test 用 fork 测 at() 找不到键会 abort，所以只在 Linux 上构建。同一份源码再编一份强制 SWAR 的版本：
# x86 上平时走 SSE2，没有 SSE2 的平台（如 aarch64）走 SWAR，两条路径都要测。
if(BREEZE_BUILD_NET)
  add_executable(flat_hash_test ${CONTAINER_DIR}/flat_hash_test.cpp)
  target_link_libraries(flat_hash_test PRIVATE breeze_container)
  target_compile_options(flat_hash_test PRIVATE -Wall -Wextra)
  add_test(NAME flat_hash_test COMMAND flat_hash_test)

  add_executable(flat_hash_swar_test ${CONTAINER_DIR}/flat_hash_test.cpp)
  target_link_libraries(flat_hash_swar_test PRIVATE breeze_container)
  target_compile_options(flat_hash_swar_test PRIVATE -Wall -Wextra)
  target_compile_definitions(flat_hash_swar_test PRIVATE BREEZE_CONTAINER_FORCE_SWAR=1 BREEZE_EXPECT_GROUP_WIDTH=8)
  add_test(NAME flat_hash_swar_test COMMAND flat_hash_swar_test)

  add_executable(event_bus_test ${EVENT_DIR}/event_bus_test.cpp)
  target_link_libraries(event_bus_test PRIVATE breeze_event)
  target_compile_options(event_bus_test PRIVATE -Wall -Wextra)
  add_test(NAME event_bus_test COMMAND event_bus_test)
  set_tests_properties(event_bus_test PROPERTIES ENVIRONMENT "${BREEZE_TEST_ENV}")

  add_executable(linked_hash_test ${CONTAINER_DIR}/linked_hash_test.cpp)
  target_link_libraries(linked_hash_test PRIVATE breeze_container)
  target_compile_options(linked_hash_test PRIVATE -Wall -Wextra)
  add_test(NAME linked_hash_test COMMAND linked_hash_test)
endif()

add_executable(murmur_hash_test ${HASH_DIR}/murmur_hash_test.cpp)
target_link_libraries(murmur_hash_test PRIVATE breeze_hash)
target_compile_options(murmur_hash_test PRIVATE -Wall -Wextra)
add_test(NAME murmur_hash_test COMMAND murmur_hash_test)

add_executable(fsm_test ${FSM_DIR}/fsm_test.cpp)
target_link_libraries(fsm_test PRIVATE breeze_fsm)
target_compile_options(fsm_test PRIVATE -Wall -Wextra)
add_test(NAME fsm_test COMMAND fsm_test)

add_executable(var_type_test ${VARTYPE_DIR}/var_type_test.cpp)
target_link_libraries(var_type_test PRIVATE breeze_vartype)
target_compile_options(var_type_test PRIVATE -Wall -Wextra)
add_test(NAME var_type_test COMMAND var_type_test)
set_tests_properties(var_type_test PROPERTIES ENVIRONMENT "${BREEZE_TEST_ENV}")

add_executable(uri_test ${URI_DIR}/uri_test.cpp)
target_link_libraries(uri_test PRIVATE breeze_uri)
target_compile_options(uri_test PRIVATE -Wall -Wextra)
add_test(NAME uri_test COMMAND uri_test)
set_tests_properties(uri_test PROPERTIES ENVIRONMENT "${BREEZE_TEST_ENV}")

add_executable(string_util_test ${STRING_DIR}/string_util_test.cpp)
target_link_libraries(string_util_test PRIVATE breeze_string)
target_compile_options(string_util_test PRIVATE -Wall -Wextra)
add_test(NAME string_util_test COMMAND string_util_test)

add_executable(buffer_test ${BUFFER_DIR}/buffer_test.cpp)
target_link_libraries(buffer_test PRIVATE breeze_buffer)
target_compile_options(buffer_test PRIVATE -Wall -Wextra)
add_test(NAME buffer_test COMMAND buffer_test)

add_executable(shared_buffer_test ${BUFFER_DIR}/shared_buffer_test.cpp)
target_link_libraries(shared_buffer_test PRIVATE breeze_buffer Threads::Threads)
target_compile_options(shared_buffer_test PRIVATE -Wall -Wextra)
add_test(NAME shared_buffer_test COMMAND shared_buffer_test)

if(BREEZE_BUILD_NET)
  # breeze_net_test(<名> [DIR <子目录>] [LIB <库>])：${NET_DIR}/[<子目录>/]<名>.cpp 编成同名测试，
  # 测试放在被测代码所在的目录，跨组件的集成测试放 testing/。
  # LIB = 链接的库（默认 breeze_net，rudp 的测试用 breeze_net_rudp）。
  # 测试公共件（base/net/testing/net_test_util.h）与别的头一样从 src/cpp 起算 include。
  function(breeze_net_test name)
    cmake_parse_arguments(A "" "DIR;LIB" "" ${ARGN})
    if(A_DIR)
      add_executable(${name} ${NET_DIR}/${A_DIR}/${name}.cpp)
    else()
      add_executable(${name} ${NET_DIR}/${name}.cpp)
    endif()
    if(A_LIB)
      target_link_libraries(${name} PRIVATE ${A_LIB})
    else()
      target_link_libraries(${name} PRIVATE breeze_net)
    endif()
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND ${name})
    # EventLoop 的任务队列用 moodycamel：tsan 下要带压制清单（原因见 tsan.supp）；其它预设下为空。
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${BREEZE_TEST_ENV}")
  endfunction()

  # base/thread 的测试：只依赖 breeze_thread。
  function(breeze_thread_test name)
    add_executable(${name} ${THREAD_DIR}/${name}.cpp)
    target_link_libraries(${name} PRIVATE breeze_thread)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${BREEZE_TEST_ENV}")
  endfunction()

  breeze_thread_test(thread_test)
  breeze_thread_test(current_thread_test)
  breeze_thread_test(spin_lock_test)
  breeze_thread_test(wait_group_test)
  breeze_thread_test(thread_pool_test)

  # base/backtrace 与 base/assert 的测试。符号名要 -rdynamic（ENABLE_EXPORTS），否则 dladdr 看不到可执行文件自己的函数。
  # logger 是 breeze_assert 的 PRIVATE 依赖，测试要读日志写到 stderr 的内容，不需要直接链它。
  add_executable(backtrace_test ${BACKTRACE_DIR}/backtrace_test.cpp)
  target_link_libraries(backtrace_test PRIVATE breeze_backtrace)
  set_target_properties(backtrace_test PROPERTIES ENABLE_EXPORTS ON)
  target_compile_options(backtrace_test PRIVATE -Wall -Wextra)
  add_test(NAME backtrace_test COMMAND backtrace_test)
  set_tests_properties(backtrace_test PROPERTIES ENVIRONMENT "${BREEZE_TEST_ENV}")

  foreach(t assert_test assert_fatal_test)
    add_executable(${t} ${ASSERT_DIR}/${t}.cpp)
    target_link_libraries(${t} PRIVATE breeze_assert Threads::Threads)
    set_target_properties(${t} PROPERTIES ENABLE_EXPORTS ON)
    target_compile_options(${t} PRIVATE -Wall -Wextra)
    add_test(NAME ${t} COMMAND ${t})
    set_tests_properties(${t} PROPERTIES ENVIRONMENT "${BREEZE_TEST_ENV}")
  endforeach()

  breeze_net_test(endpoint_test DIR endpoint)
  breeze_net_test(codec_test DIR codec)
  breeze_net_test(unique_fd_test DIR socket)
  breeze_net_test(event_loop_test DIR loop)
  breeze_net_test(connection_table_test DIR loop)
  breeze_net_test(group_registry_test DIR loop)
  breeze_net_test(stream_socket_test DIR stream/internal)
  breeze_net_test(stream_socket_config_test DIR stream/internal)
  breeze_net_test(tcp_endpoint_test DIR tcp)
  breeze_net_test(uds_endpoint_test DIR uds)
  breeze_net_test(rudp_endpoint_test DIR rudp)
  breeze_net_test(socket_address_test DIR socket)
  breeze_net_test(tcp_server_test DIR tcp)
  breeze_net_test(uds_server_test DIR uds)
  breeze_net_test(event_loop_group_test DIR loop)
  breeze_net_test(connection_handle_test DIR connection)
  breeze_net_test(connection_test DIR connection)
  breeze_net_test(stream_server_test DIR stream)
  breeze_net_test(stream_client_test DIR stream)
  breeze_net_test(echo_test DIR testing)
  breeze_net_test(net_flow_test DIR testing)
  breeze_net_test(net_lifecycle_test DIR testing)

  # rudp：协议与票据表是纯逻辑，不依赖 KCP；其余经 breeze_net_rudp。
  breeze_net_test(rudp_protocol_test DIR rudp LIB breeze_net_rudp)
  breeze_net_test(ticket_table_test DIR rudp/internal LIB breeze_net_rudp)
  breeze_net_test(kcp_session_test DIR rudp/internal LIB breeze_net_rudp)
  breeze_net_test(udp_socket_test DIR rudp/internal LIB breeze_net_rudp)
  breeze_net_test(rudp_server_test DIR rudp LIB breeze_net_rudp)
  breeze_net_test(rudp_client_test DIR rudp LIB breeze_net_rudp)
  breeze_net_test(rudp_test DIR testing LIB breeze_net_rudp)

  # framework/net 的测试复用 base/net 的测试公共件（net_test_util.h 带 event_loop.h 与 stream_socket.h）。
  function(breeze_net_module_test name)
    add_executable(${name} ${NET_MODULE_DIR}/${name}.cpp)
    target_link_libraries(${name} PRIVATE breeze_net_module)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${BREEZE_TEST_ENV}")
  endfunction()

  breeze_net_module_test(main_loop_executor_test)
  breeze_net_module_test(net_module_test)
  # NetModule(app) 的用例读仓库里真实的 config/common.yaml（net 段）
  target_compile_definitions(net_module_test PRIVATE BREEZE_CONFIG_DIR="${CONFIG_SCHEMA_ROOT}/..")

  # net 库内不抛、不 catch（设计 §4.11）。base/buffer 与 base/thread 是 net 的依赖（SharedBuffer 在 IO 线程分配、Thread 创建失败不抛），一并扫。
  add_test(NAME net_no_exceptions_test
    COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/${NET_TESTING_DIR}/check_no_exceptions.py
            ${CMAKE_SOURCE_DIR}/${NET_DIR} ${CMAKE_SOURCE_DIR}/${BUFFER_DIR} ${CMAKE_SOURCE_DIR}/${THREAD_DIR}
            ${CMAKE_SOURCE_DIR}/framework/net)
endif()

# 容器 bench：-DBREEZE_BUILD_CONTAINER_BENCH=ON 才构建（不联网）。用 Release 预设跑：cmake --preset release -DBREEZE_BUILD_CONTAINER_BENCH=ON
option(BREEZE_BUILD_CONTAINER_BENCH "构建 container_bench（FlatHashMap / LinkedHashMap 对比 std::unordered_map，用到 glibc 的 mallinfo2）" OFF)
if(BREEZE_BUILD_CONTAINER_BENCH AND BREEZE_BUILD_NET)
  add_executable(container_bench bench/container_bench.cpp)
  target_link_libraries(container_bench PRIVATE breeze_container)
  target_compile_options(container_bench PRIVATE -Wall -Wextra)
endif()

# 基线 bench：-DBREEZE_BUILD_BENCH=ON 才构建，默认关闭（要联网拉 muduo 源码）。
# 用 Release 预设跑：cmake --preset release -DBREEZE_BUILD_BENCH=ON
option(BREEZE_BUILD_BENCH "构建 net_bench（含 muduo 基线，需联网拉取源码）" OFF)
if(BREEZE_BUILD_BENCH AND BREEZE_BUILD_NET)
  include(FetchContent)
  # muduo 自己的 CMake 强制要求 Boost，还带 -Werror -Wconversion -std=c++11，所以只取源码：
  # SOURCE_SUBDIR 指向一个不存在的目录，FetchContent 就不会执行它的 CMakeLists。核心的 base/ 与 net/ 自己编。
  FetchContent_Declare(muduo
    GIT_REPOSITORY https://github.com/chenshuo/muduo.git
    GIT_TAG f1fc77e0c13b80e5086ff457362c8a86d1b609d4
    SOURCE_SUBDIR breeze-no-cmake)
  FetchContent_MakeAvailable(muduo)
  file(GLOB MUDUO_SRCS
    ${muduo_SOURCE_DIR}/muduo/base/*.cc
    ${muduo_SOURCE_DIR}/muduo/net/*.cc
    ${muduo_SOURCE_DIR}/muduo/net/poller/*.cc)
  # AsyncLogging 用 Boost 的 circular_buffer，bench 用不到异步日志，直接排除。
  list(FILTER MUDUO_SRCS EXCLUDE REGEX "/AsyncLogging\\.cc$")
  # muduo/net/boilerplate.cc 是个新建文件用的模板，include 的头文件大小写是 BoilerPlate.h，区分大小写的文件系统上找不到。
  list(FILTER MUDUO_SRCS EXCLUDE REGEX "/boilerplate\\.cc$")
  add_library(muduo_bench STATIC ${MUDUO_SRCS})
  # bench/muduo_shim 提供 boost/any.hpp（映射到 std::any），不引入 Boost。
  target_include_directories(muduo_bench PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/bench/muduo_shim ${muduo_SOURCE_DIR})
  target_link_libraries(muduo_bench PUBLIC Threads::Threads)
  set_target_properties(muduo_bench PROPERTIES CXX_STANDARD 17)
  # muduo 的 Timestamp.cc 没有自己 include <ctime>（以前靠 Boost 间接带进来），强制补上。别人的代码，不看它的警告。
  target_compile_options(muduo_bench PRIVATE -w -include ctime)

  add_executable(net_bench bench/net_bench.cpp)
  target_link_libraries(net_bench PRIVATE breeze_net breeze_net_rudp breeze_net_module breeze_logger muduo_bench)
  target_compile_definitions(net_bench PRIVATE BREEZE_BENCH_MUDUO=1)
  target_compile_options(net_bench PRIVATE -Wall -Wextra)
endif()
