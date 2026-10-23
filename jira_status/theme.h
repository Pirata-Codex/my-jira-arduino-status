#pragma once
#include <lvgl.h>

// F.R.I.D.A.Y. HUD palette: near-black blue, cyan lines, lime for progress, amber/red for attention.
#define C_BG      lv_color_hex(0x060D16)
#define C_LINE    lv_color_hex(0x1C6A80)   // panel outlines
#define C_DIM     lv_color_hex(0x0E2230)   // tracks, empty bars
#define C_CYAN    lv_color_hex(0x22D3EE)
#define C_ICE     lv_color_hex(0x8FD8E8)
#define C_TEXT    lv_color_hex(0xECEEF4)
#define C_MUTED   lv_color_hex(0x86AABA)   // brighter than before: the mono font is thin
#define C_LIME    lv_color_hex(0xA3E635)
#define C_VIOLET  lv_color_hex(0xA78BFA)
#define C_AMBER   lv_color_hex(0xF5A623)
#define C_RED     lv_color_hex(0xFF4D5E)

// Vazirmatn (Latin + Persian) with Montserrat as fallback for the LV_SYMBOL_* icons.
// Regenerate with extras/make_fonts.sh.
LV_FONT_DECLARE(vazir_14);
LV_FONT_DECLARE(vazir_16);
LV_FONT_DECLARE(vazir_20);
// Share Tech Mono (HUD lettering). hud_14/16/22 fall back to Vazirmatn for Persian, then Montserrat
// for icons; hud_48 holds only digits and ": % + - . /" for the clock and big numbers.
LV_FONT_DECLARE(hud_14);
LV_FONT_DECLARE(hud_16);
LV_FONT_DECLARE(hud_22);
LV_FONT_DECLARE(hud_48);

#define F_SMALL   (&vazir_14)
#define F_BODY    (&vazir_16)
#define F_TITLE   (&vazir_20)
#define F_HUD     (&hud_14)
#define F_HUD_M   (&hud_16)
#define F_HUD_L   (&hud_22)
#define F_CLOCK   (&hud_48)
