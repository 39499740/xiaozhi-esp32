# Mac LVGL Dashboard 预览环境 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 建立 Mac 本地 LVGL+SDL 预览环境,让 RLCD-4.2 板的 dashboard 布局代码修改后秒级看到效果(无需烧录),固件与预览共享同一份布局代码。

**Architecture:** 把 dashboard 布局逻辑(纯 LVGL 调用)从 `custom_lcd_display.cc` 抽出到独立的 `dashboard_layout.h/.cc`,固件和 Mac 预览都调用它。固件侧保留 `esp_timer`/`DisplayLockGuard`/SPI;Mac 预览用 SDL2 + LVGL 9.5 黑白窗口。

**Tech Stack:** LVGL 9.5.0、SDL2(已装 2.32.2)、CMake、C++17、ESP-IDF v6.0.2(固件侧)、系统 clang(Mac 预览侧)

**Spec:** `docs/superpowers/specs/2026-07-24-mac-lvgl-dashboard-preview-design.md`

---

## 文件结构

| 文件 | 职责 | 动作 |
|---|---|---|
| `main/boards/waveshare/esp32-s3-rlcd-4.2/dashboard_layout.h` | 纯 LVGL 布局接口:`DashboardState` 结构 + `Setup/Refresh/Show/Hide` 声明 | 新建 |
| `main/boards/waveshare/esp32-s3-rlcd-4.2/dashboard_layout.cc` | 纯 LVGL 布局实现(从 custom_lcd_display.cc 350-447 搬迁,零布局改动) | 新建 |
| `main/boards/waveshare/esp32-s3-rlcd-4.2/custom_lcd_display.h` | 持有 `DashboardState state_` 成员;方法签名不变 | 改 |
| `main/boards/waveshare/esp32-s3-rlcd-4.2/custom_lcd_display.cc` | 改为薄封装:调用 `dashboard_layout::*`,保留 esp_timer | 改 |
| `main/CMakeLists.txt` | 把 `dashboard_layout.cc` 加入 rlcd-4.2 板源文件列表 | 改 |
| `preview/CMakeLists.txt` | Mac 构建脚本:SDL2 + LVGL 9.5 + 字体 + dashboard_layout | 新建 |
| `preview/main.cc` | SDL 窗口 + LVGL init + 调用 dashboard_layout + 键盘交互 | 新建 |
| `preview/lv_conf.h` | LVGL 配置(Mac 预览专用) | 新建 |
| `preview/README.md` | 使用说明 | 新建 |

---

## Task 1: 新建 dashboard_layout.h(纯接口)

**Files:**
- Create: `main/boards/waveshare/esp32-s3-rlcd-4.2/dashboard_layout.h`

- [ ] **Step 1: 创建头文件**

```cpp
#ifndef DASHBOARD_LAYOUT_H
#define DASHBOARD_LAYOUT_H

#include <lvgl.h>

// dashboard 布局状态:所有 LVGL 对象句柄 + 尺寸。
// 由调用方持有,布局函数无状态、可重入、不依赖任何类继承。
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

// 星期名称(中文)。声明在这里供固件和预览共用。
extern const char* kWeekdays[7];

namespace dashboard_layout {

// 在 parent 下创建 dashboard UI(原 SetupDashboard 逻辑,布局逐字搬迁)。
// font 由调用方传入:固件传 &BUILTIN_TEXT_FONT,预览传 &font_noto_sans_basic_14_1。
// 注意:此函数仅创建 LVGL 对象,不创建任何定时器(定时器由调用方负责)。
void Setup(DashboardState& s, lv_obj_t* parent, int width, int height, const lv_font_t* font);

// 刷新时钟/日期文本(原 RefreshClock 的 LVGL 部分)。
// 调用方负责加锁(固件用 DisplayLockGuard)和定时驱动(固件用 esp_timer,预览用 lv_timer)。
void Refresh(DashboardState& s);

// 显示/隐藏 dashboard(纯 LVGL flag 操作)。
void Show(DashboardState& s);
void Hide(DashboardState& s);

}  // namespace dashboard_layout

#endif  // DASHBOARD_LAYOUT_H
```

- [ ] **Step 2: 验证头文件语法(用 clang 做语法检查,需先确认 lvgl.h 在哪)**

```bash
# 先确认固件侧 lvgl.h 路径(供后续参考,本步骤不强制编译此头)
ls /Users/hao/MyProject/xiaozhi-esp32/managed_components/lvgl__lvgl/lvgl.h
```
Expected: 文件存在。头文件本身不单独编译(它依赖 lvgl.h,要在固件/预览构建里验证)。

- [ ] **Step 3: Commit**

```bash
cd /Users/hao/MyProject/xiaozhi-esp32
git add main/boards/waveshare/esp32-s3-rlcd-4.2/dashboard_layout.h
git commit -m "feat(rlcd-4.2): add dashboard_layout.h - portable LVGL dashboard interface"
```

---

## Task 2: 新建 dashboard_layout.cc(从 custom_lcd_display.cc 搬迁)

**Files:**
- Create: `main/boards/waveshare/esp32-s3-rlcd-4.2/dashboard_layout.cc`
- Reference: `main/boards/waveshare/esp32-s3-rlcd-4.2/custom_lcd_display.cc:348-447` (源代码)

**重要原则**:布局代码**逐字搬迁,不改任何坐标、字号、对齐**。这是纯重构,视觉零变化。改动只限于:① 去掉 `CustomLcdDisplay::` 类前缀,改用 `DashboardState& s` 参数;② 去掉 `DisplayLockGuard`(移到调用方);③ 去掉 `esp_timer` 相关(移到调用方)。

- [ ] **Step 1: 创建 dashboard_layout.cc**

```cpp
#include "dashboard_layout.h"

#include <cstring>
#include <ctime>

// 星期名称(中文)—— 定义(原 custom_lcd_display.cc:20-21)
const char* kWeekdays[] = {"星期日", "星期一", "星期二", "星期三",
                           "星期四", "星期五", "星期六"};

namespace dashboard_layout {

void Setup(DashboardState& s, lv_obj_t* parent, int width, int height, const lv_font_t* font) {
    s.width = width;
    s.height = height;

    // 仪表盘根容器:覆盖整个屏幕,置于所有聊天 UI 之上
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

    // 右上:时钟(放大显示)
    s.clock_label = lv_label_create(s.root);
    lv_obj_set_style_text_font(s.clock_label, font, 0);
    lv_obj_set_style_transform_scale(s.clock_label, 256 * 3, 0);
    lv_obj_align(s.clock_label, LV_ALIGN_TOP_RIGHT, -20, 55);
    lv_label_set_text(s.clock_label, "00:00");

    // 右下:天气(温度+文字,占位)
    s.weather_label = lv_label_create(s.root);
    lv_obj_set_style_text_font(s.weather_label, font, 0);
    lv_obj_set_style_transform_scale(s.weather_label, 256 * 2, 0);
    lv_obj_align(s.weather_label, LV_ALIGN_BOTTOM_RIGHT, -20, -50);
    lv_label_set_text(s.weather_label, "--°");

    // 右下:城市名(小字,不放大)
    s.city_label = lv_label_create(s.root);
    lv_obj_set_style_text_font(s.city_label, font, 0);
    lv_obj_align(s.city_label, LV_ALIGN_BOTTOM_RIGHT, -20, -15);
    lv_label_set_text(s.city_label, "定位中...");

    // 默认先隐藏,等进入待机状态时显示
    Hide(s);

    // 立即刷新一次时钟显示
    Refresh(s);
}

void Refresh(DashboardState& s) {
    if (s.clock_label == nullptr) return;

    time_t now = time(nullptr);
    struct tm timeinfo;
    // 用 localtime_r(预览和固件都适用:固件因 ota.cc 已写入本地时间,localtime_r 返回本地时间)
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
```

- [ ] **Step 2: Commit**

```bash
cd /Users/hao/MyProject/xiaozhi-esp32
git add main/boards/waveshare/esp32-s3-rlcd-4.2/dashboard_layout.cc
git commit -m "feat(rlcd-4.2): add dashboard_layout.cc - portable LVGL dashboard impl"
```

---

## Task 3: 改 custom_lcd_display.h 持有 DashboardState

**Files:**
- Modify: `main/boards/waveshare/esp32-s3-rlcd-4.2/custom_lcd_display.h`

- [ ] **Step 1: 在文件顶部 include dashboard_layout.h**

读取当前文件确认结构,然后在 `#include "lcd_display.h"` 之后加一行 `#include "dashboard_layout.h"`。

当前第 7-8 行:
```cpp
#include <string>
#include "lcd_display.h"
```
改为:
```cpp
#include <string>
#include "lcd_display.h"
#include "dashboard_layout.h"
```

- [ ] **Step 2: 替换 dashboard 成员为 DashboardState**

当前 custom_lcd_display.h:48-66 有一堆独立成员(dashboard_root_、clock_label_ 等)和私有方法声明(SetupDashboard/RefreshClock 等)。把它们整体替换为一个 `DashboardState dashboard_state_;` 成员,并删除已迁移到 dashboard_layout 的私有方法声明。

**删除**这些声明(48-66 行的成员 + 62-66 行的方法声明):
```cpp
    // ---- 待机仪表盘（时钟 / 日历 / 天气）----
    lv_obj_t* dashboard_root_ = nullptr;
    lv_obj_t* clock_label_   = nullptr;
    lv_obj_t* clock_label_   = nullptr;  // (删除所有这类成员)
    ... (所有 dashboard 相关 lv_obj_t* 成员)
    esp_timer_handle_t clock_timer_ = nullptr;
    esp_timer_handle_t weather_timer_ = nullptr;
    bool weather_ok_ = false;
    std::string city_name_ = "定位中...";
    std::string weather_text_ = "--";
    int weather_temp_ = -999;

    void SetupDashboard();
    void RefreshClock();
    static void ClockTimerCallback(void* arg);
    void ShowDashboard();
    void HideDashboard();
```

**替换为**:
```cpp
    // ---- 待机仪表盘 ----
    DashboardState dashboard_state_;        // 布局状态(共享给 dashboard_layout)
    esp_timer_handle_t clock_timer_ = nullptr;  // ESP 专属:1 秒刷新时钟
    static void ClockTimerCallback(void* arg);
    void ShowDashboard();
    void HideDashboard();
```

注意保留 `SetupUI()` 和 `SetStatus()` 的 override 声明(79-80 行不变)。

- [ ] **Step 3: Commit**

```bash
cd /Users/hao/MyProject/xiaozhi-esp32
git add main/boards/waveshare/esp32-s3-rlcd-4.2/custom_lcd_display.h
git commit -m "refactor(rlcd-4.2): hold DashboardState in custom_lcd_display.h"
```

---

## Task 4: 改 custom_lcd_display.cc 为薄封装

**Files:**
- Modify: `main/boards/waveshare/esp32-s3-rlcd-4.2/custom_lcd_display.cc`

**目标**:把 350-487 行的 dashboard 实现替换为调用 `dashboard_layout::*` 的薄封装,保留 `esp_timer`。删除旧的 `kWeekdays` 定义(已移到 dashboard_layout.cc)、旧的 `SetupDashboard`/`RefreshClock`(逻辑已迁移)。

- [ ] **Step 1: 删除旧的 kWeekdays 定义(已迁移到 dashboard_layout.cc)**

删除 custom_lcd_display.cc:19-21:
```cpp
// 星期名称（中文）
static const char* kWeekdays[] = {"星期日", "星期一", "星期二", "星期三",
                                  "星期四", "星期五", "星期六"};
```
(因为 dashboard_layout.cc 已定义同名符号,留这里会重复定义导致链接错误)

- [ ] **Step 2: 替换 SetupDashboard 实现**

原 custom_lcd_display.cc:350-415(`void CustomLcdDisplay::SetupDashboard() { ... }` 整段)替换为:

```cpp
void CustomLcdDisplay::SetupDashboard() {
    // 调用共享布局代码创建 UI(纯 LVGL,无 ESP 依赖)
    auto screen = lv_screen_active();
    dashboard_layout::Setup(dashboard_state_, screen, width_, height_, &BUILTIN_TEXT_FONT);

    // 创建 ESP 专属的时钟定时器(1 秒周期)
    esp_timer_create_args_t clock_timer_args = {
        .callback = ClockTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "dash_clock",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&clock_timer_args, &clock_timer_);
}
```

- [ ] **Step 3: 替换 RefreshClock 实现**

原 custom_lcd_display.cc:417-447(`ClockTimerCallback` + `RefreshClock` 两段)替换为:

```cpp
void CustomLcdDisplay::ClockTimerCallback(void* arg) {
    auto* self = static_cast<CustomLcdDisplay*>(arg);
    DisplayLockGuard lock(self);
    dashboard_layout::Refresh(self->dashboard_state_);
}
```

(原 RefreshClock 整个删除,逻辑已迁移到 dashboard_layout::Refresh)

- [ ] **Step 4: 替换 Show/HideDashboard 实现**

原 custom_lcd_display.cc:449-469 替换为:

```cpp
void CustomLcdDisplay::ShowDashboard() {
    DisplayLockGuard lock(this);
    dashboard_layout::Show(dashboard_state_);
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
```

- [ ] **Step 5: 验证 SetupUI 和 SetStatus 无需改动**

确认 SetupUI(471-476)和 SetStatus(478-487)调用的是 `SetupDashboard()`/`ShowDashboard()`/`HideDashboard()`,这些方法名没变,只是内部实现换了。**这两段不动**。

- [ ] **Step 6: 编译验证固件(关键安全检查点)**

```bash
# 激活 IDF 环境
export IDF_PATH="$HOME/esp/esp-idf"
export PATH="/opt/homebrew/opt/python@3.11/bin:$IDF_PATH/tools:$PATH"
source "$IDF_PATH/export.sh" >/dev/null 2>&1

cd /Users/hao/MyProject/xiaozhi-esp32
idf.py build 2>&1 | tail -20
```
Expected: `Project build complete` 且无 error。如有编译错误,通常是符号重复定义或头文件缺失,根据报错修。

- [ ] **Step 7: Commit**

```bash
cd /Users/hao/MyProject/xiaozhi-esp32
git add main/boards/waveshare/esp32-s3-rlcd-4.2/custom_lcd_display.cc
git commit -m "refactor(rlcd-4.2): use dashboard_layout, keep esp_timer in firmware"
```

---

## Task 5: 把 dashboard_layout.cc 加入固件 CMakeLists

**Files:**
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: 找到 rlcd-4.2 板的源文件列表**

```bash
grep -n "rlcd-4.2\|custom_lcd_display.cc\|waveshare/esp32-s3-rlcd" /Users/hao/MyProject/xiaozhi-esp32/main/CMakeLists.txt | head -20
```
找到 rlcd-4.2 板在 CMake 里如何注册 `custom_lcd_display.cc`(可能在某个 `if(CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_RLCD_4_2)` 块,或通过通配符)。

- [ ] **Step 2: 把 dashboard_layout.cc 加入同一列表**

如果源文件是显式列出的,在 `custom_lcd_display.cc` 旁边加 `dashboard_layout.cc`。如果是 `file(GLOB)` 通配符,则无需改动(自动包含)——此时跳到 Step 3。

预期改法示例(具体行号按实际):
```cmake
target_sources(main PRIVATE
    boards/waveshare/esp32-s3-rlcd-4.2/custom_lcd_display.cc
    boards/waveshare/esp32-s3-rlcd-4.2/dashboard_layout.cc
)
```

- [ ] **Step 3: 重新编译验证**

```bash
idf.py build 2>&1 | tail -10
```
Expected: `Project build complete`。如果 GLOB 自动包含了,这步会直接通过。

- [ ] **Step 4: Commit(如有改动)**

```bash
git add main/CMakeLists.txt
git commit -m "build(rlcd-4.2): add dashboard_layout.cc to firmware sources"
```
(若 GLOB 自动包含无改动,跳过 commit)

---

## Task 6: 烧录验证固件 dashboard 行为不变

**Files:** 无(纯验证)

- [ ] **Step 1: 烧录到板子**

```bash
idf.py -p /dev/cu.usbmodem1101 flash monitor
```
Expected: 烧录成功,板子启动。按 Ctrl+] 退出 monitor。

- [ ] **Step 2: 人工确认 dashboard 行为**

等待设备进入待机态(配网后或观察日志显示 standby)。确认:
- 待机时 dashboard 显示出来(即使布局丑,也要"显示出来了")
- 时钟每秒刷新
- 离开待机时 dashboard 隐藏

**此步是安全检查点**:重构后行为应与重构前完全一致。如果 dashboard 不显示,说明重构引入了 bug,回滚 Task 3-5 排查。

- [ ] **Step 3: 记录验证结果**

在计划文档或对话中记录:"固件重构验证通过,dashboard 行为不变"。

---

## Task 7: 新建 preview 目录与 lv_conf.h

**Files:**
- Create: `preview/lv_conf.h`
- Create: `preview/README.md`

- [ ] **Step 1: 创建 preview 目录**

```bash
mkdir -p /Users/hao/MyProject/xiaozhi-esp32/preview
```

- [ ] **Step 2: 创建 lv_conf.h(Mac 预览专用,最小配置)**

使用 LVGL 内置 SDL 驱动(9.5 起 SDL 驱动已内置在 `src/drivers/sdl`,不再需要单独的 `lv_drivers` 仓库),用 `LV_USE_SDL` 启用。

```c
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN

#define LV_USE_MALLOC 1
#define LV_MEM_SIZE (128 * 1024)
#define LV_MEM_CUSTOM 0

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

#define LV_HOR_RES_MAX 400
#define LV_VER_RES_MAX 300
#define LV_DPI_DEF 130

/* 启用 LVGL 内置 SDL 驱动(9.5 起内置在 src/drivers/sdl) */
#define LV_USE_SDL 1
#define LV_USE_DRAW_SDL 1
#define LV_USE_OS LV_OS_NONE

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_FONT_PLACEHOLDER 1

#define LV_USE_OBJX_NAME 1

#define LV_BUILD_EXAMPLES 0

#endif
```

- [ ] **Step 3: 创建 README.md**

```markdown
# RLCD-4.2 Dashboard 预览(Mac)

在 Mac 上预览 dashboard 布局,无需烧录。

## 前置依赖

- macOS + SDL2(已通过 brew 安装)
- CMake + make(系统自带)

## 构建

\`\`\`bash
cd preview
mkdir build && cd build
cmake ..
make
\`\`\`

## 运行

\`\`\`bash
./dashboard_preview
\`\`\`

## 交互

- `S` 键:显示 dashboard(模拟待机)
- `H` 键:隐藏 dashboard
- `Q` 或关窗口:退出

## 改布局

编辑 `../main/boards/waveshare/esp32-s3-rlcd-4.2/dashboard_layout.cc`(固件和预览共享),
然后在 build 目录里 `make` 重新编译(秒级),重新运行即可看效果。
```

- [ ] **Step 4: Commit**

```bash
cd /Users/hao/MyProject/xiaozhi-esp32
git add preview/lv_conf.h preview/README.md
git commit -m "feat(preview): add lv_conf.h and README for Mac dashboard preview"
```

---

## Task 8: 新建 preview/main.cc(SDL 窗口 + LVGL + dashboard)

**Files:**
- Create: `preview/main.cc`

- [ ] **Step 1: 创建 main.cc**

使用 LVGL 9.5 内置 SDL 驱动(`lv_sdl_window_create`),无需手写 flush callback。这是 9.5 推荐做法(SDL 驱动已内置在 `src/drivers/sdl`)。

```cpp
// Mac LVGL+SDL 预览:RLCD-4.2 dashboard 布局
// 使用 LVGL 9.5 内置 SDL 驱动,无需手写 flush callback。
#define SDL_MAIN_HANDLED  // 避免 SDL 改写 main(Windows 需要,macOS 无害)
#include <lvgl.h>
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
    LV_UNUSED(argc); LV_UNUSED(argv);

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
    g_clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);

    // 默认显示 dashboard
    dashboard_layout::Show(g_state);

    printf("Preview running. Close window or press Q to quit.\n");
    printf("Keys are handled by LVGL SDL input (mouse works).\n");

    // LVGL 9.5 主循环:SDL 驱动内部处理事件,只需调 lv_timer_handler
    while (1) {
        lv_timer_handler();
        lv_delay_ms(5);
    }
    return 0;
}
```

**注意**:9.5 内置 SDL 驱动自动处理窗口、输入、flush、tick,所以 main 极简。
- 不需要手写 SDL 事件循环(SDL 驱动内部处理)
- 不需要手写 flush callback(`lv_sdl_window_create` 内部设置)
- 不需要手动 `lv_tick_inc`(SDL 驱动内部管理)
- 窗口关闭即退出程序(SDL 驱动处理 SDL_QUIT)

键盘交互(S/H)在纯布局预览阶段不是必须的——dashboard 默认显示就够看布局了。若后续需要切换显隐,可在 dashboard_layout.cc 里暴露一个全局按键回调,本计划暂不实现(YAGNI)。

- [ ] **Step 2: Commit**

```bash
cd /Users/hao/MyProject/xiaozhi-esp32
git add preview/main.cc
git commit -m "feat(preview): add SDL+LVGL main for Mac dashboard preview"
```

---

## Task 9: 新建 preview/CMakeLists.txt

**Files:**
- Create: `preview/CMakeLists.txt`

- [ ] **Step 1: 创建 CMakeLists.txt**

关键:LVGL 9.5 的 SDL 驱动在 `src/drivers/sdl`,需要把 SDL2 的 include 路径传给 LVGL 目标(`LV_LVGL_H_INCLUDE_SIMPLE`),并链接 SDL2。

```cmake
cmake_minimum_required(VERSION 3.16)
project(dashboard_preview C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# SDL2(已 brew 安装)
find_package(SDL2 REQUIRED)

# LVGL 9.5 - 用 FetchContent 拉取,锁定到 release/v9.5
include(FetchContent)
FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG release/v9.5
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(lvgl)

# dashboard_layout 与固件共享
set(DASHBOARD_DIR ${CMAKE_SOURCE_DIR}/../main/boards/waveshare/esp32-s3-rlcd-4.2)

# 字体 C 源文件(与固件共享)
set(FONT_DIR ${CMAKE_SOURCE_DIR}/../managed_components/78__xiaozhi-fonts/src)
set(FONT_SRC ${FONT_DIR}/font_noto_sans_basic_14_1.c)

add_executable(dashboard_preview
    main.cc
    ${DASHBOARD_DIR}/dashboard_layout.cc
    ${FONT_SRC}
)

target_include_directories(dashboard_preview PRIVATE
    ${CMAKE_SOURCE_DIR}              # 找到 lv_conf.h
    ${DASHBOARD_DIR}                 # 找到 dashboard_layout.h
)

# LVGL 配置:用我们的 lv_conf.h,且让 LVGL 用 <lvgl.h> 而非 "lvgl/lvgl.h"
target_compile_definitions(dashboard_preview PRIVATE
    LV_CONF_INCLUDE_SIMPLE=1
    LV_LVGL_H_INCLUDE_SIMPLE=1
)
# 同样传给 lvgl 目标(否则 LVGL 内部用不到我们的 lv_conf.h)
target_compile_definitions(lvgl PRIVATE
    LV_CONF_INCLUDE_SIMPLE=1
    LV_LVGL_H_INCLUDE_SIMPLE=1
)
# SDL2 include 路径传给 lvgl(内置 SDL 驱动需要 SDL.h)
target_include_directories(lvgl PRIVATE ${SDL2_INCLUDE_DIRS})

target_link_libraries(dashboard_preview PRIVATE
    lvgl
    ${SDL2_LIBRARIES}
)
```

- [ ] **Step 2: Commit**

```bash
cd /Users/hao/MyProject/xiaozhi-esp32
git add preview/CMakeLists.txt
git commit -m "feat(preview): add CMakeLists for Mac dashboard preview build"
```

---

## Task 10: 构建并运行 Mac 预览(首次验证)

**Files:** 无(纯构建验证)

- [ ] **Step 1: CMake 配置**

```bash
cd /Users/hao/MyProject/xiaozhi-esp32/preview
mkdir -p build && cd build
cmake .. 2>&1 | tail -20
```
Expected: `-- Generating done` 且无 error。FetchContent 会下载 LVGL 9.5(首次约 1-2 分钟)。如报错通常是 SDL2 找不到或网络问题。

- [ ] **Step 2: 编译**

```bash
make 2>&1 | tail -30
```
Expected: `[100%] Built target dashboard_preview` 且无 error。首次编译 LVGL 较慢(1-3 分钟),之后增量秒级。如报错:
- `font_noto_sans_basic_14_1` 未定义:检查 FONT_SRC 路径
- `lv_*` 未定义:检查 LVGL 是否 FetchContent 成功
- `dashboard_layout.h` 找不到:检查 target_include_directories

- [ ] **Step 3: 运行预览**

```bash
./dashboard_preview
```
Expected: 弹出 800x600 窗口(400x300 放大 2 倍),显示当前 dashboard 布局(白底黑字,时钟+日期+星期+天气占位+城市占位)。时钟每秒刷新。按 S/H/Q 测试交互。

- [ ] **Step 4: 确认反馈闭环成立**

改一个明显的坐标(例如把 `dashboard_layout.cc` 里 clock_label 的 `lv_obj_align(..., -20, 55)` 改成 `lv_obj_align(..., -20, 5)`),在 build 目录 `make && ./dashboard_preview`,确认窗口里时钟位置变了。改完后**记得改回原值**。

- [ ] **Step 5: Commit(若 Task 10 期间修了小问题)**

```bash
# 仅当修复了构建问题才 commit,否则跳过
```

---

## 完成标准

所有 Task 完成后必须满足:
1. ✅ 固件 `idf.py build` 成功(Task 5 验证)
2. ✅ 固件烧录后 dashboard 行为与重构前一致(Task 6 验证)
3. ✅ `preview/build/dashboard_preview` 能打开窗口显示 dashboard(Task 10 验证)
4. ✅ 改 `dashboard_layout.cc` → `make` → 重新运行,秒级看到布局变化(Task 10 Step 4 验证)
5. ✅ 固件和预览共享同一个 `dashboard_layout.cc`,无重复代码

## 后续(本计划之外)

预览跑起来后,在预览里看着调整布局(去 transform_scale、调坐标字号、确定最终方案),每改一处 `make` 验证。布局定稿后再烧录到板子做单色屏最终确认。
