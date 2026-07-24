cmake_minimum_required(VERSION 3.16)
project(dashboard_preview C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# SDL2(已 brew 安装)
find_package(SDL2 REQUIRED)

# LVGL 9.5 - 用 FetchContent 拉取,锁定到 release/v9.5
include(FetchContent)
FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG release/v9.5
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(lvgl)

# dashboard_layout 与固件共享
set(DASHBOARD_DIR ${CMAKE_SOURCE_DIR}/../main/boards/waveshare/esp32-s3-rlcd-4.2)

# 字体 C 源文件(与固件共享)
set(FONT_DIR ${CMAKE_SOURCE_DIR}/../managed_components/78__xiaozhi-fonts/src)
set(FONT_SRC ${FONT_DIR}/font_noto_sans_basic_14_1.c)

add_executable(dashboard_preview
    main.cc
    ${DASHBOARD_DIR}/dashboard_layout.cc
    ${FONT_SRC}
)

target_include_directories(dashboard_preview PRIVATE
    ${CMAKE_SOURCE_DIR}              # 找到 lv_conf.h
    ${DASHBOARD_DIR}                 # 找到 dashboard_layout.h
)

# LVGL 配置:用我们的 lv_conf.h,且让 LVGL 用 <lvgl.h>
target_compile_definitions(dashboard_preview PRIVATE
    LV_CONF_INCLUDE_SIMPLE=1
    LV_LVGL_H_INCLUDE_SIMPLE=1
)
target_compile_definitions(lvgl PRIVATE
    LV_CONF_INCLUDE_SIMPLE=1
    LV_LVGL_H_INCLUDE_SIMPLE=1
)
# SDL2 include 路径传给 lvgl(内置 SDL 驱动需要 SDL.h)
target_include_directories(lvgl PRIVATE ${SDL2_INCLUDE_DIRS})

# 修正 LVGL 9.5 桌面构建的一个坑:os_desktop.cmake 无条件 glob 了所有 .S 文件,
# 包括 Cortex-M 专用的 helium 汇编(src/draw/sw/blend/helium/lv_blend_helium.S)。
# 在 Apple Silicon 上这个 .S 会被 clang 当汇编编译,指令不被 macOS arm64 识别,导致整个构建崩溃。
# helium 只对 ARM Cortex-M (MVE) 有用,桌面平台不需要,从 lvgl 源里剔除。
get_target_property(_lvgl_sources lvgl SOURCES)
list(FILTER _lvgl_sources EXCLUDE REGEX "/helium/lv_blend_helium\\.S$")
set_target_properties(lvgl PROPERTIES SOURCES "${_lvgl_sources}")

target_link_libraries(dashboard_preview PRIVATE
    lvgl
    ${SDL2_LIBRARIES}
)
