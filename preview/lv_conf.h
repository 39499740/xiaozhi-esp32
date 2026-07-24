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
