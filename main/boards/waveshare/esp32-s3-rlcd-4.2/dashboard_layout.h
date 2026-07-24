#ifndef DASHBOARD_LAYOUT_H
#define DASHBOARD_LAYOUT_H

#include <cstddef>

#include <lvgl.h>

// dashboard 布局状态：所有 LVGL 对象句柄 + 尺寸。
// 由调用方持有，布局函数无状态、可重入、不依赖任何类继承。
struct DashboardState {
    lv_obj_t* root = nullptr;

    // 左侧月历
    lv_obj_t* calendar_title = nullptr;
    lv_obj_t* calendar_weekdays[7] = {};
    lv_obj_t* calendar_days[42] = {};
    lv_obj_t* calendar_legend = nullptr;

    // 右侧时钟和天气
    lv_obj_t* clock_caption = nullptr;
    lv_obj_t* clock_label = nullptr;
    lv_obj_t* weather_caption = nullptr;
    lv_obj_t* weather_label = nullptr;
    lv_obj_t* weather_desc_label = nullptr;
    lv_obj_t* weather_feels_like_label = nullptr;
    lv_obj_t* weather_range_label = nullptr;
    lv_obj_t* city_label = nullptr;
    int width = 0;
    int height = 0;

    // 当前年度调休日历，仅保存在内存中；设备重启后由网络任务重新获取。
    int holiday_calendar_year = 0;
    size_t holiday_calendar_length = 0;
    bool holiday_calendar_ready = false;
    char holiday_calendar_flags[367] = {};
};

// 星期名称（中文）。声明在这里供固件和预览共用。
extern const char* kWeekdays[7];

namespace dashboard_layout {

// 在 parent 下创建 dashboard UI（原 SetupDashboard 逻辑，布局逐字搬迁）。
// font 由调用方传入：固件传 &BUILTIN_TEXT_FONT，预览传 &font_noto_sans_basic_30_4；
// 中文标题/标签使用本板内置的 dashboard 字体子集。
// 注意：此函数仅创建 LVGL 对象，不创建任何定时器（定时器由调用方负责）。
void Setup(DashboardState& s, lv_obj_t* parent, int width, int height, const lv_font_t* font);

// 刷新日历、时钟文本（原 RefreshClock 的 LVGL 部分）。
// 调用方负责加锁（固件用 DisplayLockGuard）和定时驱动（固件用 esp_timer，预览用 lv_timer）。
void Refresh(DashboardState& s);

// 更新天气区域。数值参数应传入已经带单位的显示文本，例如“27 C”或“23℃”。
void SetWeather(DashboardState& s, const char* city, const char* temperature,
                const char* feels_like, const char* high_temperature,
                const char* low_temperature, const char* description);

// Update the city as soon as IP geolocation succeeds, before the two weather
// API responses finish downloading.
void SetWeatherCity(DashboardState& s, const char* city);

// 应用网络获取的年度工作日位图：按一年中的第 N 天存储 '1'=上班、'0'=休息。
// 数据只写入 DashboardState 内存，不写 NVS 或文件。
bool SetHolidayCalendar(DashboardState& s, int year, const char* workday_flags, size_t length);

// 显示/隐藏 dashboard（纯 LVGL flag 操作）。
void Show(DashboardState& s);
void Hide(DashboardState& s);

}  // namespace dashboard_layout

#endif  // DASHBOARD_LAYOUT_H
