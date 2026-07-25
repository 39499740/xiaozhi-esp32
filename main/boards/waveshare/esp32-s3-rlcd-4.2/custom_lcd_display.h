#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <mutex>
#include <string>
#include "lcd_display.h"
#include "dashboard_layout.h"

enum ColorSelection {
    ColorBlack = 0,    
    ColorWhite = 0xff
};

typedef struct {
    uint8_t mosi;
    uint8_t scl;
    uint8_t dc;
    uint8_t cs;
    uint8_t rst;
} spi_display_config_t;

class CustomLcdDisplay : public LcdDisplay {
private:
    esp_lcd_panel_io_handle_t io_handle = NULL;
    uint32_t            i2c_data_pdMS_TICKS = 0;
    uint32_t            i2c_done_pdMS_TICKS = 0;
    const char         *TAG                 = "CustomDisplay";
    int                 mosi_;
    int                 scl_;
    int                 dc_;
    int                 cs_;
    int                 rst_;
    int                 width_;
    int                 height_;
    uint8_t            *DispBuffer = NULL;
    int                 DisplayLen;
	uint16_t (*PixelIndexLUT)[300];
	uint8_t  (*PixelBitLUT  )[300];
	void InitPortraitLUT();
	void InitLandscapeLUT();
    void Set_ResetIOLevel(uint8_t level);
    void RLCD_SendCommand(uint8_t Reg);
    void RLCD_SendData(uint8_t Data);
    void RLCD_Sendbuffera(uint8_t *Data, int len);
    void RLCD_Reset(void);
    static void Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p);

    // ---- 待机仪表盘 ----
    DashboardState dashboard_state_;        // 布局状态（共享给 dashboard_layout）
    esp_timer_handle_t clock_timer_ = nullptr;  // ESP 专属：1 秒刷新时钟
    TaskHandle_t holiday_task_ = nullptr;       // 后台查询年度调休日历
    std::atomic_bool holiday_task_running_{false};
    std::atomic_bool holiday_task_stop_{false};
    std::mutex holiday_mutex_;
    int pending_holiday_year_ = 0;
    std::string pending_holiday_flags_;
    bool pending_holiday_ready_ = false;
    int pending_holiday_note_year_ = 0;
    int pending_holiday_note_day_of_year_ = 0;
    std::string pending_holiday_note_;
    bool pending_holiday_note_ready_ = false;
    std::string pending_weather_city_;
    std::string pending_weather_temperature_;
    std::string pending_weather_feels_like_;
    std::string pending_weather_high_temperature_;
    std::string pending_weather_low_temperature_;
    std::string pending_weather_description_;
    std::string pending_weather_notice_;
    bool pending_weather_ready_ = false;
    std::string pending_weather_city_only_;
    bool pending_weather_city_only_ready_ = false;
    std::string weather_location_;
    std::string weather_city_;
    // QWeather's minutely and alert endpoints require coordinates. Keep the
    // resolved longitude,latitude pair separately from the city Location ID.
    std::string weather_coordinates_;
    std::string weather_precipitation_notice_;
    std::string weather_alert_notice_;
    std::string weather_notice_;
    std::atomic_bool weather_location_refresh_requested_{false};
    bool weather_location_manual_ = false;
    bool weather_location_ready_ = false;
    lv_obj_t* battery_percent_label_ = nullptr;
    int displayed_battery_percent_ = -1;
    void SetupDashboard();
    static void ClockTimerCallback(void* arg);
    static void HolidayCalendarTask(void* arg);
    bool FetchHolidayCalendar();
    bool ResolveWeatherLocation();
    bool FetchWeather();
    bool FetchWeatherNotices();
    void ApplyPendingDashboardData();
    void StopHolidayCalendarTask();
    void ShowDashboard();
    void HideDashboard();
    void HideDefaultEmotion();

public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy,spi_display_config_t spiconfig,spi_host_device_t spi_host = SPI3_HOST);
    ~CustomLcdDisplay();
    void RLCD_Init();
    void RLCD_ColorClear(uint8_t color);
    void RLCD_Display();
	void RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color);

    // 重写：SetupUI 完成后挂上仪表盘；SetStatus 用来感知待机切换
    virtual void SetupUI() override;
    virtual void SetStatus(const char* status) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void UpdateStatusBar(bool update_all = false) override;

    // Called by the device MCP tool after the user asks the assistant to
    // update the weather location. The worker performs all network I/O.
    bool RequestWeatherLocationUpdate();
};

#endif
