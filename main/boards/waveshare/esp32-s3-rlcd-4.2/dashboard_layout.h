#ifndef DASHBOARD_LAYOUT_H
#define DASHBOARD_LAYOUT_H

#include <lvgl.h>

// dashboard 布局状态：所有 LVGL 对象句柄 + 尺寸。
// 由调用方持有，布局函数无状态、可重入、不依赖任何类继承。
struct DashboardState {
    lv_obj_t* root = nullptr;
    lv_obj_t* clock_label = nullptr;
    lv_obj_t* date_label = nullptr;
    lv_obj_t* weekday_label = nullptr;
    lv_obj_t* weather_label = nullptr;
    lv_obj_t* city_label = nullptr;
    int width = 0;
    int height = 0;
};

// 星期名称（中文）。声明在这里供固件和预览共用。
extern const char* kWeekdays[7];

namespace dashboard_layout {

// 在 parent 下创建 dashboard UI（原 SetupDashboard 逻辑，布局逐字搬迁）。
// font 由调用方传入：固件传 &BUILTIN_TEXT_FONT，预览传 &font_noto_sans_basic_14_1。
// 注意：此函数仅创建 LVGL 对象，不创建任何定时器（定时器由调用方负责）。
void Setup(DashboardState& s, lv_obj_t* parent, int width, int height, const lv_font_t* font);

// 刷新时钟/日期文本（原 RefreshClock 的 LVGL 部分）。
// 调用方负责加锁（固件用 DisplayLockGuard）和定时驱动（固件用 esp_timer，预览用 lv_timer）。
void Refresh(DashboardState& s);

// 显示/隐藏 dashboard（纯 LVGL flag 操作）。
void Show(DashboardState& s);
void Hide(DashboardState& s);

}  // namespace dashboard_layout

#endif  // DASHBOARD_LAYOUT_H
