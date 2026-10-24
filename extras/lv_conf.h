/* LVGL 9.2 config for the Jira status board.
 * Copy this file next to the lvgl library folder:  Documents/Arduino/libraries/lv_conf.h
 * Anything not set here uses LVGL's defaults. */
#if 1
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
/* LVGL's memory comes from its own region reserved at boot (jira_status/lv_mem_pool.cpp), so the
 * screen can't fragment the heap the HTTPS connections need. A static pool overflows DRAM at link time. */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_DEF_REFR_PERIOD 20
/* Small layer buffer: leaves heap for the HTTPS (TLS) handshake. */
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE (4 * 1024)

/* Display driver: LVGL's built-in TFT_eSPI glue (pins/panel come from TFT_eSPI's User_Setup.h). */
#define LV_USE_TFT_ESPI 1

/* Widgets used by the UI */
#define LV_USE_LABEL 1
#define LV_USE_ARC 1
#define LV_USE_BAR 1
#define LV_USE_CHART 1
#define LV_USE_FLEX 1

/* Fonts used by the UI */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_16

/* Persian/Arabic shaping + right-to-left text, for Persian card titles once a Persian font is added. */
#define LV_USE_BIDI 1
#define LV_USE_ARABIC_PERSIAN_CHARS 1
#define LV_BIDI_BASE_DIR_DEF LV_BASE_DIR_AUTO

/* Warnings/errors to the serial monitor (callback set in jira_status.ino). */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 0

/* A failed allocation used to freeze the screen in while(1); restart instead (the log says why). */
#define LV_ASSERT_HANDLER_INCLUDE "esp_system.h"
#define LV_ASSERT_HANDLER esp_restart();

#endif
#endif
