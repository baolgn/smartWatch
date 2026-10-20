// apply the watch's LVGL settings once per translation unit.
#ifndef WATCH_LV_CONF_H
#define WATCH_LV_CONF_H

/* platformio.ini applies this to all translation units, overriding vendor
 * lv_conf.h; only the watch task calls LVGL. */
#define LV_USE_OS LV_OS_NONE
// use tightly packed RGB565 pixels for the display transport.
#define LV_COLOR_DEPTH 16
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565
// reserve a fixed 64 KiB LVGL heap.
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (64U * 1024U)
// set the refresh interval and layout density.
#define LV_DEF_REFR_PERIOD 33
#define LV_DPI_DEF 130
// keep logging and unused built-in themes disabled.
#define LV_USE_LOG 0
#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0
// include the Montserrat sizes used by watch labels and icons.
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_40 1
// exclude example and demo code.
#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS 0
#define LV_USE_DEMO_BENCHMARK 0
#define LV_USE_DEMO_STRESS 0
#define LV_USE_DEMO_MUSIC 0

#endif
