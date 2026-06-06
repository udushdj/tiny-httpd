cmake_minimum_required(VERSION 3.10)
project(tiny-httpd C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ──── 编译选项 ────
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pthread -U_FORTIFY_SOURCE")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ──── 源文件 ────
set(SOURCES
    src/main.c
    src/server.c
    src/http_parser.c
    src/logger.c
)

# ──── 可执行文件 ────
add_executable(tiny-httpd ${SOURCES})
target_link_libraries(tiny-httpd pthread)

# ──── 自定义目标 ────
# 编译 CGI 测试程序
add_executable(cgi-test www/cgi-bin/test.c)
set_target_properties(cgi-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/www/cgi-bin
)

# 编译压力测试工具
add_executable(stress-test test/stress.c)
target_link_libraries(stress-test pthread)
set_target_properties(stress-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/test
)

add_executable(webbench test/webbench.c)
target_link_libraries(webbench pthread)
set_target_properties(webbench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/test
)

# ──── ARM 交叉编译工具链 ────
# 使用方式: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/arm-toolchain.cmake ..
option(ARM_CROSS_COMPILE "Enable ARM cross compilation" OFF)

if(ARM_CROSS_COMPILE)
    set(CMAKE_SYSTEM_NAME Linux)
    set(CMAKE_SYSTEM_PROCESSOR arm)
    set(CMAKE_C_COMPILER "/home/wz/usr/local/arm/5.4.0/usr/bin/arm-linux-gcc")
    set(CMAKE_FIND_ROOT_PATH "/home/wz/usr/local/arm/5.4.0")
endif()

# ──── 安装 ────
install(TARGETS tiny-httpd DESTINATION bin)
install(DIRECTORY www/ DESTINATION share/tiny-httpd/www)
