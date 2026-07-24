// Mac LVGL+SDL 预览:RLCD-4.2 dashboard 布局
// 使用 LVGL 9.5 内置 SDL 驱动,无需手写 flush callback。
#define SDL_MAIN_HANDLED  // 避免 SDL 改写 main
#include <lvgl.h>
#include <cstdlib>
#include <cstdio>
#include <material_symbols.h>
#include "dashboard_layout.h"

// 字体:与 RLCD 固件共享同一个 LVGL C 数组
LV_FONT_DECLARE(font_noto_sans_basic_30_4);
LV_FONT_DECLARE(font_material_symbols_30_4);

static const int WIN_W = 400;
static const int WIN_H = 300;

static DashboardState g_state;
static lv_timer_t* g_clock_timer = nullptr;

static void clock_timer_cb(lv_timer_t* t) {
    LV_UNUSED(t);
    dashboard_layout::Refresh(g_state);
}

static void SetupPreviewTopBar(lv_obj_t* screen) {
    // Keep the same creation order as the device: the dashboard is created
    // afterwards and is transparent, so the top bar remains visible beneath
    // its labels wherever the two layers overlap.
    lv_obj_t* top_bar = lv_obj_create(screen);
    lv_obj_set_size(top_bar, WIN_W, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar, 0, 0);
    lv_obj_set_style_bg_opa(top_bar, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(top_bar, lv_color_white(), 0);
    lv_obj_set_style_border_width(top_bar, 0, 0);
    lv_obj_set_style_pad_all(top_bar, 0, 0);
    lv_obj_set_style_pad_top(top_bar, 4, 0);
    lv_obj_set_style_pad_bottom(top_bar, 4, 0);
    lv_obj_set_style_pad_left(top_bar, 8, 0);
    lv_obj_set_style_pad_right(top_bar, 8, 0);
    lv_obj_set_flex_flow(top_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t* network = lv_label_create(top_bar);
    lv_label_set_text(network, MATERIAL_SYMBOLS_WIFI);
    lv_obj_set_style_text_font(network, &font_material_symbols_30_4, 0);
    lv_obj_set_style_text_color(network, lv_color_black(), 0);

    lv_obj_t* right_icons = lv_obj_create(top_bar);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t* battery = lv_label_create(right_icons);
    lv_label_set_text(battery, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_5);
    lv_obj_set_style_text_font(battery, &font_material_symbols_30_4, 0);
    lv_obj_set_style_text_color(battery, lv_color_black(), 0);

    lv_obj_t* battery_percent = lv_label_create(right_icons);
    lv_label_set_text(battery_percent, "86%");
    lv_obj_set_style_text_font(battery_percent, &font_noto_sans_basic_30_4, 0);
    lv_obj_set_style_text_color(battery_percent, lv_color_black(), 0);
    lv_obj_set_style_margin_left(battery_percent, 2, 0);
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

    // 先创建设备端默认 topbar；dashboard 随后创建并与它保持真实的重叠关系。
    SetupPreviewTopBar(lv_screen_active());

    // 创建 dashboard(共享布局代码),大号数字使用 30px 字体，中文标签由仪表盘字体子集提供。
    dashboard_layout::Setup(g_state, lv_screen_active(), WIN_W, WIN_H, &font_noto_sans_basic_30_4);

    // 用 LVGL 原生 timer 替代 esp_timer(1 秒刷新时钟)
    g_clock_timer = lv_timer_create(clock_timer_cb, 1000, nullptr);

    // 默认显示 dashboard
    dashboard_layout::Show(g_state);
    // 预览使用一组与和风实时/预报接口字段一致的样例数据，便于先检查屏幕排版。
    // 城市通过环境变量注入，默认不再假定任何位置：
    //   RLCD_PREVIEW_CITY=济南 ./dashboard_preview
    const char* preview_city = std::getenv("RLCD_PREVIEW_CITY");
    dashboard_layout::SetWeather(g_state, preview_city != nullptr ? preview_city : "定位中", "27 C", "29℃",
                                 "31℃", "23℃", "阴");

    printf("Preview running. Close window to quit.\n");

    // LVGL 9.5 主循环:SDL 驱动内部处理事件,只需调 lv_timer_handler
    while (1) {
        lv_timer_handler();
        lv_delay_ms(5);
    }
    return 0;
}
