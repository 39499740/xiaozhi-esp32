// Mac LVGL+SDL 预览:RLCD-4.2 dashboard 布局
// 使用 LVGL 9.5 内置 SDL 驱动,无需手写 flush callback。
#define SDL_MAIN_HANDLED  // 避免 SDL 改写 main
#include <lvgl.h>
#include <cstdio>
#include "dashboard_layout.h"

// 字体:与固件共享同一个 LVGL C 数组
LV_FONT_DECLARE(font_noto_sans_basic_14_1);

static const int WIN_W = 400;
static const int WIN_H = 300;

static DashboardState g_state;
static lv_timer_t* g_clock_timer = nullptr;

static void clock_timer_cb(lv_timer_t* t) {
    LV_UNUSED(t);
    dashboard_layout::Refresh(g_state);
}

int main(int argc, char** argv) {
    LV_UNUSED(argc);
    LV_UNUSED(argv);

    lv_init();

    // LVGL 内置 SDL 窗口(9.5 推荐做法,自动处理 flush/buffer/input)
    lv_display_t* disp = lv_sdl_window_create(WIN_W, WIN_H);
    LV_UNUSED(disp);

    // 背景白色(模拟单色屏白底)
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_white(), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

    // 创建 dashboard(共享布局代码),字体用 font_noto_sans_basic_14_1
    dashboard_layout::Setup(g_state, lv_screen_active(), WIN_W, WIN_H, &font_noto_sans_basic_14_1);

    // 用 LVGL 原生 timer 替代 esp_timer(1 秒刷新时钟)
    g_clock_timer = lv_timer_create(clock_timer_cb, 1000, nullptr);

    // 默认显示 dashboard
    dashboard_layout::Show(g_state);

    printf("Preview running. Close window to quit.\n");

    // LVGL 9.5 主循环:SDL 驱动内部处理事件,只需调 lv_timer_handler
    while (1) {
        lv_timer_handler();
        lv_delay_ms(5);
    }
    return 0;
}
