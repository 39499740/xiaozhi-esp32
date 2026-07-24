#include "dashboard_layout.h"

#include <cstdio>
#include <cstring>
#include <ctime>

// 仪表盘使用专用的常用汉字字库，覆盖日历、天气以及常见城市名。
// 该字库为 1bpp，适合 RLCD 黑白显示，同时避免把 4bpp 全字库带进固件。
LV_FONT_DECLARE(rlcd_chinese_20_1);

// 星期名称（中文）—— 定义（原 custom_lcd_display.cc:20-21）
const char* kWeekdays[] = {"星期日", "星期一", "星期二", "星期三",
                           "星期四", "星期五", "星期六"};

namespace dashboard_layout {

namespace {

constexpr int kCalendarColumns = 7;
constexpr int kCalendarRows = 6;
constexpr int kCalendarCells = kCalendarColumns * kCalendarRows;
constexpr int kCalendarX = 8;
constexpr int kCalendarWidth = 224;
constexpr int kCalendarCellWidth = kCalendarWidth / kCalendarColumns;
// LcdDisplay creates a 30px status-icon row with 4px padding above and below.
// Keep the dashboard below that row on both the real RLCD and the preview.
constexpr int kTopBarHeight = 38;
constexpr int kCalendarTitleY = 40;
constexpr int kCalendarWeekdayY = 73;
constexpr int kCalendarDaysY = 99;
constexpr int kCalendarCellHeight = 25;
constexpr int kCalendarLegendY = 265;
constexpr int kRightPanelX = 242;
constexpr int kRightPanelWidth = 150;
constexpr int kClockCaptionY = 40;
constexpr int kClockY = 62;
constexpr int kClockDividerY = 108;
constexpr int kWeatherCaptionY = 114;
constexpr int kWeatherY = 138;
constexpr int kWeatherDescriptionY = 174;
constexpr int kWeatherFeelsLikeY = 200;
constexpr int kWeatherRangeY = 226;
constexpr int kCityY = 252;

const char* kCalendarWeekdays[] = {"日", "一", "二", "三", "四", "五", "六"};

lv_obj_t* CreateTextLabel(lv_obj_t* parent, const lv_font_t* font, int x, int y, int width, int height,
                          int scale) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_size(label, width, height);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(label, 0, 0);
    lv_obj_set_style_radius(label, 0, 0);
    lv_obj_set_style_transform_scale(label, scale, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

lv_obj_t* CreateRule(lv_obj_t* parent, int x, int y, int width, int height) {
    lv_obj_t* rule = lv_obj_create(parent);
    lv_obj_set_size(rule, width, height);
    lv_obj_set_pos(rule, x, y);
    lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_radius(rule, 0, 0);
    lv_obj_set_style_pad_all(rule, 0, 0);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
    return rule;
}

int DaysInMonth(int year, int month) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        return 29;
    }
    return kDays[month - 1];
}

int WeekdayOfDate(int year, int month, int day) {
    struct tm date = {};
    date.tm_year = year - 1900;
    date.tm_mon = month - 1;
    date.tm_mday = day;
    mktime(&date);
    return date.tm_wday;
}

int DayOfYear(int year, int month, int day) {
    int result = day;
    for (int current_month = 1; current_month < month; ++current_month) {
        result += DaysInMonth(year, current_month);
    }
    return result;
}

bool HasHolidayCalendar(const DashboardState& s, int year) {
    return s.holiday_calendar_ready && s.holiday_calendar_year == year &&
           s.holiday_calendar_length >= 365;
}

bool IsScheduledWorkday(const DashboardState& s, int year, int month, int day) {
    if (!HasHolidayCalendar(s, year)) return false;
    const int day_of_year = DayOfYear(year, month, day);
    if (day_of_year < 1 || static_cast<size_t>(day_of_year) > s.holiday_calendar_length) return false;
    return s.holiday_calendar_flags[day_of_year - 1] == '1';
}

bool IsDayOff(const DashboardState& s, int year, int month, int day, int weekday) {
    if (HasHolidayCalendar(s, year)) {
        // 年度表同时包含法定节假日和调休上班日。只要标记为休息日，
        // 不论落在周末还是工作日，都用反显标记；调休上班日则恢复正常色。
        return !IsScheduledWorkday(s, year, month, day);
    }

    // 网络尚未返回年度表时，先按普通周末显示，法定节假日等待年度数据。
    return weekday == 0 || weekday == 6;
}

void SetDayStyle(lv_obj_t* label, bool inverted) {
    if (inverted) {
        lv_obj_set_style_bg_color(label, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_set_style_radius(label, 3, 0);
    } else {
        lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(label, lv_color_black(), 0);
        lv_obj_set_style_radius(label, 0, 0);
    }
}

void SetTodayStyle(lv_obj_t* label, bool today, bool inverted) {
    // 当天只画日期下方一条横线，不再额外画框；反显日期使用白线保持可见。
    lv_obj_set_style_border_color(label, inverted ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_border_side(label, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_opa(label, today ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(label, today ? 1 : 0, 0);
}

}  // namespace

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

    // 左侧：标准月历。中文标题和星期使用专用常用字库。
    const lv_font_t* chinese_font = &rlcd_chinese_20_1;
    s.calendar_title = CreateTextLabel(s.root, chinese_font, kCalendarX, kCalendarTitleY, kCalendarWidth, 30, 256);
    lv_label_set_text(s.calendar_title, "----年--月");
    CreateRule(s.root, kCalendarX, 70, kCalendarWidth, 1);

    for (int column = 0; column < kCalendarColumns; ++column) {
        const int x = kCalendarX + column * kCalendarCellWidth;
        s.calendar_weekdays[column] = CreateTextLabel(s.root, chinese_font, x, kCalendarWeekdayY,
                                                      kCalendarCellWidth, 24, 256);
        lv_label_set_text(s.calendar_weekdays[column], kCalendarWeekdays[column]);
        // 日、六表头与对应日期保持同样的周末反显样式。
        SetDayStyle(s.calendar_weekdays[column], column == 0 || column == 6);
    }

    for (int index = 0; index < kCalendarCells; ++index) {
        const int column = index % kCalendarColumns;
        const int row = index / kCalendarColumns;
        s.calendar_days[index] = CreateTextLabel(
            s.root, chinese_font, kCalendarX + column * kCalendarCellWidth, kCalendarDaysY + row * kCalendarCellHeight,
            kCalendarCellWidth, kCalendarCellHeight, 256);
        lv_label_set_text(s.calendar_days[index], "");
    }
    s.calendar_legend = CreateTextLabel(s.root, chinese_font, kCalendarX, kCalendarLegendY, kCalendarWidth, 30, 256);
    lv_label_set_text(s.calendar_legend, "周末/节日");

    // 中间分隔线，避免日历和右侧信息互相抢空间。
    CreateRule(s.root, 234, kTopBarHeight, 1, 254);

    // 右上：电子表。
    s.clock_caption = CreateTextLabel(s.root, chinese_font, kRightPanelX, kClockCaptionY, kRightPanelWidth, 26, 256);
    lv_label_set_text(s.clock_caption, "时间");
    s.clock_label = CreateTextLabel(s.root, font, kRightPanelX, kClockY, kRightPanelWidth, 42, 256);
    lv_label_set_text(s.clock_label, "00:00");
    CreateRule(s.root, kRightPanelX, kClockDividerY, kRightPanelWidth, 1);

    // 右下：天气卡片。当前温度使用大号数字，其余信息按重要性从上到下排列。
    s.weather_caption = CreateTextLabel(s.root, chinese_font, kRightPanelX, kWeatherCaptionY, kRightPanelWidth, 26, 256);
    lv_label_set_text(s.weather_caption, "天气");
    s.weather_label = CreateTextLabel(s.root, font, kRightPanelX, kWeatherY, kRightPanelWidth, 36, 256);
    lv_label_set_text(s.weather_label, "-- C");
    s.weather_desc_label = CreateTextLabel(s.root, chinese_font, kRightPanelX, kWeatherDescriptionY, kRightPanelWidth, 26, 256);
    lv_label_set_text(s.weather_desc_label, "定位中");
    s.weather_feels_like_label = CreateTextLabel(s.root, chinese_font, kRightPanelX, kWeatherFeelsLikeY, kRightPanelWidth, 26, 256);
    lv_label_set_text(s.weather_feels_like_label, "体感 --℃");
    s.weather_range_label = CreateTextLabel(s.root, chinese_font, kRightPanelX, kWeatherRangeY, kRightPanelWidth, 26, 256);
    lv_label_set_text(s.weather_range_label, "--℃～--℃");
    s.city_label = CreateTextLabel(s.root, chinese_font, kRightPanelX, kCityY, kRightPanelWidth, 26, 256);
    lv_label_set_text(s.city_label, "定位中");

    // 默认先隐藏，等进入待机状态时显示
    Hide(s);

    // 立即刷新一次时钟显示
    Refresh(s);
}

void Refresh(DashboardState& s) {
    if (s.root == nullptr) return;

    time_t now = time(nullptr);
    struct tm timeinfo;
    // 用 localtime_r（预览和固件都适用：固件因 ota.cc 已写入本地时间，localtime_r 返回本地时间）
    localtime_r(&now, &timeinfo);

    char time_buf[16];
    char month_buf[32];
    strftime(time_buf, sizeof(time_buf), "%H:%M", &timeinfo);
    const int year = timeinfo.tm_year + 1900;
    const int month = timeinfo.tm_mon + 1;
    const int today = timeinfo.tm_mday;
    const int first_weekday = WeekdayOfDate(year, month, 1);
    const int days = DaysInMonth(year, month);

    if (s.clock_label != nullptr) {
        lv_label_set_text(s.clock_label, time_buf);
    }
    if (s.calendar_title != nullptr) {
        std::snprintf(month_buf, sizeof(month_buf), "%04d年%02d月", year, month);
        lv_label_set_text(s.calendar_title, month_buf);
    }
    for (int index = 0; index < kCalendarCells; ++index) {
        if (s.calendar_days[index] == nullptr) continue;
        const int day = index - first_weekday + 1;
        if (day < 1 || day > days) {
            lv_label_set_text(s.calendar_days[index], "");
            SetDayStyle(s.calendar_days[index], false);
            SetTodayStyle(s.calendar_days[index], false, false);
            continue;
        }
        const int weekday = (first_weekday + day - 1) % kCalendarColumns;
        char day_buf[12];
        std::snprintf(day_buf, sizeof(day_buf), "%d", day);
        lv_label_set_text(s.calendar_days[index], day_buf);
        // 周末、法定休息日统一反显；调休上班日恢复正常色。
        const bool day_off = IsDayOff(s, year, month, day, weekday);
        SetDayStyle(s.calendar_days[index], day_off);
        SetTodayStyle(s.calendar_days[index], day == today, day_off);
    }
}

void SetWeather(DashboardState& s, const char* city, const char* temperature,
                const char* feels_like, const char* high_temperature,
                const char* low_temperature, const char* description) {
    if (s.city_label != nullptr) {
        lv_label_set_text(s.city_label, city != nullptr ? city : "定位中");
    }
    if (s.weather_label != nullptr) {
        lv_label_set_text(s.weather_label, temperature != nullptr ? temperature : "-- C");
    }
    if (s.weather_desc_label != nullptr) {
        lv_label_set_text(s.weather_desc_label, description != nullptr ? description : "定位中");
    }
    if (s.weather_feels_like_label != nullptr) {
        char feels_like_buf[48];
        std::snprintf(feels_like_buf, sizeof(feels_like_buf), "体感 %s",
                      feels_like != nullptr ? feels_like : "--℃");
        lv_label_set_text(s.weather_feels_like_label, feels_like_buf);
    }
    if (s.weather_range_label != nullptr) {
        char range_buf[64];
        // 和风接口按最高/最低传入，屏幕按用户习惯显示为最低～最高。
        std::snprintf(range_buf, sizeof(range_buf), "%s～%s",
                      low_temperature != nullptr ? low_temperature : "--℃",
                      high_temperature != nullptr ? high_temperature : "--℃");
        lv_label_set_text(s.weather_range_label, range_buf);
    }
}

void SetWeatherCity(DashboardState& s, const char* city) {
    if (s.city_label != nullptr) {
        lv_label_set_text(s.city_label, city != nullptr && city[0] != '\0' ? city : "定位中");
    }
}

bool SetHolidayCalendar(DashboardState& s, int year, const char* workday_flags, size_t length) {
    if (year < 1970 || workday_flags == nullptr || length < 365 || length > sizeof(s.holiday_calendar_flags)) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        if (workday_flags[index] != '0' && workday_flags[index] != '1') return false;
    }

    std::memset(s.holiday_calendar_flags, 0, sizeof(s.holiday_calendar_flags));
    std::memcpy(s.holiday_calendar_flags, workday_flags, length);
    s.holiday_calendar_year = year;
    s.holiday_calendar_length = length;
    s.holiday_calendar_ready = true;
    return true;
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
