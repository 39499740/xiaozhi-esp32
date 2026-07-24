#include <vector>
#include <cstring>
#include <ctime>
#include <freertos/FreeRTOS.h>
#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <esp_err.h>
#include "custom_lcd_display.h"
#include "lcd_display.h"
#include "esp_lvgl_port.h"
#include "assets/lang_config.h"
#include "settings.h"
#include "config.h"
#include "board.h"

// BUILTIN_TEXT_FONT 由 CMake 定义（RLCD 板为 font_noto_sans_basic_14_1），保证被编译进来
LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

// 星期名称（中文）
static const char* kWeekdays[] = {"星期日", "星期一", "星期二", "星期三",
                                  "星期四", "星期五", "星期六"};

void CustomLcdDisplay::Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p)
{
    assert(disp != NULL);
    CustomLcdDisplay *Disp = (CustomLcdDisplay *)lv_display_get_user_data(disp);
    uint16_t *buffer = (uint16_t *)color_p;
  	for(int y = area->y1; y <= area->y2; y++)
  	{
  	 	for(int x = area->x1; x <= area->x2; x++) 
  	 	{
  	 	   	uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
  	 	   	Disp->RLCD_SetPixel(x,y,color);
  	 	   	buffer++;
  	 	}
  	}
  	Disp->RLCD_Display();
	lv_disp_flush_ready(disp);
}

CustomLcdDisplay::CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
esp_lcd_panel_handle_t panel,
int width, 
int height, 
int offset_x, 
int offset_y,
bool mirror_x, 
bool mirror_y, 
bool swap_xy,
spi_display_config_t spiconfig,
spi_host_device_t spi_host) : LcdDisplay(panel_io, panel, width, height),
mosi_(spiconfig.mosi),
scl_(spiconfig.scl), 
dc_(spiconfig.dc), 
cs_(spiconfig.cs), 
rst_(spiconfig.rst), 
width_(width), 
height_(height)
{
	ESP_LOGI(TAG, "Initialize SPI");
	esp_err_t        ret;
    spi_bus_config_t buscfg   = {};
    int              transfer = width_ * height_;
    buscfg.miso_io_num                   = -1;
    buscfg.mosi_io_num                   = mosi_;
    buscfg.sclk_io_num                   = scl_;
    buscfg.quadwp_io_num                 = -1;
    buscfg.quadhd_io_num                 = -1;
    buscfg.max_transfer_sz               = transfer;
    ret                                  = spi_bus_initialize(spi_host, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = static_cast<gpio_num_t>(dc_);
    io_config.cs_gpio_num = static_cast<gpio_num_t>(cs_);
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.spi_mode = 0;
    io_config.trans_queue_depth = 7;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spi_host, &io_config, &io_handle));
    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type     = GPIO_INTR_DISABLE;
    gpio_conf.mode          = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask  = (0x1ULL << rst_);
    gpio_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
    Set_ResetIOLevel(1);

    DisplayLen                = transfer >> 3; //(1byte 8ipex)
    DispBuffer                = (uint8_t *) heap_caps_malloc(DisplayLen, MALLOC_CAP_SPIRAM);
    assert(DispBuffer);
	PixelIndexLUT = (uint16_t (*)[300])heap_caps_malloc(transfer * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
	PixelBitLUT   = (uint8_t (*)[300])heap_caps_malloc(transfer * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    assert(PixelIndexLUT);
    assert(PixelBitLUT);
    if(width_ == 400) {
        InitLandscapeLUT();
    } else {
        InitPortraitLUT();
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = 2;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);
    lvgl_port_lock(0);

    display_ = lv_display_create(width, height); /* 以水平和垂直分辨率（像素）进行基本初始化 */
    lv_display_set_flush_cb(display_, Lvgl_flush_cb);
    lv_display_set_user_data(display_, this);
	size_t lvgl_buffer_size = LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565) * transfer;
	uint8_t *lvgl_buffer1 = (uint8_t *) heap_caps_malloc(lvgl_buffer_size, MALLOC_CAP_SPIRAM);
    assert(lvgl_buffer1);
	lv_display_set_buffers(display_, lvgl_buffer1, NULL, lvgl_buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI(TAG, "RLCD init");
    RLCD_Init();

    lvgl_port_unlock();
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    // Note: SetupUI() should be called by Application::Initialize(), not in constructor
    // to ensure lvgl objects are created after the display is fully initialized.
}

CustomLcdDisplay::~CustomLcdDisplay() {
}

void CustomLcdDisplay::InitPortraitLUT() {
    uint16_t W4 = width_ >> 2;
    for (uint16_t y = 0; y < height_; y++)
    {
        uint16_t byte_y = y >> 1;
        uint8_t  local_y = y & 1;
        for (uint16_t x = 0; x < width_; x++)
        {
            uint16_t byte_x = x >> 2;
            uint8_t  local_x = x & 3;

            uint32_t index = byte_y * W4 + byte_x;
            uint8_t bit = 7 - ((local_x << 1) | local_y);

            PixelIndexLUT[x][y] = index;
            PixelBitLUT  [x][y] = (1 << bit);
        }
    }
}

void CustomLcdDisplay::InitLandscapeLUT() {
    uint16_t H4 = height_ >> 2;
    for (uint16_t y = 0; y < height_; y++)
    {
        uint16_t inv_y = height_ - 1 - y;
        uint16_t block_y = inv_y >> 2;
        uint8_t  local_y  = inv_y & 3;
        for (uint16_t x = 0; x < width_; x++)
        {
            uint16_t byte_x = x >> 1;
            uint8_t  local_x = x & 1;

            uint32_t index = byte_x * H4 + block_y;
            uint8_t bit = 7 - ((local_y << 1) | local_x);

            PixelIndexLUT[x][y] = index;
            PixelBitLUT  [x][y] = (1 << bit);
        }
    }
}

void CustomLcdDisplay::Set_ResetIOLevel(uint8_t level) {
    gpio_set_level((gpio_num_t) rst_, level ? 1 : 0);
}

void CustomLcdDisplay::RLCD_SendCommand(uint8_t Reg) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, Reg, NULL, 0));
}

void CustomLcdDisplay::RLCD_SendData(uint8_t Data) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, -1, &Data, 1));
}

void CustomLcdDisplay::RLCD_Sendbuffera(uint8_t *Data, int len) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io_handle, -1, Data, len));
}

void CustomLcdDisplay::RLCD_Reset(void) {
    Set_ResetIOLevel(1);
    vTaskDelay(pdMS_TO_TICKS(50));
    Set_ResetIOLevel(0);
    vTaskDelay(pdMS_TO_TICKS(20));
    Set_ResetIOLevel(1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

void CustomLcdDisplay::RLCD_ColorClear(uint8_t color) {
    memset(DispBuffer, color, DisplayLen);
}

void CustomLcdDisplay::RLCD_Init() {
    RLCD_Reset();

    RLCD_SendCommand(0xD6);  // NVM Load Control
	RLCD_SendData(0x17);
	RLCD_SendData(0x02);

	RLCD_SendCommand(0xD1); //Booster Enable
	RLCD_SendData(0x01);

	RLCD_SendCommand(0xC0); //Gate Voltage Control
	RLCD_SendData(0x11);   
	RLCD_SendData(0x04);   

	RLCD_SendCommand(0xC1); //VSHP Setting
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);

	RLCD_SendCommand(0xC2);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);

	RLCD_SendCommand(0xC4);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);

	RLCD_SendCommand(0xC5);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);

	RLCD_SendCommand(0xD8);
	RLCD_SendData(0x80);
	RLCD_SendData(0xE9);

	RLCD_SendCommand(0xB2);
	RLCD_SendData(0x02);

	RLCD_SendCommand(0xB3);
	RLCD_SendData(0xE5);
	RLCD_SendData(0xF6);
	RLCD_SendData(0x05);
	RLCD_SendData(0x46);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x76);
	RLCD_SendData(0x45);

	RLCD_SendCommand(0xB4);
	RLCD_SendData(0x05);
	RLCD_SendData(0x46);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x76);
	RLCD_SendData(0x45);

	RLCD_SendCommand(0x62);
	RLCD_SendData(0x32);
	RLCD_SendData(0x03);
	RLCD_SendData(0x1F);

	RLCD_SendCommand(0xB7);
	RLCD_SendData(0x13);

	RLCD_SendCommand(0xB0);
	RLCD_SendData(0x64);

	RLCD_SendCommand(0x11); 
	vTaskDelay(pdMS_TO_TICKS(200));     
	RLCD_SendCommand(0xC9);
	RLCD_SendData(0x00);

	RLCD_SendCommand(0x36);
	RLCD_SendData(0x48); 

	RLCD_SendCommand(0x3A);
	RLCD_SendData(0x11); 

	RLCD_SendCommand(0xB9);
	RLCD_SendData(0x20);

	RLCD_SendCommand(0xB8);
	RLCD_SendData(0x29);

	RLCD_SendCommand(0x21);

	RLCD_SendCommand(0x2A); 
	RLCD_SendData(0x12);
	RLCD_SendData(0x2A);

	RLCD_SendCommand(0x2B); 
	RLCD_SendData(0x00);
	RLCD_SendData(0xC7);

	RLCD_SendCommand(0x35);
	RLCD_SendData(0x00);

	RLCD_SendCommand(0xD0);
	RLCD_SendData(0xFF);

	RLCD_SendCommand(0x38);
	RLCD_SendCommand(0x29);

    RLCD_ColorClear(ColorWhite);
}

void CustomLcdDisplay::RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color) {
    uint32_t idx = PixelIndexLUT[x][y];
    uint8_t  mask = PixelBitLUT[x][y];

    uint8_t *p = &DispBuffer[idx];

    if (color)
        *p |= mask;
    else
        *p &= ~mask;
}

void CustomLcdDisplay::RLCD_Display() {
    RLCD_SendCommand(0x2A);     // Column Address Set
  	RLCD_SendData(0x12);
  	RLCD_SendData(0x2A);

  	RLCD_SendCommand(0x2B);     // Page Address Set
  	RLCD_SendData(0x00);
  	RLCD_SendData(0xC7);

  	RLCD_SendCommand(0x2c);     // Page Address Set

  	RLCD_Sendbuffera(DispBuffer,DisplayLen);
}

// ===================== 待机仪表盘实现 =====================

void CustomLcdDisplay::SetupDashboard() {
    // 仪表盘根容器：覆盖整个屏幕，置于所有聊天 UI 之上
    // 用 screen 作为父对象，默认隐藏，待机时才显示
    auto screen = lv_screen_active();
    dashboard_root_ = lv_obj_create(screen);
    // 覆盖整个屏幕，无边框无内边距
    lv_obj_set_size(dashboard_root_, width_, height_);
    lv_obj_set_pos(dashboard_root_, 0, 0);
    lv_obj_set_style_bg_opa(dashboard_root_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dashboard_root_, 0, 0);
    lv_obj_set_style_pad_all(dashboard_root_, 0, 0);
    lv_obj_set_style_radius(dashboard_root_, 0, 0);
    lv_obj_set_scrollbar_mode(dashboard_root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(dashboard_root_, LV_OBJ_FLAG_SCROLLABLE);
    // 置顶（覆盖在聊天内容之上）
    lv_obj_move_foreground(dashboard_root_);

    // 左侧日历区（约 60% 宽度）—— 日期放大显示
    date_label_ = lv_label_create(dashboard_root_);
    lv_obj_set_style_text_font(date_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_transform_scale(date_label_, 256 * 2, 0);  // 放大 2 倍
    lv_obj_align(date_label_, LV_ALIGN_LEFT_MID, 25, -40);
    lv_label_set_text(date_label_, "2026/01/01");

    weekday_label_ = lv_label_create(dashboard_root_);
    lv_obj_set_style_text_font(weekday_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_transform_scale(weekday_label_, 256 * 2, 0);
    lv_obj_align(weekday_label_, LV_ALIGN_LEFT_MID, 25, 15);
    lv_label_set_text(weekday_label_, "星期一");

    // 右上：时钟（放大显示）
    clock_label_ = lv_label_create(dashboard_root_);
    lv_obj_set_style_text_font(clock_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_transform_scale(clock_label_, 256 * 3, 0);  // 放大 3 倍
    lv_obj_align(clock_label_, LV_ALIGN_TOP_RIGHT, -20, 55);
    lv_label_set_text(clock_label_, "00:00");

    // 右下：天气（温度+文字，第 2 步接真实数据）
    weather_label_ = lv_label_create(dashboard_root_);
    lv_obj_set_style_text_font(weather_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_transform_scale(weather_label_, 256 * 2, 0);
    lv_obj_align(weather_label_, LV_ALIGN_BOTTOM_RIGHT, -20, -50);
    lv_label_set_text(weather_label_, "--°");

    // 右下：城市名（小字，不放大）
    city_label_ = lv_label_create(dashboard_root_);
    lv_obj_set_style_text_font(city_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_align(city_label_, LV_ALIGN_BOTTOM_RIGHT, -20, -15);
    lv_label_set_text(city_label_, "定位中...");

    // 创建每秒刷新时钟的定时器
    esp_timer_create_args_t clock_timer_args = {
        .callback = ClockTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "dash_clock",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&clock_timer_args, &clock_timer_);

    // 默认先隐藏，等进入待机状态时显示
    HideDashboard();

    // 立即刷新一次时钟显示
    RefreshClock();
}

void CustomLcdDisplay::ClockTimerCallback(void* arg) {
    auto* self = static_cast<CustomLcdDisplay*>(arg);
    self->RefreshClock();
}

void CustomLcdDisplay::RefreshClock() {
    if (clock_label_ == nullptr) return;

    // 小智激活时已把本地时间写入系统（见 ota.cc 的 settimeofday）
    // 因此系统时间即为本地时间，直接用 gmtime_r 读取
    time_t now = time(nullptr);
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);

    char time_buf[16];
    char date_buf[16];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d",
             timeinfo.tm_hour, timeinfo.tm_min);
    snprintf(date_buf, sizeof(date_buf), "%04d/%02d/%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);

    DisplayLockGuard lock(this);
    if (clock_label_ != nullptr) {
        lv_label_set_text(clock_label_, time_buf);
    }
    if (date_label_ != nullptr) {
        lv_label_set_text(date_label_, date_buf);
    }
    if (weekday_label_ != nullptr && timeinfo.tm_wday >= 0 && timeinfo.tm_wday <= 6) {
        lv_label_set_text(weekday_label_, kWeekdays[timeinfo.tm_wday]);
    }
}

void CustomLcdDisplay::ShowDashboard() {
    if (dashboard_root_ == nullptr) return;
    DisplayLockGuard lock(this);
    lv_obj_remove_flag(dashboard_root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(dashboard_root_);
    // 启动时钟定时器（每秒刷新）
    if (clock_timer_ != nullptr) {
        esp_timer_start_periodic(clock_timer_, 1000000);
    }
    RefreshClock();
}

void CustomLcdDisplay::HideDashboard() {
    if (dashboard_root_ == nullptr) return;
    DisplayLockGuard lock(this);
    lv_obj_add_flag(dashboard_root_, LV_OBJ_FLAG_HIDDEN);
    // 停止时钟定时器（节省 CPU）
    if (clock_timer_ != nullptr) {
        esp_timer_stop(clock_timer_);
    }
}

void CustomLcdDisplay::SetupUI() {
    // 先调用基类 SetupUI 完成标准聊天界面的创建
    LcdDisplay::SetupUI();
    // 在标准 UI 之上叠加仪表盘
    SetupDashboard();
}

void CustomLcdDisplay::SetStatus(const char* status) {
    // 先调用基类处理状态文字显示
    LcdDisplay::SetStatus(status);
    // 感知待机切换：收到 STANDBY 显示仪表盘，其他状态隐藏
    if (strcmp(status, Lang::Strings::STANDBY) == 0) {
        ShowDashboard();
    } else {
        HideDashboard();
    }
}

