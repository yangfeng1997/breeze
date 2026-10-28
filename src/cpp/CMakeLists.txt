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

find_package(Threads REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
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
set(BREEZE_GEN_DIR ${CMAKE_SOURCE_DIR}/gen)  # 生成代码根目录，不进版本库；include 路径以它为根
set(LOGGER_DIR base/logger)
set(KERNEL_DIR framework/kernel)

# CLI11 只 PRIVATE 链接：公开头文件不含它，使用方看不到这个依赖。
# internal/ 里的 args_impl.h 含 CLI11，必须 PRIVATE，不能并入公开目录。
add_library(breeze_args ${ARGS_DIR}/args_parser.cpp)
target_include_directories(breeze_args PUBLIC ${ARGS_DIR} PRIVATE ${ARGS_DIR}/internal)
target_link_libraries(breeze_args PRIVATE CLI11::CLI11)
target_compile_options(breeze_args PRIVATE -Wall -Wextra)

# breeze_config_schema(<target> SCHEMA_DIR <dir> [GEN_SUBDIR <名>] [INCLUDE <dir>...])
# 用 tools/configgen 把目录下所有 schema（*.proto，不含 options.proto）生成 C++ 头文件，不进版本库。
# <target> 是 INTERFACE 库，自带 breeze_config：
#   给了 GEN_SUBDIR：生成到 ${BREEZE_GEN_DIR}/<名>/，include 根是 ${BREEZE_GEN_DIR}，
#                    用 #include "<名>/<schema 文件名>_config.h"。scripts/configure.py 也写这个目录。
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
    set(include_root ${BREEZE_GEN_DIR})
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
  target_include_directories(${target} INTERFACE ${include_root})
  target_link_libraries(${target} INTERFACE breeze_config)
endfunction()

# yaml-cpp 同理只 PRIVATE：config.h 不含它。
add_library(breeze_config ${CONFIG_DIR}/config.cpp)
target_include_directories(breeze_config PUBLIC ${CONFIG_DIR})
target_link_libraries(breeze_config PRIVATE yaml-cpp::yaml-cpp)
target_compile_options(breeze_config PRIVATE -Wall -Wextra)

# spdlog 同理只 PRIVATE：logger.h / logger_macros.h 不含它，internal/ 里的头含它。
add_library(breeze_logger ${LOGGER_DIR}/logger.cpp)
target_include_directories(breeze_logger PUBLIC ${LOGGER_DIR} PRIVATE ${LOGGER_DIR}/internal)
target_link_libraries(breeze_logger PRIVATE spdlog::spdlog)
target_compile_options(breeze_logger PRIVATE -Wall -Wextra)

add_library(breeze_app
  ${APP_DIR}/app.cpp
  ${APP_DIR}/lifecycle.cpp
  ${APP_DIR}/loop.cpp
  ${APP_DIR}/module.cpp)
target_include_directories(breeze_app PUBLIC ${APP_DIR})
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

# 测试要直接喂 HourlyFileSink，所以也看得到 internal/ 与 spdlog，使用方则不行。
add_executable(logger_test ${LOGGER_DIR}/logger_test.cpp)
target_include_directories(logger_test PRIVATE ${LOGGER_DIR}/internal)
target_link_libraries(logger_test PRIVATE breeze_logger spdlog::spdlog)
add_test(NAME logger_test COMMAND logger_test)

# 真实的 schema（config/schema/）：生成并编译，schema 改坏了构建就失败。
breeze_config_schema(breeze_config_schemas SCHEMA_DIR ${CONFIG_SCHEMA_ROOT} GEN_SUBDIR config)

# 内核模块：依赖 App（Module 基类）与 Logger；LoggerConfig 出现在 LogModule 的构造函数里，故 PUBLIC。
# ConfigModule 的头文件里有 ArgsParser 与生成的 CommonConfig，所以 breeze_args / breeze_config_schemas 也是 PUBLIC。
add_library(breeze_kernel ${KERNEL_DIR}/log_module.cpp ${KERNEL_DIR}/config_module.cpp)
target_include_directories(breeze_kernel PUBLIC ${KERNEL_DIR})
# log_config_yaml.h 同时用到 Logger 与 Config 的类型，所以两者都是 PUBLIC。
target_link_libraries(breeze_kernel PUBLIC breeze_app breeze_logger breeze_config breeze_args breeze_config_schemas)
target_compile_options(breeze_kernel PRIVATE -Wall -Wextra)
breeze_apply_log_level(breeze_kernel)

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
# 都要直接用 internal/ 里的 sink 和测试工具，所以与 logger_test 一样看得到 internal/ 与 spdlog。
add_executable(logger_robust_test ${LOGGER_DIR}/logger_robust_test.cpp)
target_include_directories(logger_robust_test PRIVATE ${LOGGER_DIR}/internal)
target_link_libraries(logger_robust_test PRIVATE breeze_logger spdlog::spdlog Threads::Threads)
add_test(NAME logger_robust_test COMMAND logger_robust_test)

# TZ 环境变量只能在没有其它线程时改，所以单独一个进程。
add_executable(logger_tz_test ${LOGGER_DIR}/logger_tz_test.cpp)
target_include_directories(logger_tz_test PRIVATE ${LOGGER_DIR}/internal)
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

add_executable(common_config_test ${KERNEL_DIR}/common_config_test.cpp)
target_link_libraries(common_config_test PRIVATE breeze_config_schemas)
# 测试读仓库里真实的 config/common.yaml 与 config/env/*.yaml
target_compile_definitions(common_config_test PRIVATE BREEZE_CONFIG_DIR="${CONFIG_SCHEMA_ROOT}/..")
add_test(NAME common_config_test COMMAND common_config_test)
