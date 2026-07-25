#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <utility>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <esp_err.h>
#include <cJSON.h>
#include "miniz.h"
#include "custom_lcd_display.h"
#include "lcd_display.h"
#include "esp_lvgl_port.h"
#include "assets/lang_config.h"
#include "settings.h"
#include "config.h"
#include "sdkconfig.h"
#include "board.h"
#include <wifi_manager.h>

// BUILTIN_TEXT_FONT 由 CMake 定义（RLCD 板为 font_noto_sans_basic_30_4），保证被编译进来
LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

namespace {

// 年度工作日位图镜像（数据依据国务院年度放假调休通知）；只读网络源，不落盘。
// 该站点当前的证书链不在 ESP-IDF 的 CRT bundle 中，但服务同时提供同一份
// 纯文本 HTTP 响应。内容只接受严格的年度 0/1 位图校验，因此使用 HTTP
// 可以避免设备每次启动都触发无效 TLS 握手，同时保留调休日历功能。
constexpr char kHolidayCalendarUrl[] = "http://chinese-workday-calendar.aops.io/years.properties";
// 该 MIT 数据集提供国务院公布的节假日和调休名称；年度位图接口仍作为
// 日期格子反显的主数据源，名称接口失败时仪表盘会自动回退到周末/工作日。
constexpr char kHolidayNameCalendarBaseUrl[] = "https://unpkg.com/holiday-calendar/data/CN/";
constexpr char kHolidayNameCalendarFallbackBaseUrl[] =
    "https://raw.githubusercontent.com/cg-zhou/holiday-calendar/main/data/CN/";
constexpr size_t kMaxHolidayCalendarBody = 16 * 1024;
constexpr uint32_t kHolidayCalendarRefreshMs = 6 * 60 * 60 * 1000;
constexpr uint32_t kHolidayCalendarRetryMs = 5 * 60 * 1000;
constexpr size_t kMaxWeatherBody = 16 * 1024;
constexpr size_t kMaxWeatherDecodedBody = 16 * 1024;
constexpr uint32_t kWeatherBatteryRefreshMs = 60 * 60 * 1000;
constexpr uint32_t kWeatherExternalPowerRefreshMs = 60 * 1000;
constexpr uint32_t kWeatherRetryMs = 5 * 60 * 1000;
constexpr uint32_t kWeatherLocationRetryMs = 30 * 1000;
constexpr uint32_t kNetworkWaitMs = 1000;
// 使用用户指定的 uapis.cn 作为公网 IP 定位主来源。它同时返回 ip、region 和
// latitude/longitude；ipinfo.io 仅作为主来源不可用时的回退。
constexpr char kIpLocationUrl[] = "https://uapis.cn/api/v1/network/myip";
constexpr char kIpLocationFallbackUrl[] = "https://ipinfo.io/json";

// TLS setup, HTTP parsing and cJSON are all called from the same worker. The
// default 6 KiB stack was exhausted immediately after the IP response was
// parsed on ESP32-S3, before the QWeather request could start.
constexpr uint32_t kHolidayTaskStackSize = 16 * 1024;

#ifndef CONFIG_RLCD_QWEATHER_API_HOST
#define CONFIG_RLCD_QWEATHER_API_HOST ""
#endif
#ifndef CONFIG_RLCD_QWEATHER_API_KEY
#define CONFIG_RLCD_QWEATHER_API_KEY ""
#endif
#ifndef CONFIG_RLCD_QWEATHER_LOCATION
#define CONFIG_RLCD_QWEATHER_LOCATION ""
#endif
#ifndef CONFIG_RLCD_QWEATHER_CITY
#define CONFIG_RLCD_QWEATHER_CITY ""
#endif

std::string TrimWhitespace(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool ParseHolidayCalendarFlags(const std::string& body, int year, std::string& flags) {
    const std::string year_key = std::to_string(year);
    size_t line_start = 0;
    while (line_start < body.size()) {
        const size_t line_end = body.find('\n', line_start);
        const size_t line_length = line_end == std::string::npos ? body.size() - line_start : line_end - line_start;
        const std::string line = TrimWhitespace(body.substr(line_start, line_length));
        const size_t separator = line.find('=');
        if (separator != std::string::npos && TrimWhitespace(line.substr(0, separator)) == year_key) {
            const std::string value = TrimWhitespace(line.substr(separator + 1));
            if (value.size() < 365 || value.size() > 366) return false;
            for (const char flag : value) {
                if (flag != '0' && flag != '1') return false;
            }
            flags = value;
            return true;
        }
        if (line_end == std::string::npos) break;
        line_start = line_end + 1;
    }
    return false;
}

bool DecodeGzip(const std::string& body, std::string& decoded) {
    // QWeather currently returns gzip even when the request asks for identity.
    // Keep the uncompressed path as well so this remains compatible with custom hosts.
    if (body.size() < 2 || static_cast<uint8_t>(body[0]) != 0x1f || static_cast<uint8_t>(body[1]) != 0x8b) {
        decoded = body;
        return true;
    }
    if (body.size() < 18 || static_cast<uint8_t>(body[2]) != 8) return false;  // gzip + deflate

    const uint8_t flags = static_cast<uint8_t>(body[3]);
    if ((flags & 0xe0) != 0) return false;  // reserved gzip flags

    size_t offset = 10;
    if ((flags & 0x04) != 0) {  // FEXTRA
        if (offset + 2 > body.size()) return false;
        const uint16_t extra_length = static_cast<uint16_t>(static_cast<uint8_t>(body[offset])) |
                                       (static_cast<uint16_t>(static_cast<uint8_t>(body[offset + 1])) << 8);
        offset += 2;
        if (extra_length > body.size() - offset) return false;
        offset += extra_length;
    }
    for (uint8_t flag : {static_cast<uint8_t>(0x08), static_cast<uint8_t>(0x10)}) {
        if ((flags & flag) == 0) continue;
        while (offset < body.size() && body[offset] != '\0') ++offset;
        if (offset >= body.size()) return false;
        ++offset;
    }
    if ((flags & 0x02) != 0) {
        if (offset + 2 > body.size()) return false;
        offset += 2;  // header CRC16
    }
    if (offset >= body.size() || body.size() - offset < 8) return false;

    std::vector<uint8_t> output(kMaxWeatherDecodedBody);
    const size_t compressed_length = body.size() - offset - 8;  // omit gzip trailer
    const size_t output_length = tinfl_decompress_mem_to_mem(
        output.data(), output.size(), body.data() + offset, compressed_length,
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (output_length == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) return false;
    decoded.assign(reinterpret_cast<const char*>(output.data()), output_length);
    return true;
}

bool FetchHttpResponse(const char* url, const char* user_agent, const char* api_key, const char* service_name,
                       std::string& decoded) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) return false;
    auto http = network->CreateHttp(0);
    if (http == nullptr) return false;

    http->SetTimeout(15000);
    http->SetKeepAlive(false);
    http->SetHeader("User-Agent", user_agent);
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Accept-Encoding", "gzip");
    if (api_key != nullptr && api_key[0] != '\0') {
        http->SetHeader("X-QW-Api-Key", api_key);
    }
    if (!http->Open("GET", std::string(url))) {
        ESP_LOGW("CustomDisplay", "%s request failed to open", service_name);
        return false;
    }

    const int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGW("CustomDisplay", "%s request returned status %d", service_name, status_code);
        http->Close();
        return false;
    }

    std::string body;
    body.reserve(2048);
    char buffer[512];
    bool read_ok = true;
    while (body.size() < kMaxWeatherBody) {
        const int bytes_read = http->Read(buffer, sizeof(buffer));
        if (bytes_read < 0) {
            read_ok = false;
            break;
        }
        if (bytes_read == 0) break;
        body.append(buffer, static_cast<size_t>(bytes_read));
    }
    if (body.size() >= kMaxWeatherBody) read_ok = false;
    http->Close();

    if (!read_ok || !DecodeGzip(body, decoded)) {
        ESP_LOGW("CustomDisplay", "%s response could not be decoded", service_name);
        return false;
    }
    return true;
}

bool FetchQWeatherResponse(const std::string& path, std::string& decoded) {
    std::string url = "https://";
    url += CONFIG_RLCD_QWEATHER_API_HOST;
    url += path;
    return FetchHttpResponse(url.c_str(), "xiaozhi-rlcd-qweather/1.0", CONFIG_RLCD_QWEATHER_API_KEY,
                             "QWeather", decoded);
}

bool FetchIpLocationResponse(std::string& decoded) {
    const char* const urls[] = {kIpLocationUrl, kIpLocationFallbackUrl};
    const char* const names[] = {"IP location (uapis.cn)", "IP location fallback (ipinfo)"};
    for (size_t index = 0; index < sizeof(urls) / sizeof(urls[0]); ++index) {
        if (FetchHttpResponse(urls[index], "xiaozhi-rlcd-ip-location/1.0", nullptr, names[index], decoded)) {
            return true;
        }
    }
    // Public IP geolocation providers can be rate limited independently. The
    // final plain-HTTP provider also covers networks where TLS/SNI is blocked.
    return false;
}

bool JsonNumberText(cJSON* item, std::string& text) {
    if (cJSON_IsNumber(item)) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.2f", item->valuedouble);
        text = buffer;
        return true;
    }
    if (cJSON_IsString(item) && item->valuestring != nullptr && item->valuestring[0] != '\0') {
        text = item->valuestring;
        return true;
    }
    return false;
}

std::string JsonStringOr(cJSON* item, const char* fallback) {
    if (cJSON_IsString(item) && item->valuestring != nullptr && item->valuestring[0] != '\0') {
        return item->valuestring;
    }
    return fallback != nullptr ? fallback : "";
}

std::string LastRegionComponent(const std::string& region) {
    std::string value = TrimWhitespace(region);
    if (value.empty()) return {};

    const size_t separator = value.find_last_of(" \t\r\n");
    if (separator != std::string::npos) value = TrimWhitespace(value.substr(separator + 1));
    return value;
}

bool ParseHolidayCalendarNote(const std::string& body, int year, int month, int day, std::string& note) {
    note.clear();
    cJSON* root = cJSON_Parse(body.c_str());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    cJSON* dates = cJSON_GetObjectItem(root, "dates");
    if (!cJSON_IsArray(dates)) {
        cJSON_Delete(root);
        return false;
    }

    std::string date_buf = std::to_string(year) + "-";
    if (month < 10) date_buf += '0';
    date_buf += std::to_string(month) + "-";
    if (day < 10) date_buf += '0';
    date_buf += std::to_string(day);
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, dates) {
        if (!cJSON_IsObject(item)) continue;
        const std::string date = JsonStringOr(cJSON_GetObjectItem(item, "date"), "");
        if (date != date_buf) continue;

        note = JsonStringOr(cJSON_GetObjectItem(item, "name_cn"), "");
        if (note.empty()) note = JsonStringOr(cJSON_GetObjectItem(item, "name"), "");
        break;
    }
    cJSON_Delete(root);
    return true;
}

double JsonDoubleOr(cJSON* item, double fallback) {
    if (cJSON_IsNumber(item)) return item->valuedouble;
    if (cJSON_IsString(item) && item->valuestring != nullptr && item->valuestring[0] != '\0') {
        char* end = nullptr;
        const double value = std::strtod(item->valuestring, &end);
        if (end != item->valuestring && end != nullptr && *end == '\0') return value;
    }
    return fallback;
}

std::string AlertColorText(const std::string& color) {
    if (color == "blue") return "蓝色";
    if (color == "yellow") return "黄色";
    if (color == "orange") return "橙色";
    if (color == "red") return "红色";
    if (color == "purple") return "紫色";
    if (color == "green") return "绿色";
    if (color == "black") return "黑色";
    if (color == "white") return "白色";
    if (color == "gray") return "灰色";
    return color;
}

std::string CombineWeatherNotices(const std::string& alert, const std::string& precipitation) {
    if (alert.empty() && precipitation.empty()) return {};
    // Always reserve two visual rows: alert on the first row and minutely
    // precipitation on the second. The dashboard layout turns this delimiter
    // into two independent single-line scrolling labels.
    return alert + "\n" + precipitation;
}

std::string ParseMinutelyNotice(cJSON* root) {
    if (!cJSON_IsObject(root)) return {};
    cJSON* code = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsString(code) || code->valuestring == nullptr || std::strcmp(code->valuestring, "200") != 0) {
        return {};
    }
    cJSON* minutely = cJSON_GetObjectItem(root, "minutely");
    if (!cJSON_IsArray(minutely)) return {};

    bool has_precipitation = false;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, minutely) {
        if (JsonDoubleOr(cJSON_GetObjectItem(item, "precip"), 0.0) > 0.0001) {
            has_precipitation = true;
            break;
        }
    }
    if (!has_precipitation) return {};

    const std::string summary = TrimWhitespace(JsonStringOr(cJSON_GetObjectItem(root, "summary"), ""));
    return summary.empty() ? "未来两小时有降水" : summary;
}

std::string ParseAlertNotice(cJSON* root) {
    if (!cJSON_IsObject(root)) return {};
    cJSON* alerts = cJSON_GetObjectItem(root, "alerts");
    const bool legacy_warning_api = !cJSON_IsArray(alerts);
    if (legacy_warning_api) alerts = cJSON_GetObjectItem(root, "warning");
    if (!cJSON_IsArray(alerts)) return {};

    cJSON* alert = nullptr;
    cJSON_ArrayForEach(alert, alerts) {
        if (legacy_warning_api) {
            const std::string status = JsonStringOr(cJSON_GetObjectItem(alert, "status"), "active");
            if (status == "cancel" || status == "expired" || status == "past") continue;
            const std::string type_name = JsonStringOr(cJSON_GetObjectItem(alert, "typeName"), "");
            const std::string level = JsonStringOr(cJSON_GetObjectItem(alert, "level"), "");
            if (!type_name.empty()) {
                return "预警 " + type_name + level + "预警";
            }
            const std::string title = TrimWhitespace(JsonStringOr(cJSON_GetObjectItem(alert, "title"), ""));
            if (!title.empty()) return "预警 " + title;
            continue;
        }

        cJSON* message_type = cJSON_GetObjectItem(alert, "messageType");
        const std::string message_type_code = JsonStringOr(
            message_type != nullptr ? cJSON_GetObjectItem(message_type, "code") : nullptr, "");
        if (message_type_code == "cancel") continue;

        cJSON* event_type = cJSON_GetObjectItem(alert, "eventType");
        const std::string event_name = JsonStringOr(
            event_type != nullptr ? cJSON_GetObjectItem(event_type, "name") : nullptr, "");
        const std::string color = AlertColorText(JsonStringOr(
            cJSON_GetObjectItem(alert, "color") != nullptr
                ? cJSON_GetObjectItem(cJSON_GetObjectItem(alert, "color"), "code")
                : nullptr,
            ""));
        if (!event_name.empty()) {
            std::string notice = "预警 " + event_name;
            if (!color.empty()) notice += color + "预警";
            return notice;
        }

        const std::string headline = TrimWhitespace(JsonStringOr(cJSON_GetObjectItem(alert, "headline"), ""));
        if (!headline.empty()) return "预警 " + headline;
    }
    return {};
}

bool IsCoordinate(const std::string& text, double minimum, double maximum) {
    if (text.empty()) return false;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    return end != text.c_str() && end != nullptr && *end == '\0' && value >= minimum && value <= maximum;
}

bool ParseLongitudeLatitude(const std::string& text, std::string& longitude, std::string& latitude) {
    const size_t separator = text.find(',');
    if (separator == std::string::npos) return false;
    const std::string parsed_longitude = TrimWhitespace(text.substr(0, separator));
    const std::string parsed_latitude = TrimWhitespace(text.substr(separator + 1));
    if (!IsCoordinate(parsed_longitude, -180.0, 180.0) || !IsCoordinate(parsed_latitude, -90.0, 90.0)) {
        return false;
    }
    longitude = parsed_longitude;
    latitude = parsed_latitude;
    return true;
}

bool ParseIpInfoLocation(cJSON* item, std::string& latitude, std::string& longitude) {
    if (!cJSON_IsString(item) || item->valuestring == nullptr) return false;
    const std::string value = TrimWhitespace(item->valuestring);
    const size_t separator = value.find(',');
    if (separator == std::string::npos) return false;

    const std::string parsed_latitude = TrimWhitespace(value.substr(0, separator));
    const std::string parsed_longitude = TrimWhitespace(value.substr(separator + 1));
    if (!IsCoordinate(parsed_latitude, -90.0, 90.0) || !IsCoordinate(parsed_longitude, -180.0, 180.0)) {
        return false;
    }
    latitude = parsed_latitude;
    longitude = parsed_longitude;
    return true;
}

bool IsClockStatus(const char* status) {
    if (status == nullptr || std::strlen(status) != 5 || status[2] != ':') return false;
    for (size_t index : {size_t{0}, size_t{1}, size_t{3}, size_t{4}}) {
        if (status[index] < '0' || status[index] > '9') return false;
    }
    return true;
}

}  // namespace

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
    StopHolidayCalendarTask();
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
    // 调用共享布局代码创建 UI（纯 LVGL，无 ESP 依赖）
    auto screen = lv_screen_active();
    dashboard_layout::Setup(dashboard_state_, screen, width_, height_, &BUILTIN_TEXT_FONT);

    weather_location_ = CONFIG_RLCD_QWEATHER_LOCATION;
    weather_city_ = CONFIG_RLCD_QWEATHER_CITY;
    weather_location_manual_ = !weather_location_.empty();
    weather_location_ready_ = false;
    if (weather_location_manual_) {
        std::string longitude;
        std::string latitude;
        if (ParseLongitudeLatitude(weather_location_, longitude, latitude)) {
            weather_coordinates_ = longitude + "," + latitude;
            weather_location_ready_ = true;
        }
    }

    // 创建 ESP 专属的时钟定时器（1 秒周期）
    esp_timer_create_args_t clock_timer_args = {
        .callback = ClockTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "dash_clock",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&clock_timer_args, &clock_timer_);

    // 调休日历只保存在 RAM，由后台任务定期重新查询，避免把年度安排写入 NVS。
    holiday_task_stop_ = false;
    holiday_task_running_ = false;
    if (xTaskCreate(HolidayCalendarTask, "dash_holiday", kHolidayTaskStackSize, this, 2, &holiday_task_) != pdPASS) {
        ESP_LOGW(TAG, "Failed to start holiday calendar task");
        holiday_task_ = nullptr;
    } else {
        holiday_task_running_ = true;
    }
    if (CONFIG_RLCD_QWEATHER_API_KEY[0] == '\0') {
        ESP_LOGW(TAG, "QWeather is not configured; set RLCD_QWEATHER_API_KEY in menuconfig");
    }
}

void CustomLcdDisplay::ClockTimerCallback(void* arg) {
    auto* self = static_cast<CustomLcdDisplay*>(arg);
    DisplayLockGuard lock(self);
    self->ApplyPendingDashboardData();
    dashboard_layout::Refresh(self->dashboard_state_);
}

void CustomLcdDisplay::HolidayCalendarTask(void* arg) {
    auto* self = static_cast<CustomLcdDisplay*>(arg);
    TickType_t next_holiday = xTaskGetTickCount();
    TickType_t next_weather = next_holiday;
    // Resolve weather as soon as the first network connection is ready.  The
    // holiday feed is independent and must not delay the visible weather card.
    TickType_t next_location = next_holiday;
    while (!self->holiday_task_stop_.load()) {
        // SetupUI() runs before Application::Initialize() starts the network.
        // Do not enter esp-tls/getaddrinfo until esp-netif/lwIP is initialized
        // and the station has obtained an IP address; otherwise tcpip's mbox
        // is not valid yet and lwIP asserts from tcpip_send_msg_wait_sem().
        auto& wifi = WifiManager::GetInstance();
        if (!wifi.IsInitialized() || !wifi.IsConnected()) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kNetworkWaitMs));
            continue;
        }

        TickType_t now = xTaskGetTickCount();
        const bool weather_configured = CONFIG_RLCD_QWEATHER_API_HOST[0] != '\0' &&
                                        CONFIG_RLCD_QWEATHER_API_KEY[0] != '\0';
        if (weather_configured) {
            // Resolve the public-IP location once at startup. Further changes
            // are explicitly requested by the AI MCP tool; do not poll the
            // geolocation providers on a timer.
            const bool location_requested = self->weather_location_refresh_requested_.load();
            const bool location_due = !self->weather_location_ready_ ||
                                      (!self->weather_location_manual_ && location_requested);
            if (location_due && (location_requested || static_cast<int32_t>(now - next_location) >= 0)) {
                const bool resolved = self->ResolveWeatherLocation();
                next_location = xTaskGetTickCount() + pdMS_TO_TICKS(kWeatherLocationRetryMs);
                if (resolved) {
                    self->weather_location_refresh_requested_.store(false);
                    next_weather = xTaskGetTickCount();
                }
            }

            now = xTaskGetTickCount();
            if (self->weather_location_ready_ && static_cast<int32_t>(now - next_weather) >= 0) {
                const bool fetched = self->FetchWeather();
                int battery_level = 0;
                bool charging = false;
                bool discharging = false;
                const bool power_state_known =
                    Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging);
                const bool external_power = power_state_known && (charging || !discharging);
                const uint32_t refresh_ms = external_power ? kWeatherExternalPowerRefreshMs
                                                            : kWeatherBatteryRefreshMs;
                next_weather = xTaskGetTickCount() +
                               pdMS_TO_TICKS(fetched ? refresh_ms : kWeatherRetryMs);
            }
        } else if (!weather_configured) {
            // Do not busy-loop when the optional API key is intentionally absent.
            next_location = now + pdMS_TO_TICKS(kWeatherBatteryRefreshMs);
            next_weather = now + pdMS_TO_TICKS(kWeatherBatteryRefreshMs);
        }

        now = xTaskGetTickCount();
        if (static_cast<int32_t>(now - next_holiday) >= 0) {
            const bool fetched = self->FetchHolidayCalendar();
            next_holiday = xTaskGetTickCount() + pdMS_TO_TICKS(fetched ? kHolidayCalendarRefreshMs
                                                                      : kHolidayCalendarRetryMs);
        }

        if (self->holiday_task_stop_.load()) break;
        now = xTaskGetTickCount();
        const TickType_t holiday_wait = next_holiday - now;
        const TickType_t weather_wait = next_weather - now;
        TickType_t wait_ticks = holiday_wait;
        if (weather_wait < wait_ticks) wait_ticks = weather_wait;
        if (weather_configured &&
            (!self->weather_location_ready_ ||
             (!self->weather_location_manual_ && self->weather_location_refresh_requested_.load()))) {
            const TickType_t location_wait = next_location - now;
            if (location_wait < wait_ticks) wait_ticks = location_wait;
        }
        ulTaskNotifyTake(pdTRUE, wait_ticks);
    }
    self->holiday_task_running_ = false;
    vTaskDelete(nullptr);
}

bool CustomLcdDisplay::FetchHolidayCalendar() {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    const int year = timeinfo.tm_year + 1900;
    if (year < 2000) {
        ESP_LOGD(TAG, "Holiday calendar query postponed until system time is synchronized");
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) return false;
    auto http = network->CreateHttp(0);
    if (http == nullptr) return false;

    http->SetTimeout(15000);
    http->SetKeepAlive(false);
    http->SetHeader("User-Agent", "xiaozhi-rlcd-holiday-calendar/1.0");
    http->SetHeader("Accept", "text/plain");
    http->SetHeader("Accept-Encoding", "identity");
    if (!http->Open("GET", kHolidayCalendarUrl)) {
        ESP_LOGW(TAG, "Holiday calendar request failed to open");
        return false;
    }

    const int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGW(TAG, "Holiday calendar request returned status %d", status_code);
        http->Close();
        return false;
    }

    std::string body;
    body.reserve(10 * 1024);
    char buffer[512];
    bool read_ok = true;
    while (body.size() < kMaxHolidayCalendarBody) {
        const int bytes_read = http->Read(buffer, sizeof(buffer));
        if (bytes_read < 0) {
            read_ok = false;
            break;
        }
        if (bytes_read == 0) break;
        body.append(buffer, static_cast<size_t>(bytes_read));
    }
    if (body.size() >= kMaxHolidayCalendarBody) read_ok = false;
    http->Close();

    std::string flags;
    if (!read_ok || !ParseHolidayCalendarFlags(body, year, flags)) {
        ESP_LOGW(TAG, "Holiday calendar response has no valid %d entry", year);
        return false;
    }

    const unsigned flag_count = static_cast<unsigned>(flags.size());

    // 名称数据是可选增强：年度位图成功后即使 CDN 暂时不可用，日历黑底和
    // “周末/工作日”兜底仍然正常显示。
    std::string holiday_note;
    bool holiday_note_fetched = false;
    const int month = timeinfo.tm_mon + 1;
    const int day = timeinfo.tm_mday;
    const int day_of_year = timeinfo.tm_yday + 1;
    const std::string year_suffix = std::to_string(year) + ".json";
    std::string name_body;
    std::string name_url = kHolidayNameCalendarBaseUrl + year_suffix;
    if (FetchHttpResponse(name_url.c_str(), "xiaozhi-rlcd-holiday-names/1.0", nullptr, "Holiday names", name_body) ||
        (name_url = kHolidayNameCalendarFallbackBaseUrl + year_suffix,
         FetchHttpResponse(name_url.c_str(), "xiaozhi-rlcd-holiday-names/1.0", nullptr,
                           "Holiday names fallback", name_body))) {
        holiday_note_fetched = ParseHolidayCalendarNote(name_body, year, month, day, holiday_note);
    }

    {
        std::lock_guard<std::mutex> lock(holiday_mutex_);
        pending_holiday_year_ = year;
        pending_holiday_flags_ = std::move(flags);
        pending_holiday_ready_ = true;
        if (holiday_note_fetched) {
            pending_holiday_note_year_ = year;
            pending_holiday_note_day_of_year_ = day_of_year;
            pending_holiday_note_ = std::move(holiday_note);
            pending_holiday_note_ready_ = true;
        }
    }
    ESP_LOGI(TAG, "Holiday calendar fetched for %d (%u days)%s", year, flag_count,
             holiday_note_fetched ? ", today name resolved" : ", today name unavailable");
    return true;
}

bool CustomLcdDisplay::ResolveWeatherLocation() {
    // 保留一个显式配置入口，便于没有公网 IP 定位服务时手动指定位置；默认配置为空，
    // 正常路径始终根据当前网络重新定位，不把任何城市写死在固件中。
    if (weather_location_manual_) {
        if (weather_location_.empty()) return false;
        // Minutely precipitation and alerts need coordinates even when the
        // user configured a QWeather city Location ID. Resolve them once via
        // GeoAPI, while keeping the configured ID for the regular weather API.
        if (weather_coordinates_.empty()) {
            std::string geo_json;
            std::string geo_path = "/geo/v2/city/lookup?location=";
            geo_path += weather_location_;
            geo_path += "&lang=zh&number=1";
            if (FetchQWeatherResponse(geo_path, geo_json)) {
                cJSON* geo_root = cJSON_Parse(geo_json.c_str());
                cJSON* locations = geo_root != nullptr ? cJSON_GetObjectItem(geo_root, "location") : nullptr;
                cJSON* location = cJSON_IsArray(locations) ? cJSON_GetArrayItem(locations, 0) : nullptr;
                std::string latitude;
                std::string longitude;
                if (cJSON_IsObject(location) &&
                    JsonNumberText(cJSON_GetObjectItem(location, "lat"), latitude) &&
                    JsonNumberText(cJSON_GetObjectItem(location, "lon"), longitude) &&
                    IsCoordinate(latitude, -90.0, 90.0) && IsCoordinate(longitude, -180.0, 180.0)) {
                    weather_coordinates_ = longitude + "," + latitude;
                    if (weather_city_.empty()) {
                        weather_city_ = JsonStringOr(cJSON_GetObjectItem(location, "adm2"), "");
                        if (weather_city_.empty()) {
                            weather_city_ = JsonStringOr(cJSON_GetObjectItem(location, "name"), "");
                        }
                    }
                }
                cJSON_Delete(geo_root);
            }
        }
        weather_location_ready_ = true;
        if (weather_city_.empty()) weather_city_ = "定位中";
        if (weather_city_ != "定位中") {
            std::lock_guard<std::mutex> lock(holiday_mutex_);
            pending_weather_city_only_ = weather_city_;
            pending_weather_city_only_ready_ = true;
        }
        return weather_location_ready_;
    }

    std::string ip_json;
    if (!FetchIpLocationResponse(ip_json)) {
        ESP_LOGW(TAG, "IP location lookup failed");
        return false;
    }

    cJSON* ip_root = cJSON_Parse(ip_json.c_str());
    if (ip_root == nullptr) {
        ESP_LOGW(TAG, "IP location response is not valid JSON");
        return false;
    }

    std::string latitude;
    std::string longitude;
    bool has_latitude = JsonNumberText(cJSON_GetObjectItem(ip_root, "latitude"), latitude);
    bool has_longitude = JsonNumberText(cJSON_GetObjectItem(ip_root, "longitude"), longitude);
    if (!has_latitude || !has_longitude) {
        latitude.clear();
        longitude.clear();
        has_latitude = JsonNumberText(cJSON_GetObjectItem(ip_root, "lat"), latitude);
        has_longitude = JsonNumberText(cJSON_GetObjectItem(ip_root, "lon"), longitude);
    }
    // ipinfo.io exposes the same coordinates as a single "loc": "lat,lon"
    // field. Accept it so the fallback response does not leave the dashboard
    // in its initial state.
    if (!has_latitude || !has_longitude) {
        latitude.clear();
        longitude.clear();
        const bool parsed_loc = ParseIpInfoLocation(cJSON_GetObjectItem(ip_root, "loc"), latitude, longitude);
        has_latitude = parsed_loc;
        has_longitude = parsed_loc;
    }
    cJSON* ip_city_item = cJSON_GetObjectItem(ip_root, "city");
    std::string ip_city = cJSON_IsString(ip_city_item) && ip_city_item->valuestring != nullptr
                              ? TrimWhitespace(ip_city_item->valuestring)
                              : std::string();
    // uapis.cn/ipinfo.io both use "ip". Keep "query" for compatibility with
    // older ip-api responses that may still be returned by a local proxy.
    cJSON* ip_address_item = cJSON_GetObjectItem(ip_root, "ip");
    if (!cJSON_IsString(ip_address_item) || ip_address_item->valuestring == nullptr) {
        ip_address_item = cJSON_GetObjectItem(ip_root, "query");
    }
    std::string ip_address = cJSON_IsString(ip_address_item) && ip_address_item->valuestring != nullptr
                                 ? TrimWhitespace(ip_address_item->valuestring)
                                 : std::string();
    if (ip_city.empty()) {
        cJSON* region_item = cJSON_GetObjectItem(ip_root, "region");
        if (cJSON_IsString(region_item) && region_item->valuestring != nullptr) {
            ip_city = LastRegionComponent(region_item->valuestring);
        }
    }
    cJSON_Delete(ip_root);

    if (!has_latitude || !has_longitude || !IsCoordinate(latitude, -90.0, 90.0) ||
        !IsCoordinate(longitude, -180.0, 180.0)) {
        ESP_LOGW(TAG, "IP location response has no coordinates");
        return false;
    }

    ESP_LOGI(TAG, "IP geolocation estimate: ip=%s city=%s (%s,%s); public-IP location may be approximate",
             ip_address.empty() ? "unknown" : ip_address.c_str(), ip_city.empty() ? "unknown" : ip_city.c_str(),
             latitude.c_str(), longitude.c_str());

    // 和风天气的 location 坐标格式是“经度,纬度”，并建议最多保留两位小数。
    const std::string coordinate = longitude + "," + latitude;
    std::string geo_json;
    std::string geo_path = "/geo/v2/city/lookup?location=";
    geo_path += coordinate;
    geo_path += "&lang=zh&number=1";

    std::string resolved_location = coordinate;
    // Keep the city supplied by the IP provider when it is available. The
    // QWeather GeoAPI response is used to obtain its stable Location ID, but
    // it may map an approximate public-IP coordinate to a neighboring city
    // and should not silently replace the provider's city label.
    std::string resolved_city = ip_city;
    std::string qweather_city;
    if (FetchQWeatherResponse(geo_path, geo_json)) {
        cJSON* geo_root = cJSON_Parse(geo_json.c_str());
        if (geo_root != nullptr) {
            cJSON* geo_code = cJSON_GetObjectItem(geo_root, "code");
            cJSON* locations = cJSON_GetObjectItem(geo_root, "location");
            cJSON* location = cJSON_IsArray(locations) ? cJSON_GetArrayItem(locations, 0) : nullptr;
            cJSON* location_id = location != nullptr ? cJSON_GetObjectItem(location, "id") : nullptr;
            cJSON* location_adm2 = location != nullptr ? cJSON_GetObjectItem(location, "adm2") : nullptr;
            cJSON* location_name = location != nullptr ? cJSON_GetObjectItem(location, "name") : nullptr;
            if (cJSON_IsString(geo_code) && geo_code->valuestring != nullptr &&
                std::strcmp(geo_code->valuestring, "200") == 0 && cJSON_IsObject(location) &&
                cJSON_IsString(location_id) && location_id->valuestring != nullptr &&
                location_id->valuestring[0] != '\0') {
                resolved_location = location_id->valuestring;
                if (cJSON_IsString(location_adm2) && location_adm2->valuestring != nullptr &&
                    location_adm2->valuestring[0] != '\0') {
                    qweather_city = location_adm2->valuestring;
                } else if (cJSON_IsString(location_name) && location_name->valuestring != nullptr &&
                           location_name->valuestring[0] != '\0') {
                    qweather_city = location_name->valuestring;
                }
                if (resolved_city.empty()) {
                    resolved_city = qweather_city;
                } else if (!qweather_city.empty() && qweather_city != resolved_city) {
                    ESP_LOGW(TAG, "IP/QWeather city mismatch: ip=%s qweather=%s; keeping IP city",
                             resolved_city.c_str(), qweather_city.c_str());
                }
            } else {
                ESP_LOGW(TAG, "QWeather GeoAPI returned no matching city; using coordinates");
            }
            cJSON_Delete(geo_root);
        }
    }

    weather_coordinates_ = coordinate;
    weather_location_ = std::move(resolved_location);
    weather_city_ = CONFIG_RLCD_QWEATHER_CITY[0] != '\0' ? CONFIG_RLCD_QWEATHER_CITY : resolved_city;
    if (weather_city_.empty()) weather_city_ = "定位中";
    weather_precipitation_notice_.clear();
    weather_alert_notice_.clear();
    weather_location_ready_ = true;
    if (weather_city_ != "定位中") {
        std::lock_guard<std::mutex> lock(holiday_mutex_);
        pending_weather_city_only_ = weather_city_;
        pending_weather_city_only_ready_ = true;
    }
    ESP_LOGI(TAG, "Weather location resolved: %s (%s)", weather_city_.c_str(), weather_location_.c_str());
    return true;
}

bool CustomLcdDisplay::FetchWeatherNotices() {
    std::string longitude;
    std::string latitude;
    if (!ParseLongitudeLatitude(weather_coordinates_, longitude, latitude)) return false;

    bool notice_updated = false;

    std::string minutely_json;
    std::string minutely_path = "/v7/minutely/5m?location=";
    minutely_path += weather_coordinates_;
    minutely_path += "&lang=zh";
    if (FetchQWeatherResponse(minutely_path, minutely_json)) {
        cJSON* minutely_root = cJSON_Parse(minutely_json.c_str());
        cJSON* minutely_code = minutely_root != nullptr ? cJSON_GetObjectItem(minutely_root, "code") : nullptr;
        cJSON* minutely_data = minutely_root != nullptr ? cJSON_GetObjectItem(minutely_root, "minutely") : nullptr;
        if (cJSON_IsString(minutely_code) && minutely_code->valuestring != nullptr &&
            std::strcmp(minutely_code->valuestring, "200") == 0 && cJSON_IsArray(minutely_data)) {
            weather_precipitation_notice_ = ParseMinutelyNotice(minutely_root);
            notice_updated = true;
        }
        cJSON_Delete(minutely_root);
    }

    std::string alert_json;
    // The configured free API key is currently served by the v7 host. Use its
    // warning endpoint first; fall back to the newer v1 path for custom API
    // hosts that have already migrated.
    std::string alert_path = "/v7/warning/now?location=";
    alert_path += weather_location_;
    alert_path += "&lang=zh";
    bool alert_request_ok = FetchQWeatherResponse(alert_path, alert_json);
    if (!alert_request_ok) {
        alert_json.clear();
        alert_path = "/weatheralert/v1/current/";
        alert_path += latitude;
        alert_path += ",";
        alert_path += longitude;
        alert_path += "?localTime=true&lang=zh";
        alert_request_ok = FetchQWeatherResponse(alert_path, alert_json);
    }
    if (alert_request_ok) {
        cJSON* alert_root = cJSON_Parse(alert_json.c_str());
        cJSON* alert_code = alert_root != nullptr ? cJSON_GetObjectItem(alert_root, "code") : nullptr;
        cJSON* alerts = alert_root != nullptr ? cJSON_GetObjectItem(alert_root, "alerts") : nullptr;
        cJSON* legacy_warning = alert_root != nullptr ? cJSON_GetObjectItem(alert_root, "warning") : nullptr;
        cJSON* metadata = alert_root != nullptr ? cJSON_GetObjectItem(alert_root, "metadata") : nullptr;
        cJSON* zero_result = metadata != nullptr ? cJSON_GetObjectItem(metadata, "zeroResult") : nullptr;
        const bool code_ok = cJSON_IsString(alert_code) && alert_code->valuestring != nullptr &&
                             std::strcmp(alert_code->valuestring, "200") == 0;
        const bool valid_alert_response = (code_ok && (cJSON_IsArray(alerts) || cJSON_IsArray(legacy_warning))) ||
                                          cJSON_IsBool(zero_result);
        if (valid_alert_response) {
            weather_alert_notice_ = ParseAlertNotice(alert_root);
            notice_updated = true;
        }
        cJSON_Delete(alert_root);
    }

    if (!notice_updated) return false;
    weather_notice_ = CombineWeatherNotices(weather_alert_notice_, weather_precipitation_notice_);
    return true;
}

bool CustomLcdDisplay::FetchWeather() {
    if (!weather_location_ready_ || weather_location_.empty()) return false;

    std::string now_path = "/v7/weather/now?location=";
    now_path += weather_location_;
    now_path += "&lang=zh&unit=m";
    std::string forecast_path = "/v7/weather/7d?location=";
    forecast_path += weather_location_;
    forecast_path += "&lang=zh&unit=m";

    std::string now_json;
    std::string forecast_json;
    if (!FetchQWeatherResponse(now_path, now_json) || !FetchQWeatherResponse(forecast_path, forecast_json)) {
        return false;
    }

    cJSON* now_root = cJSON_Parse(now_json.c_str());
    cJSON* forecast_root = cJSON_Parse(forecast_json.c_str());
    if (now_root == nullptr || forecast_root == nullptr) {
        ESP_LOGW(TAG, "QWeather response is not valid JSON");
        cJSON_Delete(now_root);
        cJSON_Delete(forecast_root);
        return false;
    }

    cJSON* now_code = cJSON_GetObjectItem(now_root, "code");
    cJSON* now = cJSON_GetObjectItem(now_root, "now");
    cJSON* temperature = now != nullptr ? cJSON_GetObjectItem(now, "temp") : nullptr;
    cJSON* feels_like = now != nullptr ? cJSON_GetObjectItem(now, "feelsLike") : nullptr;
    cJSON* description = now != nullptr ? cJSON_GetObjectItem(now, "text") : nullptr;

    cJSON* forecast_code = cJSON_GetObjectItem(forecast_root, "code");
    cJSON* daily = cJSON_GetObjectItem(forecast_root, "daily");
    cJSON* today = cJSON_IsArray(daily) ? cJSON_GetArrayItem(daily, 0) : nullptr;
    cJSON* high_temperature = today != nullptr ? cJSON_GetObjectItem(today, "tempMax") : nullptr;
    cJSON* low_temperature = today != nullptr ? cJSON_GetObjectItem(today, "tempMin") : nullptr;

    const bool valid_now = cJSON_IsString(now_code) && now_code->valuestring != nullptr &&
                           std::strcmp(now_code->valuestring, "200") == 0 && cJSON_IsObject(now) &&
                           cJSON_IsString(temperature) && temperature->valuestring != nullptr &&
                           cJSON_IsString(feels_like) && feels_like->valuestring != nullptr &&
                           cJSON_IsString(description) && description->valuestring != nullptr;
    const bool valid_forecast = cJSON_IsString(forecast_code) && forecast_code->valuestring != nullptr &&
                                std::strcmp(forecast_code->valuestring, "200") == 0 &&
                                cJSON_IsString(high_temperature) && high_temperature->valuestring != nullptr &&
                                cJSON_IsString(low_temperature) && low_temperature->valuestring != nullptr;
    if (!valid_now || !valid_forecast) {
        const char* response_code = cJSON_IsString(now_code) && now_code->valuestring != nullptr
                                        ? now_code->valuestring
                                        : "unknown";
        ESP_LOGW(TAG, "QWeather response code %s has no usable weather data", response_code);
        cJSON_Delete(now_root);
        cJSON_Delete(forecast_root);
        return false;
    }

    std::string display_temperature = temperature->valuestring;
    display_temperature += " C";
    std::string display_feels_like = feels_like->valuestring;
    display_feels_like += "℃";
    std::string display_high_temperature = high_temperature->valuestring;
    display_high_temperature += "℃";
    std::string display_low_temperature = low_temperature->valuestring;
    display_low_temperature += "℃";
    std::string display_description = description->valuestring;
    std::string display_city = weather_city_;
    if (display_city.empty()) display_city = "定位中";
    cJSON_Delete(now_root);
    cJSON_Delete(forecast_root);

    // These endpoints are optional enrichments. A transient failure keeps the
    // last known notice instead of replacing a still-valid warning with blank.
    FetchWeatherNotices();

    {
        std::lock_guard<std::mutex> lock(holiday_mutex_);
        pending_weather_city_ = std::move(display_city);
        pending_weather_temperature_ = std::move(display_temperature);
        pending_weather_feels_like_ = std::move(display_feels_like);
        pending_weather_high_temperature_ = std::move(display_high_temperature);
        pending_weather_low_temperature_ = std::move(display_low_temperature);
        pending_weather_description_ = std::move(display_description);
        pending_weather_notice_ = weather_notice_;
        pending_weather_ready_ = true;
    }
    ESP_LOGI(TAG, "QWeather current and daily weather fetched for %s", weather_location_.c_str());
    return true;
}

void CustomLcdDisplay::ApplyPendingDashboardData() {
    std::string flags;
    int year = 0;
    std::string holiday_note;
    int holiday_note_year = 0;
    int holiday_note_day_of_year = 0;
    std::string city;
    std::string temperature;
    std::string feels_like;
    std::string high_temperature;
    std::string low_temperature;
    std::string description;
    std::string notice;
    std::string city_only;
    bool apply_holiday = false;
    bool apply_holiday_note = false;
    bool apply_city_only = false;
    bool apply_weather = false;
    {
        std::lock_guard<std::mutex> lock(holiday_mutex_);
        if (pending_holiday_ready_) {
            year = pending_holiday_year_;
            flags = std::move(pending_holiday_flags_);
            pending_holiday_ready_ = false;
            apply_holiday = true;
        }
        if (pending_holiday_note_ready_) {
            holiday_note_year = pending_holiday_note_year_;
            holiday_note_day_of_year = pending_holiday_note_day_of_year_;
            holiday_note = std::move(pending_holiday_note_);
            pending_holiday_note_ready_ = false;
            apply_holiday_note = true;
        }
        if (pending_weather_ready_) {
            city = std::move(pending_weather_city_);
            temperature = std::move(pending_weather_temperature_);
            feels_like = std::move(pending_weather_feels_like_);
            high_temperature = std::move(pending_weather_high_temperature_);
            low_temperature = std::move(pending_weather_low_temperature_);
            description = std::move(pending_weather_description_);
            notice = std::move(pending_weather_notice_);
            pending_weather_ready_ = false;
            apply_weather = true;
        }
        if (pending_weather_city_only_ready_) {
            city_only = std::move(pending_weather_city_only_);
            pending_weather_city_only_ready_ = false;
            apply_city_only = true;
        }
    }

    if (apply_holiday && dashboard_layout::SetHolidayCalendar(dashboard_state_, year, flags.c_str(), flags.size())) {
        ESP_LOGI(TAG, "Applied holiday calendar for %d", year);
    }
    if (apply_holiday_note && dashboard_layout::SetHolidayNote(dashboard_state_, holiday_note_year,
                                                                holiday_note_day_of_year, holiday_note.c_str())) {
        ESP_LOGI(TAG, "Applied holiday note for %d day %d: %s", holiday_note_year, holiday_note_day_of_year,
                 holiday_note.empty() ? "none" : holiday_note.c_str());
    }
    if (apply_city_only) {
        dashboard_layout::SetWeatherCity(dashboard_state_, city_only.c_str());
    }
    if (apply_weather) {
        dashboard_layout::SetWeather(dashboard_state_, city.c_str(), temperature.c_str(), feels_like.c_str(),
                                     high_temperature.c_str(), low_temperature.c_str(), description.c_str(),
                                     notice.c_str());
    }
}

void CustomLcdDisplay::StopHolidayCalendarTask() {
    if (holiday_task_ == nullptr) return;
    holiday_task_stop_ = true;
    xTaskNotifyGive(holiday_task_);
    for (int attempt = 0; attempt < 200 && holiday_task_running_.load(); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (holiday_task_running_.load()) {
        vTaskDelete(holiday_task_);
    }
    holiday_task_running_ = false;
    holiday_task_ = nullptr;
}

bool CustomLcdDisplay::RequestWeatherLocationUpdate() {
    if (CONFIG_RLCD_QWEATHER_API_HOST[0] == '\0' || CONFIG_RLCD_QWEATHER_API_KEY[0] == '\0') {
        ESP_LOGW(TAG, "Weather location update unavailable because QWeather is not configured");
        return false;
    }
    if (weather_location_manual_) {
        ESP_LOGW(TAG, "Weather location is fixed by RLCD_QWEATHER_LOCATION; AI update ignored");
        return false;
    }
    if (holiday_task_ == nullptr || !holiday_task_running_.load()) {
        ESP_LOGW(TAG, "Weather location update unavailable because the weather worker is not running");
        return false;
    }
    weather_location_refresh_requested_.store(true);
    xTaskNotifyGive(holiday_task_);
    ESP_LOGI(TAG, "Weather location update requested by AI");
    return true;
}

void CustomLcdDisplay::ShowDashboard() {
    DisplayLockGuard lock(this);
    dashboard_layout::Show(dashboard_state_);
    HideDefaultEmotion();
    if (clock_timer_ != nullptr) {
        esp_timer_start_periodic(clock_timer_, 1000000);
    }
}

void CustomLcdDisplay::HideDashboard() {
    DisplayLockGuard lock(this);
    dashboard_layout::Hide(dashboard_state_);
    if (clock_timer_ != nullptr) {
        esp_timer_stop(clock_timer_);
    }
}

void CustomLcdDisplay::HideDefaultEmotion() {
    // The base LcdDisplay keeps its centered robot/emotion widgets alive and
    // SetEmotion() can make them visible again.  They must not cover the
    // dashboard while the RLCD is in its standby layout.
    if (emoji_label_ != nullptr) {
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (preview_image_ != nullptr) {
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    }
}

void CustomLcdDisplay::SetupUI() {
    // 先调用基类 SetupUI 完成标准聊天界面的创建
    LcdDisplay::SetupUI();

    {
        DisplayLockGuard lock(this);
        HideDefaultEmotion();
    }

    // The standard top bar only contains the battery glyph. Add the numeric
    // level beside it for this board, using the same 30 px text font as the
    // dashboard so the value remains readable on the RLCD.
    if (battery_label_ != nullptr) {
        lv_obj_t* battery_parent = lv_obj_get_parent(battery_label_);
        if (battery_parent != nullptr) {
            battery_percent_label_ = lv_label_create(battery_parent);
            lv_label_set_text(battery_percent_label_, "--%");
            lv_obj_set_style_text_font(battery_percent_label_, &BUILTIN_TEXT_FONT, 0);
            lv_obj_set_style_text_color(battery_percent_label_,
                                         lv_obj_get_style_text_color(battery_label_, LV_PART_MAIN), 0);
            lv_obj_set_style_margin_left(battery_percent_label_, 2, 0);
        }
    }

    // 在标准 UI 之上叠加仪表盘
    SetupDashboard();
}

void CustomLcdDisplay::UpdateStatusBar(bool update_all) {
    LcdDisplay::UpdateStatusBar(update_all);
    if (battery_percent_label_ == nullptr || battery_level_ < 0 ||
        displayed_battery_percent_ == battery_level_) {
        return;
    }

    char percent[12];
    std::snprintf(percent, sizeof(percent), "%d%%", battery_level_);
    DisplayLockGuard lock(this);
    if (battery_percent_label_ != nullptr) {
        lv_label_set_text(battery_percent_label_, percent);
        displayed_battery_percent_ = battery_level_;
    }
}

void CustomLcdDisplay::SetStatus(const char* status) {
    // 先调用基类处理状态文字显示
    LcdDisplay::SetStatus(status);
    // The base display periodically writes HH:MM to the status label while
    // idle. That is already represented by the dashboard clock; treating it
    // as a state change would hide the dashboard and reveal the smiley page.
    if (status != nullptr &&
        (strcmp(status, Lang::Strings::STANDBY) == 0 || IsClockStatus(status))) {
        ShowDashboard();
    } else {
        HideDashboard();
    }
}

void CustomLcdDisplay::SetEmotion(const char* emotion) {
    // Preserve the normal emotion behavior while the assistant is active.
    LcdDisplay::SetEmotion(emotion);

    // Application updates the emotion immediately after SetStatus().  That
    // second call used to reveal the centered robot again after the dashboard
    // had been shown, so re-apply the dashboard-only hide here.
    if (dashboard_state_.root == nullptr) return;
    DisplayLockGuard lock(this);
    if (!lv_obj_has_flag(dashboard_state_.root, LV_OBJ_FLAG_HIDDEN)) {
        HideDefaultEmotion();
    }
}
