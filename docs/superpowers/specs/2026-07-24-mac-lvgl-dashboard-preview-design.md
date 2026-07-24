# 设计:Mac LVGL Dashboard 预览环境

**日期**:2026-07-24
**板子**:Waveshare ESP32-S3 4.2" RLCD(`main/boards/waveshare/esp32-s3-rlcd-4.2/`)
**目标读者**:做这块板二次开发的本人

## 背景与动机

当前 RLCD 板的待机界面(dashboard)开发存在严重的反馈闭环缺失:

1. **坐标/字号靠猜**:`lv_obj_align(..., 25, -40)` 这类偏移量,不实际渲染根本不知道落在屏上哪个位置。
2. **无法看见烧录结果**:开发助手(ZCode)看不到硬件屏幕,无法给出"往左移一点"这类反馈。
3. **`transform_scale` 在单色屏失真**:放大字体产生锯齿,实际效果与预期不符。
4. **每次调 UI 必须烧录**:改 → build → flash → monitor 循环约 1 分钟,迭代慢且"烧了才发现丑"。

本方案建立一个 **Mac 本地 LVGL+SDL 预览**,让 dashboard 布局代码修改后秒级看到效果(无需烧录),从而建立可靠的反馈闭环。

## 可行性依据(已完成调研)

调研确认 dashboard UI 代码与 ESP32 硬件耦合度极低:

| 代码段 | 行数 | ESP 专属调用 | 纯 LVGL 调用 |
|---|---|---|---|
| `SetupDashboard` | ~65 | 仅 1 处(`esp_timer_create`) | ~30 个 `lv_*` |
| `RefreshClock` | ~25 | 0 | 3 个 `lv_label_set_text` |
| `Show/HideDashboard` | ~20 | 2 处(`esp_timer_*`) | 3 个 `lv_*` |

所有 SPI、GPIO、PSRAM、`heap_caps_malloc` **只在构造函数和 `RLCD_Init` 里**,与布局代码完全隔离。字体 `font_noto_sans_basic_14_1.c` 是标准 LVGL C 数组(仅依赖 `lvgl.h`),可移植。

**环境依赖已确认就位**:
- LVGL 9.5.0(xiaozhi `idf_component.yml` 指定 `~9.5.0`)
- SDL2 2.32.2(已通过 brew 安装在 `/opt/homebrew/Cellar/sdl2/2.32.2`)
- Apple Silicon arm64 原生支持,无需 Rosetta

## 目标与非目标

### 目标
- dashboard 布局代码修改后,在 Mac 上秒级预览(无需烧录)。
- 固件和 Mac 预览**共享同一份布局代码**,保证一致性。
- 预览窗口为黑白双色(模拟单色屏观感,但不做阈值化后处理)。

### 非目标(YAGNI)
- ❌ 不模拟 1-bit 阈值化(锯齿效果仍需烧录验证)。
- ❌ 不改现有布局(`transform_scale` 等,等预览起来后"看着改")。
- ❌ 不模拟硬件层(SPI/PSRAM/真实面板)。
- ❌ 不接天气数据(布局稳定后的独立任务)。
- ❌ 不预览基类 `LcdDisplay::SetupUI`(聊天 UI/表情脸),仅聚焦 dashboard。

## 架构

```
┌──────────────────────────────────────────────────┐
│  main/boards/waveshare/esp32-s3-rlcd-4.2/        │
│                                                   │
│  dashboard_layout.h     ← 新建:纯 LVGL 接口      │
│  dashboard_layout.cc    ← 新建:纯 LVGL 实现      │
│   (Setup/Refresh/Show/Hide,无 esp_*/freertos)    │
│                                                   │
│  custom_lcd_display.h    ← 改:持有 DashboardState│
│  custom_lcd_display.cc   ← 改:调用 layout 函数   │
│   (ESP 版:esp_timer + Lock + SPI 仍在此)        │
└──────────────────────────────────────────────────┘
                       ↑ 共享同一份布局代码
┌──────────────────────────────────────────────────┐
│  preview/  (新建,仓库顶层目录)                  │
│                                                   │
│  main.cc           ← SDL 窗口 + LVGL init        │
│  CMakeLists.txt    ← Mac 构建,链 SDL2 + LVGL 9.5│
│  README.md         ← 使用说明                    │
└──────────────────────────────────────────────────┘
```

### 数据结构

布局的"状态"(所有 label 句柄、尺寸)放进一个 POD struct,由调用方持有。这样布局函数无状态、可重入、不依赖任何类继承:

```cpp
// dashboard_layout.h
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

namespace dashboard_layout {
    // 在 parent 下创建 dashboard UI(原 SetupDashboard 逻辑)
    // font 由调用方传入(固件传 &BUILTIN_TEXT_FONT,预览传 &font_noto_sans_basic_14_1)
    void Setup(DashboardState& s, lv_obj_t* parent, int width, int height, const lv_font_t* font);
    // 刷新时钟/日期文本(原 RefreshClock 逻辑)
    void Refresh(DashboardState& s);
    // 显示/隐藏(原 Show/HideDashboard 逻辑,纯 LVGL 部分)
    void Show(DashboardState& s);
    void Hide(DashboardState& s);
}
```

### 固件侧集成

`CustomLcdDisplay` 持有 `DashboardState state_` 成员,原方法改为薄封装:

- `SetupDashboard()` → 创建 screen、调用 `dashboard_layout::Setup(state_, screen, width_, height_)`,然后创建 `esp_timer`(ESP 专属,留在此处)。
- `RefreshClock()` → 调用 `dashboard_layout::Refresh(state_)`(若需加锁,在此处加 `DisplayLockGuard`)。
- `ShowDashboard()` / `HideDashboard()` → 调用 layout 的 Show/Hide + 启停 `esp_timer`。

**布局逻辑零改动**,只是搬家。

### Mac 预览侧

`preview/main.cc`:
1. SDL 创建 400×300 窗口(可放大 2x 显示更清楚,但逻辑分辨率仍是 400×300)。
2. `lv_init()` + 创建 SDL display。
3. 背景白色,文本颜色设为黑色(模拟单色屏观感)。
4. 调用 `dashboard_layout::Setup(state_, lv_screen_active(), 400, 300)`。
5. 用 `lv_timer_create` 每 1 秒调用 `dashboard_layout::Refresh(state_)`(替代 esp_timer)。
6. 主循环跑 LVGL tick + SDL 事件。
7. 按 `S` 键模拟待机(`Show`)、按 `H` 键隐藏(`Hide`),便于观察两种状态。

### 字体处理

直接把 `managed_components/78__xiaozhi-fonts/src/font_noto_sans_basic_14_1.c` 编进预览构建(CMake 里 add 这个源文件 + include 路径)。固件侧 `LV_FONT_DECLARE(BUILTIN_TEXT_FONT)` 保持不变。

**关键澄清**:`dashboard_layout.cc` **不直接引用 `BUILTIN_TEXT_FONT` 这个宏**(那是固件 CMake 定义的)。布局代码改为接收一个 `const lv_font_t* font` 参数,由调用方传入:
- 固件侧 `custom_lcd_display.cc` 传入 `&BUILTIN_TEXT_FONT`
- 预览侧 `preview/main.cc` 传入 `&font_noto_sans_basic_14_1`(直接 extern 引用)

这样布局代码对字体符号零硬依赖,两边各自提供具体字体对象。

## Stub 清单(Mac 预览需要提供的假实现)

| 原 ESP 符号 | Mac 替代 | 说明 |
|---|---|---|
| `esp_timer_create/start/stop` | `lv_timer_create` | 1 秒周期回调 |
| `DisplayLockGuard` / `Lock/Unlock` | no-op(`return true` / 空) | 单线程无需锁 |
| `ESP_LOGE/W/I` | `printf` 或空宏 | 仅日志 |
| `BUILTIN_TEXT_FONT` | 同名 declare + 编入 .c | 字体数组共享 |

**关键**:布局代码本身**不需要任何 stub** —— 它只调 `lv_*`。stub 只存在于固件侧封装层和预览 main 里。

## 构建方式

预览不依赖 ESP-IDF,用系统 clang + CMake:

```sh
cd preview
mkdir build && cd build
cmake .. && make
./dashboard_preview
```

CMakeLists.txt 核心:
- `find_package(SDL2 REQUIRED)`
- 拉取 LVGL 9.5.x(用 FetchContent 或 git submodule 指向 release/v9.5)
- 编译 `preview/main.cc` + `dashboard_layout.cc` + `font_noto_sans_basic_14_1.c` + LVGL 源码
- 链接 SDL2

## 验证标准

完成后必须满足:
1. ✅ **固件仍能编译烧录**:在 `~/MyProject/xiaozhi-esp32` 跑 `idf.py build` 成功,dashboard 行为不变(待机显示、时钟刷新、隐藏)。
2. ✅ **Mac 预览能跑**:`preview/build/dashboard_preview` 打开一个 400×300 黑白窗口,显示当前 dashboard 布局。
3. ✅ **改布局后秒级反馈**:改 `dashboard_layout.cc` 任意坐标/字号 → 重 make(秒级)→ 窗口刷新看到效果。
4. ✅ **单一代码源**:固件和预览调用的布局函数是**同一个 `dashboard_layout.cc`**,无重复。

## 已知局限(诚实声明)

1. **锯齿看不到**:Mac RGB 抗锯齿渲染,`transform_scale` 放大的锯齿在预览里看不到,仍需烧录验证单色效果。
2. **基类 UI 不预览**:预览只画 dashboard,不画基类的表情脸/聊天泡/状态栏。待机时实际屏幕会有基类 UI 在底层(被 dashboard 覆盖),预览不模拟这层。
3. **时间显示**:Mac 上 `localtime_r` 返回本机时区时间;固件用 `gmtime_r`(因 ota.cc 已把本地时间写入系统时钟)。预览里可直接用 `localtime_r`,不影响布局验证。

## 实施顺序

1. 新建 `dashboard_layout.h` / `dashboard_layout.cc`,把 `SetupDashboard` / `RefreshClock` / `Show/HideDashboard` 的纯 LVGL 部分搬过去(布局代码逐字搬,不改坐标字号)。
2. 改 `custom_lcd_display.h` / `.cc`,持有 `DashboardState`、调用 layout 函数、保留 esp_timer。
3. 验证固件编译 + 烧录,dashboard 行为不变(此为安全检查点)。
4. 建 `preview/` 目录、CMakeLists、main.cc、stub。
5. 验证 Mac 预览能跑、能刷新时钟。
6. 验证改布局 → 重 make → 秒级看到效果(闭环成立)。

步骤 3 是关键安全点:重构后先确认固件没坏,再做预览侧。如果步骤 3 失败,回滚步骤 1-2。
