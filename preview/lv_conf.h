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

/* The dashboard's generated CJK font has bitmap offsets beyond the compact
 * descriptor range; keep the preview ABI identical to the firmware config. */
#define LV_FONT_FMT_TXT_LARGE 1

/* 模拟器保真度核心:用 SDL 做窗口/输入,但渲染走 draw_sw(和真机一致)。
 *
 * 为什么不用 LV_USE_DRAW_SDL:它是另一套用 SDL_Renderer/Texture 的硬件加速
 * 渲染管线,和真机的 LV_USE_DRAW_SW 是不同代码。transform_scale + lv_obj_align
 * 的组合在 draw_sdl 后端有 bug(不缩放),但在真机的 draw_sw 后端正常。
 * 为了让模拟器忠实还原真机,渲染逻辑必须用同一套代码(draw_sw),只把输出接到
 * SDL 窗口。这样模拟器只在"显示目标"上和真机不同,渲染逻辑 100% 一致。
 */
#define LV_USE_SDL 1           /* SDL 窗口驱动(创建窗口、接收鼠标键盘) */
#define LV_USE_DRAW_SW 1       /* 软件 draw unit —— 和真机一致 */
#define LV_USE_DRAW_SDL 0      /* 关掉 SDL draw unit(保真度差异来源) */
#define LV_USE_OS LV_OS_NONE

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_FONT_PLACEHOLDER 1

#define LV_USE_OBJX_NAME 1

#define LV_BUILD_EXAMPLES 0

#endif
