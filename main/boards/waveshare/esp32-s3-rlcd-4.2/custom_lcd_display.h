#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include <driver/gpio.h>
#include <esp_timer.h>
#include <string>
#include "lcd_display.h"

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

    // ---- 待机仪表盘（时钟 / 日历 / 天气）----
    lv_obj_t* dashboard_root_ = nullptr;   // 仪表盘根容器，整体显隐用
    lv_obj_t* clock_label_   = nullptr;    // 右上：时钟
    lv_obj_t* date_label_    = nullptr;    // 左侧：日期
    lv_obj_t* weekday_label_ = nullptr;    // 左侧：星期
    lv_obj_t* weather_label_ = nullptr;    // 右下：天气（温度+文字）
    lv_obj_t* city_label_    = nullptr;    // 右下：城市名
    esp_timer_handle_t clock_timer_ = nullptr;     // 每秒刷新时钟
    esp_timer_handle_t weather_timer_ = nullptr;   // 每 30 分钟刷新天气
    bool weather_ok_ = false;              // 天气是否拉取成功（失败时显示占位）
    std::string city_name_ = "定位中...";  // 当前城市名
    std::string weather_text_ = "--";      // 天气文字
    int weather_temp_ = -999;              // 温度

    void SetupDashboard();
    void RefreshClock();
    static void ClockTimerCallback(void* arg);
    void ShowDashboard();
    void HideDashboard();

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
};

#endif