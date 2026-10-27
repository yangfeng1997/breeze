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
set(LOGGER_DIR base/logger)
set(KERNEL_DIR framework/kernel)

# CLI11 只 PRIVATE 链接：公开头文件不含它，使用方看不到这个依赖。
# internal/ 里的 args_impl.h 含 CLI11，必须 PRIVATE，不能并入公开目录。
add_library(breeze_args ${ARGS_DIR}/args_parser.cpp)
target_include_directories(breeze_args PUBLIC ${ARGS_DIR} PRIVATE ${ARGS_DIR}/internal)
target_link_libraries(breeze_args PRIVATE CLI11::CLI11)
target_compile_options(breeze_args PRIVATE -Wall -Wextra)

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

# 内核模块：依赖 App（Module 基类）与 Logger；LoggerConfig 出现在 LogModule 的构造函数里，故 PUBLIC。
add_library(breeze_kernel ${KERNEL_DIR}/log_module.cpp)
target_include_directories(breeze_kernel PUBLIC ${KERNEL_DIR})
target_link_libraries(breeze_kernel PUBLIC breeze_app breeze_logger)
target_compile_options(breeze_kernel PRIVATE -Wall -Wextra)
breeze_apply_log_level(breeze_kernel)

add_executable(log_module_test ${KERNEL_DIR}/log_module_test.cpp)
target_link_libraries(log_module_test PRIVATE breeze_kernel)
add_test(NAME log_module_test COMMAND log_module_test)

# 编译期剥离本身的测试：该 TU 自己定义 BREEZE_LOG_ACTIVE_LEVEL，与构建类型无关。
add_executable(logger_strip_test ${LOGGER_DIR}/logger_strip_test.cpp)
target_link_libraries(logger_strip_test PRIVATE breeze_logger)
add_test(NAME logger_strip_test COMMAND logger_strip_test)
