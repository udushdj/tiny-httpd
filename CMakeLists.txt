cmake_minimum_required(VERSION 3.12)

# ---------- 交叉工具链路径 ----------
set(CMAKE_C_COMPILER   "/home/wz/usr/local/arm/5.4.0/usr/bin/arm-linux-gcc")
set(CMAKE_CXX_COMPILER "/home/wz/usr/local/arm/5.4.0/usr/bin/arm-linux-g++")

set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)

project(tiny-httpd LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 可执行文件放到项目根目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR})

# ===================== 源文件 =====================
# tiny-httpd 自身源文件
file(GLOB HTTPD_SRCS
    "src/main.c"
    "src/server.c"
    "src/logger.c"
    "src/http_parser.c"
    "src/gui.c"
)

# LVGL 源码（与 lvgl_fb 项目共享，路径按需修改）
set(LVGL_ROOT "/home/wz/codepath/lvgl_fb")
if(EXISTS ${LVGL_ROOT})
    file(GLOB_RECURSE LVGL_SRCS
        "${LVGL_ROOT}/lvgl/src/*.c"
        "${LVGL_ROOT}/lvgl/src/**/*.c"
        "${LVGL_ROOT}/lv_drivers/*.c"
        "${LVGL_ROOT}/lv_drivers/**/*.c"
    )
    file(GLOB LVGL_FONT_SRCS
        "${LVGL_ROOT}/my_font_cn_48.c"
        "${LVGL_ROOT}/my_font_cn.c"
        "${LVGL_ROOT}/mouse_cursor_icon.c"
    )
    list(APPEND HTTPD_SRCS ${LVGL_SRCS} ${LVGL_FONT_SRCS})
    message(STATUS "LVGL 源码已包含: ${LVGL_ROOT}")
else()
    message(WARNING "LVGL 路径不存在: ${LVGL_ROOT}, 使用无GUI模式编译")
    add_compile_definitions(ENABLE_LVGL=0)
endif()

add_executable(${PROJECT_NAME} ${HTTPD_SRCS})

# ===================== 头文件路径 =====================
target_include_directories(${PROJECT_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
    /home/wz/usr/local/openssl-arm/include
)

# LVGL 头文件
if(EXISTS ${LVGL_ROOT})
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${LVGL_ROOT}
        ${LVGL_ROOT}/lvgl
        ${LVGL_ROOT}/lv_drivers
    )
endif()

# ===================== 编译定义 =====================
target_compile_definitions(${PROJECT_NAME} PRIVATE
    LV_CONF_INCLUDE_SIMPLE
    LV_LVGL_H_INCLUDE_SIMPLE
)

# ===================== OpenSSL 库路径 =====================
target_link_directories(${PROJECT_NAME} PRIVATE
    /home/wz/usr/local/openssl-arm/lib
)

# ===================== 链接库 =====================
target_link_libraries(${PROJECT_NAME}
    pthread
    m
    rt
    dl
    ssl
    crypto
)

# ===================== 编译选项 =====================
target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall
    -Wextra
    -O2
    -g
)
