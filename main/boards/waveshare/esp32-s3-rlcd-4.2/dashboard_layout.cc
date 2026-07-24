#include "dashboard_layout.h"

#include <cstring>
#include <ctime>

// 星期名称（中文）—— 定义（原 custom_lcd_display.cc:20-21）
const char* kWeekdays[] = {"星期日", "星期一", "星期二", "星期三",
                           "星期四", "星期五", "星期六"};

namespace dashboard_layout {

void Setup(DashboardState& s, lv_obj_t* parent, int width, int height, const lv_font_t* font) {
    s.width = width;
    s.height = height;

    // 仪表盘根容器：覆盖整个屏幕，置于所有聊天 UI 之上
    s.root = lv_obj_create(parent);
    lv_obj_set_size(s.root, width, height);
    lv_obj_set_pos(s.root, 0, 0);
    lv_obj_set_style_bg_opa(s.root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s.root, 0, 0);
    lv_obj_set_style_pad_all(s.root, 0, 0);
    lv_obj_set_style_radius(s.root, 0, 0);
    lv_obj_set_scrollbar_mode(s.root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s.root);

    // 左侧日历区 —— 日期放大显示
    s.date_label = lv_label_create(s.root);
    lv_obj_set_style_text_font(s.date_label, font, 0);
    lv_obj_set_style_transform_scale(s.date_label, 256 * 2, 0);
    lv_obj_align(s.date_label, LV_ALIGN_LEFT_MID, 25, -40);
    lv_label_set_text(s.date_label, "2026/01/01");

    s.weekday_label = lv_label_create(s.root);
    lv_obj_set_style_text_font(s.weekday_label, font, 0);
    lv_obj_set_style_transform_scale(s.weekday_label, 256 * 2, 0);
    lv_obj_align(s.weekday_label, LV_ALIGN_LEFT_MID, 25, 15);
    lv_label_set_text(s.weekday_label, "星期一");

    // 右上：时钟（放大显示）
    s.clock_label = lv_label_create(s.root);
    lv_obj_set_style_text_font(s.clock_label, font, 0);
    lv_obj_set_style_transform_scale(s.clock_label, 256 * 3, 0);
    lv_obj_align(s.clock_label, LV_ALIGN_TOP_RIGHT, -20, 55);
    lv_label_set_text(s.clock_label, "00:00");

    // 右下：天气（温度+文字，占位）
    s.weather_label = lv_label_create(s.root);
    lv_obj_set_style_text_font(s.weather_label, font, 0);
    lv_obj_set_style_transform_scale(s.weather_label, 256 * 2, 0);
    lv_obj_align(s.weather_label, LV_ALIGN_BOTTOM_RIGHT, -20, -50);
    lv_label_set_text(s.weather_label, "--°");

    // 右下：城市名（小字，不放大）
    s.city_label = lv_label_create(s.root);
    lv_obj_set_style_text_font(s.city_label, font, 0);
    lv_obj_align(s.city_label, LV_ALIGN_BOTTOM_RIGHT, -20, -15);
    lv_label_set_text(s.city_label, "定位中...");

    // 默认先隐藏，等进入待机状态时显示
    Hide(s);

    // 立即刷新一次时钟显示
    Refresh(s);
}

void Refresh(DashboardState& s) {
    if (s.clock_label == nullptr) return;

    time_t now = time(nullptr);
    struct tm timeinfo;
    // 用 localtime_r（预览和固件都适用：固件因 ota.cc 已写入本地时间，localtime_r 返回本地时间）
    localtime_r(&now, &timeinfo);

    char time_buf[16];
    char date_buf[16];
    strftime(time_buf, sizeof(time_buf), "%H:%M", &timeinfo);
    strftime(date_buf, sizeof(date_buf), "%Y/%m/%d", &timeinfo);

    if (s.clock_label != nullptr) {
        lv_label_set_text(s.clock_label, time_buf);
    }
    if (s.date_label != nullptr) {
        lv_label_set_text(s.date_label, date_buf);
    }
    if (s.weekday_label != nullptr && timeinfo.tm_wday >= 0 && timeinfo.tm_wday <= 6) {
        lv_label_set_text(s.weekday_label, kWeekdays[timeinfo.tm_wday]);
    }
}

void Show(DashboardState& s) {
    if (s.root == nullptr) return;
    lv_obj_remove_flag(s.root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s.root);
    Refresh(s);
}

void Hide(DashboardState& s) {
    if (s.root == nullptr) return;
    lv_obj_add_flag(s.root, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace dashboard_layout
