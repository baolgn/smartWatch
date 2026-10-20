#include "watch_ui.h"
#include "calendar_ui.h"

#include <lvgl.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

// watch screens and input-safe navigation.
namespace ui {
// private LVGL state, controls and render callbacks.
namespace {
// shared UI palette and icon colors.
constexpr uint32_t Background = 0xFFFFFF;
constexpr uint32_t Surface = 0xFFFFFF;
constexpr uint32_t Primary = 0x000000;
constexpr uint32_t Secondary = 0xFF0000;
constexpr uint32_t IconMuted = 0x90A2BC;
constexpr uint32_t Accent = 0xFF0000;
constexpr float Pi = 3.14159265358979323846f;
// borrowed model and current/pending screen state.
const watch::WatchState* model = nullptr;
View view = View::Face;
View pendingView = View::Face;
bool navigationPending = false;
bool displayDimmed = false;  // mode of the current LVGL object tree
// current screen and face complication objects.
lv_obj_t* screen = nullptr;
lv_obj_t* batteryFill = nullptr;
lv_obj_t* batteryBody = nullptr;
lv_obj_t* batteryLabel = nullptr;
lv_obj_t* batteryCap = nullptr;
lv_obj_t* faceWeather = nullptr;
// first-contact shortcut state and animation timing.
bool shortcutPending = false;
View shortcutView = View::Face;
bool shortcutSettled = false;
uint32_t shortcutSettledTick = 0;
lv_obj_t* shortcutVisual = nullptr;
constexpr uint32_t ShortcutAnimationMs = 100;
constexpr uint32_t ShortcutSettleMs = 40;
constexpr int32_t ShortcutPressedScale = 246;  // 96% of resting artwork
// cached face values avoid redundant style updates.
int16_t lastBatteryPercent = INT16_MIN;
const lv_font_t* lastTemperatureFont = nullptr;
// cached endpoints for a minute mark.
struct DialMark { float x1, y1, x2, y2; };
DialMark dialMarks[60];
// cached endpoint for a clock hand.
struct HandTip { float x, y; };
HandTip handTips[3];
// view-specific text, forecast viewport and tab objects.
lv_obj_t* temperatureLabel = nullptr;
lv_obj_t* statusLabel = nullptr;
lv_obj_t* dateLabel = nullptr;
lv_obj_t* digitalLabel = nullptr;
lv_obj_t* forecastList = nullptr;
lv_obj_t* hourlyTab = nullptr;
lv_obj_t* dailyTab = nullptr;
// hourly and daily forecast views.
enum class WeatherMode { Hourly, Daily };
// active/pending forecast tab and rendered-data cache.
WeatherMode weatherMode = WeatherMode::Hourly;
WeatherMode pendingWeatherMode = WeatherMode::Hourly;
bool weatherModePending = false;
watch::WeatherDisplay lastWeatherDisplay = watch::WeatherDisplay::Missing;
size_t lastVisibleDay = 0;
size_t lastVisibleHour = 0;
float lastWeatherTemperature = 0;
uint8_t lastWeatherCode = 0;
uint32_t lastDialUtc = UINT32_MAX;
uint32_t lastWeatherRevision = UINT32_MAX;
uint32_t lastWeatherHour = UINT32_MAX;
int32_t lastOffset = INT32_MAX;
uint32_t lastTransition = UINT32_MAX;
int32_t lastNextOffset = INT32_MAX;
watch::TemperatureUnit lastUnits = watch::TemperatureUnit::Celsius;
bool lastTimeValid = false;

// clear default styles and disable scrolling/input.
void plain(lv_obj_t* obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
}

// update label content only when it changes.
void text(lv_obj_t* label, const char* value) {
    // ignore absent labels and unchanged text.
    if (label && std::strcmp(lv_label_get_text(label), value) != 0) {
        lv_label_set_text(label, value);
    }
}

// create a positioned label with explicit font, color and alignment.
lv_obj_t* label(lv_obj_t* parent, const char* value, int x, int y, int width,
                const lv_font_t* font, uint32_t color = Primary,
                lv_text_align_t align = LV_TEXT_ALIGN_CENTER) {
    lv_obj_t* obj = lv_label_create(parent);
    plain(obj);
    lv_obj_set_width(obj, width);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(obj, align, 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    lv_label_set_text(obj, value);
    return obj;
}

// create a non-interactive colored rectangle.
lv_obj_t* box(lv_obj_t* parent, int x, int y, int width, int height,
              uint32_t color, int radius = 0) {
    lv_obj_t* obj = lv_obj_create(parent);
    plain(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

// queue the requested screen without rebuilding during event dispatch.
void navigate(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    // ignore navigation while dimmed or a face shortcut is committed.
    if (displayDimmed || (model && model->displayDimmed) || shortcutPending) return;
    requestView(static_cast<View>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))));
}

// apply shared navigation/tab input flags and pressed feedback.
void styleButton(lv_obj_t* obj) {
    lv_obj_set_clickable(obj, true);
    lv_obj_set_event_bubble(obj, false);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(Primary), 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(Accent), LV_STATE_PRESSED);
    lv_obj_set_style_transform_width(obj, -2, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(obj, -2, LV_STATE_PRESSED);
}

// create a navigation control with release activation and pressed feedback.
lv_obj_t* button(lv_obj_t* parent, View target, int x, int y, int width, int height,
                 lv_event_code_t activation = LV_EVENT_CLICKED) {
    lv_obj_t* obj = box(parent, x, y, width, height, Surface, 12);
    styleButton(obj);
    // bind the target screen to the selected activation event.
    lv_obj_add_event_cb(obj, navigate, activation,
                       reinterpret_cast<void*>(static_cast<uintptr_t>(target)));
    return obj;
}

// create a fixed bottom return target that accepts long-held releases.
void back(View target, const char* title, const lv_font_t* font = &lv_font_montserrat_14) {
    /* finger-sized target inside the panel; release activates even after
     * the LVGL long-press threshold, with no separate hold action. */
    lv_obj_t* obj = button(screen, target, 76, 188, 88, 44, LV_EVENT_CLICKED);
    lv_obj_set_style_radius(obj, 22, 0);
    lv_obj_t* caption = label(obj, title, 0, 0, 88, font, Accent);
    lv_obj_center(caption);
}

// format rounded temperature with the configured degree/unit suffix.
void temperatureText(char* out, size_t capacity, float celsius) {
    const float value = watch::temperatureForDisplay(celsius, model->units);
    std::snprintf(out, capacity, "%ld\xC2\xB0%c", static_cast<long>(std::lround(value)),
                  model->units == watch::TemperatureUnit::Celsius ? 'C' : 'F');
}

// draw an opaque rounded line on the supplied LVGL layer.
void drawLine(lv_layer_t* layer, float x1, float y1, float x2, float y2,
              uint32_t color, int width) {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.p1.x = x1;
    dsc.p1.y = y1;
    dsc.p2.x = x2;
    dsc.p2.y = y2;
    dsc.color = lv_color_hex(color);
    dsc.width = width;
    dsc.round_start = 1;
    dsc.round_end = 1;
    lv_draw_line(layer, &dsc);
}

// draw a filled circular icon element.
void disc(lv_layer_t* layer, int x, int y, int radius, uint32_t color) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(color);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = LV_RADIUS_CIRCLE;
    const lv_area_t area = {x - radius, y - radius, x + radius, y + radius};
    lv_draw_rect(layer, &dsc, &area);
}

// draw a forecast-row symbol from its WMO condition code.
void drawWeatherIcon(lv_event_t* event) {
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target_obj(event), &area);
    lv_layer_t* layer = lv_event_get_layer(event);
    const int x = area.x1, y = area.y1;
    // classify the condition for sun, cloud and precipitation elements.
    const uint8_t code = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    const bool clear = code <= 1;
    const bool partly = code == 2;
    const bool fog = code == 45 || code == 48;
    const bool drizzle = code >= 51 && code <= 55;
    const bool freezing = code == 56 || code == 57 || code == 66 || code == 67;
    const bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
    const bool storm = code >= 95;
    // draw the sun for clear and partly cloudy conditions.
    if (clear || partly) {
        const int cx = x + (partly ? 12 : 19), cy = y + (partly ? 11 : 17);
        // draw eight sun rays.
        for (int i = 0; i < 8; ++i) {
            const float angle = i * Pi / 4;
            drawLine(layer, cx + std::cos(angle) * 10, cy + std::sin(angle) * 10,
                     cx + std::cos(angle) * 14, cy + std::sin(angle) * 14, 0xFFC857, 2);
        }
        disc(layer, cx, cy, 6, 0xFFC857);
        // finish clear-sky artwork before cloud rendering.
        if (clear) return;
    }
    // replace cloud artwork with fog bands.
    if (fog) {
        // draw three staggered fog bands.
        for (int i = 0; i < 3; ++i)
            drawLine(layer, x + 5 + (i % 2) * 3, y + 10 + i * 8,
                     x + 32 - (i % 2) * 3, y + 10 + i * 8, IconMuted, 3);
        return;
    }
    // form a cloud from overlapping circles and a rounded baseline.
    disc(layer, x + 11, y + 16, 7, 0xB5C7DB);
    disc(layer, x + 20, y + 12, 9, 0xB5C7DB);
    disc(layer, x + 29, y + 17, 6, 0xB5C7DB);
    drawLine(layer, x + 10, y + 21, x + 29, y + 21, 0xB5C7DB, 5);
    // finish cloud-only conditions before precipitation.
    if (partly || code == 3) return;
    // draw a lightning bolt beneath the cloud.
    if (storm) {
        drawLine(layer, x + 22, y + 23, x + 16, y + 29, 0xFFC857, 3);
        drawLine(layer, x + 16, y + 29, x + 23, y + 29, 0xFFC857, 3);
        drawLine(layer, x + 23, y + 29, x + 18, y + 35, 0xFFC857, 3);
    }
    // draw ice/snow marks beneath the cloud.
    else if (snow || freezing) {
        // draw two precipitation symbols.
        for (int cx : {12, 27}) {
            drawLine(layer, x + cx - 3, y + 29, x + cx + 3, y + 29, 0x8DDCF2, 1);
            drawLine(layer, x + cx, y + 26, x + cx, y + 32, 0x8DDCF2, 1);
            // add diagonal arms to each snowflake.
            if (snow) {
                drawLine(layer, x + cx - 2, y + 27, x + cx + 2, y + 31, 0x8DDCF2, 1);
                drawLine(layer, x + cx - 2, y + 31, x + cx + 2, y + 27, 0x8DDCF2, 1);
            }
        }
        // add a blue streak for freezing precipitation.
        if (freezing) drawLine(layer, x + 21, y + 25, x + 18, y + 33, 0x66BFFF, 2);
    }
    // draw rain/drizzle streaks beneath the cloud.
    else {
        // shorten streaks for drizzle.
        for (int i = 0; i < 3; ++i)
            drawLine(layer, x + 12 + i * 9, y + 26, x + 10 + i * 9,
                     y + (drizzle ? 28 : 33), 0x66BFFF, 2);
    }
}

// attach a condition-specific draw callback to a forecast-row icon.
void weatherIcon(lv_obj_t* parent, int x, int y, uint8_t code) {
    lv_obj_t* icon = lv_obj_create(parent);
    plain(icon);
    lv_obj_set_pos(icon, x, y);
    lv_obj_set_size(icon, 38, 38);
    lv_obj_add_event_cb(icon, drawWeatherIcon, LV_EVENT_DRAW_MAIN,
                       reinterpret_cast<void*>(static_cast<uintptr_t>(code)));
}

// prepare fixed minute-mark geometry for repeated partial redraws.
void prepareDial() {
    // cache dial trig once; partial flushes redraw each strip separately.
    for (int i = 0; i < 60; ++i) {
        const float radians = i * Pi / 30.0f;
        const float sine = std::sin(radians), cosine = std::cos(radians);
        const float inner = i % 5 == 0 ? 102.0f : 111.0f;
        dialMarks[i] = {120 + sine * inner, 120 - cosine * inner,
                        120 + sine * 116, 120 - cosine * 116};
    }
}

// cache a hand endpoint from its angle and length.
void setHandTip(unsigned index, float angle, float length) {
    const float radians = angle * Pi / 180.0f;
    handTips[index] = {120 + std::sin(radians) * length, 120 - std::cos(radians) * length};
}

// apply the animation scale only to shortcut artwork.
void animateShortcutScale(void* object, int32_t scale) {
    lv_obj_t* visual = static_cast<lv_obj_t*>(object);
    lv_obj_set_style_transform_scale_x(visual, scale, 0);
    lv_obj_set_style_transform_scale_y(visual, scale, 0);
}

// record animation completion before the visible settle delay.
void shortcutAnimationComplete(lv_anim_t*) {
    // ignore completion after the shortcut was cancelled.
    if (!shortcutPending) return;
    // start the settle delay at actual completion, even after timer stalls.
    shortcutSettled = true;
    shortcutSettledTick = lv_tick_get();
    lv_obj_invalidate(shortcutVisual);
}

// commit a face shortcut on first contact and animate before navigation.
void shortcutPress(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    // ignore dimmed, duplicate or already queued navigation.
    if (displayDimmed || (model && model->displayDimmed) || shortcutPending || navigationPending) return;
    lv_obj_t* target = lv_event_get_target_obj(event);
    shortcutVisual = lv_obj_get_child(target, 0);
    shortcutView = static_cast<View>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    shortcutSettled = false;
    shortcutPending = true;
    /* commit first contact despite later release noise/movement; wait for
     * release to prevent click-through into Calendar/Weather. */
    lv_indev_t* input = lv_event_get_indev(event);
    // suppress this held contact until release.
    if (input) lv_indev_wait_release(input);
    // shrink the visual child over the configured press duration.
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, shortcutVisual);
    lv_anim_set_exec_cb(&animation, animateShortcutScale);
    lv_anim_set_values(&animation, LV_SCALE_NONE, ShortcutPressedScale);
    lv_anim_set_duration(&animation, ShortcutAnimationMs);
    // ease-out responds quickly then settles; ease-in barely moves in frame one.
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&animation, shortcutAnimationComplete);
    lv_anim_start(&animation);
}

// create a fixed hit target with separately animated artwork.
lv_obj_t* faceShortcut(View target, int x, int y, int width, int height,
                       int visualX, int visualY, int visualWidth, int visualHeight) {
    lv_obj_t* hit = lv_obj_create(screen);
    plain(hit);
    lv_obj_set_pos(hit, x, y);
    lv_obj_set_size(hit, width, height);
    lv_obj_set_clickable(hit, true);
    lv_obj_set_event_bubble(hit, false);
    // activate on first press rather than waiting for a second contact.
    lv_obj_add_event_cb(hit, shortcutPress, LV_EVENT_PRESSED,
                       reinterpret_cast<void*>(static_cast<uintptr_t>(target)));
    // scale only the non-clickable child; keep the larger hit target fixed.
    lv_obj_t* visual = lv_obj_create(hit);
    plain(visual);
    lv_obj_set_pos(visual, visualX, visualY);
    lv_obj_set_size(visual, visualWidth, visualHeight);
    lv_obj_set_style_transform_pivot_x(visual, visualWidth / 2, 0);
    lv_obj_set_style_transform_pivot_y(visual, visualHeight / 2, 0);
    return visual;
}

// draw compact face weather around the temperature label.
void drawFaceWeather(lv_event_t* event) {
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target_obj(event), &area);
    lv_layer_t* layer = lv_event_get_layer(event);
    const int x = area.x1, y = area.y1;
    // scale vector coordinates without a permanent transform layer.
    constexpr float scale = 0.66f;
    // scale line coordinates and thickness to the compact icon.
    const auto line = [layer, x, y](float ax, float ay, float bx, float by, uint32_t color, int width) {
        drawLine(layer, x + ax * scale, y + ay * scale, x + bx * scale, y + by * scale,
                 color, width > 1 ? static_cast<int>(std::lround(width * scale)) : 1);
    };
    // scale circle coordinates and radius to the compact icon.
    const auto circle = [layer, x, y](int cx, int cy, int radius, uint32_t color) {
        disc(layer, x + static_cast<int>(std::lround(cx * scale)),
             y + static_cast<int>(std::lround(cy * scale)),
             static_cast<int>(std::lround(radius * scale)), color);
    };
    // resolve displayable weather before selecting icon elements.
    float temperature = 0;
    uint8_t code = 0;
    const bool missing = !model || watch::weatherForDisplay(*model, temperature, code) == watch::WeatherDisplay::Missing;
    const bool clear = !missing && code <= 1;
    const bool partly = !missing && code == 2;
    const bool fog = !missing && (code == 45 || code == 48);
    // draw a sun behind clear or partly cloudy conditions.
    if (clear || partly) {
        const int cx = (partly ? 23 : 44), cy = (partly ? 19 : 30);
        const int radius = partly ? 12 : 21;
        // draw eight scaled sun rays.
        for (int i = 0; i < 8; ++i) {
            const float angle = i * Pi / 4;
            line(cx + std::cos(angle) * (radius + 3), cy + std::sin(angle) * (radius + 3),
                 cx + std::cos(angle) * (radius + 7), cy + std::sin(angle) * (radius + 7), 0xFFC857, 2);
        }
        circle(cx, cy, radius, 0xFFC857);
        // finish clear-sky artwork without clouds.
        if (clear) return;
    }
    // use fog bands around the temperature text.
    if (fog) {
        // leave room for temperature within the fog symbol.
        for (int row : {10, 16, 44, 50})
            line(14, row, 74, row, IconMuted, 3);
        return;
    }
    // draw a muted cloud when weather is unavailable.
    const uint32_t cloud = missing ? IconMuted : 0xB5C7DB;
    circle(27, 29, 16, cloud);
    circle(46, 23, 21, cloud);
    circle(65, 30, 14, cloud);
    line(26, 39, 65, 39, cloud, 12);
    // finish unavailable/cloud-only symbols before precipitation.
    if (missing || partly || code == 3) return;
    // draw storm lightning.
    if (code >= 95) {
        line(48, 46, 39, 53, 0xFFC857, 3);
        line(39, 53, 49, 53, 0xFFC857, 3);
        line(49, 53, 41, 61, 0xFFC857, 3);
    }
    // choose snow, freezing precipitation or rain marks.
    else {
        const bool freezing = code == 56 || code == 57 || code == 66 || code == 67;
        const bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
        // draw two precipitation marks beneath the cloud.
        for (int cx : {27, 60}) {
            // draw ice/snow crosses.
            if (snow || freezing) {
                line(cx - 4, 54, cx + 4, 54, 0x8DDCF2, 2);
                line(cx, 50, cx, 58, 0x8DDCF2, 2);
                // add snowflake diagonals.
                if (snow) {
                    line(cx - 3, 51, cx + 3, 57, 0x8DDCF2, 1);
                    line(cx - 3, 57, cx + 3, 51, 0x8DDCF2, 1);
                }
            }
            // draw rain streaks, shortened for drizzle.
            else line(cx + 2, 49, cx - 2,
                            (code >= 51 && code <= 55 ? 54 : 59), 0x66BFFF, 2);
        }
        // add a blue streak for freezing precipitation.
        if (freezing) line(45, 49, 41, 59, 0x66BFFF, 2);
    }
}

// render minute marks in the active face palette.
void drawDial(lv_event_t* event) {
    lv_layer_t* layer = lv_event_get_layer(event);
    // draw sixty cached marks, thickening each hour position.
    for (unsigned i = 0; i < 60; ++i) {
        const auto& mark = dialMarks[i];
        drawLine(layer, mark.x1, mark.y1, mark.x2, mark.y2,
                 displayDimmed ? 0xFFFFFF : Primary, i % 5 == 0 ? 2 : 1);
    }
}

// render valid clock time, including RTC fault interpolation, and the pivot.
void drawHands(lv_event_t* event) {
    lv_layer_t* layer = lv_event_get_layer(event);
    const uint32_t color = displayDimmed ? 0xFFFFFF : Primary;
    // draw hour/minute hands in the face palette and seconds in red.
    if (model && model->timeValid) {
        drawLine(layer, 120, 120, handTips[0].x, handTips[0].y, color, 5);
        drawLine(layer, 120, 120, handTips[1].x, handTips[1].y, color, 3);
        drawLine(layer, 120, 120, handTips[2].x, handTips[2].y, Accent, 1);
    }
    // draw the pivot even when clock time is unavailable.
    disc(layer, 120, 120, 4, color);
    disc(layer, 120, 120, 2, Accent);
}

// create compact battery, weather and date elements.
void buildFaceComplications() {
    // place the battery level fill and percentage above the center.
    batteryBody = box(screen, 104, 51, 32, 18, Background, 4);
    lv_obj_set_style_border_width(batteryBody, 1, 0);
    lv_obj_set_style_border_color(batteryBody, lv_color_hex(IconMuted), 0);
    batteryCap = box(screen, 136, 57, 3, 6, IconMuted, 1);
    batteryFill = box(batteryBody, 2, 2, 26, 12, Background, 2);
    batteryLabel = label(batteryBody, "--%", 0, 0, 28, &lv_font_montserrat_10);
    lv_obj_center(batteryLabel);

    // place weather inside a larger invisible shortcut target.
    faceWeather = faceShortcut(View::Weather, 11, 90, 88, 58, 14, 6, 60, 44);
    lv_obj_add_event_cb(faceWeather, drawFaceWeather, LV_EVENT_DRAW_MAIN, nullptr);
    temperatureLabel = label(faceWeather, "--", 0, 13, 60, &lv_font_montserrat_12);
    // give the date its own fixed calendar hit target.
    lv_obj_t* date = faceShortcut(View::Calendar, 64, 149, 112, 58, 14, 5, 84, 46);
    /* show only the two-line date; retain the larger invisible hit target
     * and press animation. */
    dateLabel = label(date, "SET\nRTC", 0, 0, 78, &lv_font_montserrat_14);
    lv_obj_set_style_text_line_space(dateLabel, 1, 0);
    lv_obj_center(dateLabel);
}

// construct the analog dial and optional awake-only controls.
void buildFace() {
    // enable face navigation and complications only while awake.
    if (!displayDimmed) {
        lv_obj_set_clickable(screen, true);
        lv_obj_add_event_cb(screen, navigate, LV_EVENT_CLICKED,
                           reinterpret_cast<void*>(static_cast<uintptr_t>(View::Launcher)));
        buildFaceComplications();
    }
    lv_obj_add_event_cb(screen, drawDial, LV_EVENT_DRAW_MAIN, nullptr);

    // draw hands above complications without intercepting their taps.
    lv_obj_t* hands = lv_obj_create(screen);
    plain(hands);
    lv_obj_set_size(hands, 240, 240);
    lv_obj_set_pos(hands, 0, 0);
    lv_obj_add_event_cb(hands, drawHands, LV_EVENT_DRAW_MAIN, nullptr);
}

// create the scrollable app list with a fixed return arrow.
void buildLauncher() {
    label(screen, "APPS", 60, 25, 120, &lv_font_montserrat_14, Secondary);
    // configure the vertical app viewport below the heading.
    lv_obj_t* list = box(screen, 35, 54, 170, 124, Background);
    lv_obj_set_clickable(list, true);
    lv_obj_set_scrollable(list, true);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(list, lv_color_hex(Secondary), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_bottom(list, 4, 0);
    // map launcher labels to screen destinations.
    struct App { const char* name; View target; };
    // append apps here; preserve the viewport and fixed return key.
    static const App apps[] = {{"Clock", View::Clock}, {"Weather", View::Weather},
                               {"Calendar", View::Calendar}};
    int y = 0;
    // append one compact navigation row per app.
    for (const auto& app : apps) {
        lv_obj_t* item = button(list, app.target, 3, y, 162, 42);
        lv_obj_t* caption = label(item, app.name, 0, 0, 156, &lv_font_montserrat_14);
        lv_obj_center(caption);
        y += 50;
    }
    back(View::Face, LV_SYMBOL_LEFT, &lv_font_montserrat_24);
}

// delegate the calendar view and add its launcher return arrow.
void buildCalendar() {
    calendar::build(screen, *model);
    back(View::Launcher, LV_SYMBOL_LEFT, &lv_font_montserrat_24);
}

// create the time-only screen with RTC status and a return arrow.
void buildClock() {
    label(screen, "CLOCK", 60, 30, 120, &lv_font_montserrat_14, Secondary);
    digitalLabel = label(screen, "--:--:--", 13, 94, 214, &lv_font_montserrat_40);
    statusLabel = label(screen, "Set RTC over USB / upload", 30, 148, 180,
                         &lv_font_montserrat_12, Secondary);
    back(View::Launcher, LV_SYMBOL_LEFT, &lv_font_montserrat_24);
}

// queue the selected forecast tab for processing after event dispatch.
void selectWeatherMode(lv_event_t* event) {
    // ignore forecast tab changes while dimmed.
    if (displayDimmed || (model && model->displayDimmed)) return;
    pendingWeatherMode = static_cast<WeatherMode>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    weatherModePending = true;
}

// create a release-activated forecast tab with pressed feedback.
lv_obj_t* weatherTab(WeatherMode target, const char* title, int x) {
    lv_obj_t* obj = box(screen, x, 43, 80, 34, Surface, 12);
    styleButton(obj);
    // associate each tab with its deferred forecast mode.
    lv_obj_add_event_cb(obj, selectWeatherMode, LV_EVENT_CLICKED,
                       reinterpret_cast<void*>(static_cast<uintptr_t>(target)));
    lv_obj_t* caption = label(obj, title, 0, 0, 80, &lv_font_montserrat_14);
    lv_obj_center(caption);
    return obj;
}

// highlight the selected forecast tab without changing its background.
void styleWeatherTabs() {
    lv_obj_set_style_border_color(hourlyTab, lv_color_hex(weatherMode == WeatherMode::Hourly ? Accent : Primary), 0);
    lv_obj_set_style_border_color(dailyTab, lv_color_hex(weatherMode == WeatherMode::Daily ? Accent : Primary), 0);
    lv_obj_set_style_bg_color(hourlyTab, lv_color_hex(Surface), 0);
    lv_obj_set_style_bg_color(dailyTab, lv_color_hex(Surface), 0);
}

// create forecast tabs, scrollable content and a fixed return arrow.
void buildWeather() {
    label(screen, "WEATHER", 52, 21, 136, &lv_font_montserrat_14, Secondary);
    weatherMode = WeatherMode::Hourly;
    hourlyTab = weatherTab(WeatherMode::Hourly, "24h", 36);
    dailyTab = weatherTab(WeatherMode::Daily, "7 days", 124);
    styleWeatherTabs();
    // keep the return button outside the forecast scroll viewport.
    forecastList = box(screen, 25, 83, 190, 101, Background);
    lv_obj_set_clickable(forecastList, true);
    lv_obj_set_scrollable(forecastList, true);
    lv_obj_set_scroll_dir(forecastList, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(forecastList, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_bottom(forecastList, 8, 0);
    lv_obj_set_style_bg_color(forecastList, lv_color_hex(Secondary), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(forecastList, 2, LV_PART_SCROLLBAR);
    back(View::Launcher, LV_SYMBOL_LEFT, &lv_font_montserrat_24);
}

// replace view objects and invalidate cached rendering state.
void rebuild() {
    /* outside event dispatch, free the old tree before building another
     * to avoid holding two forecast lists in memory. */
    calendar::reset();
    // delete shortcut animation before its visual object is removed.
    if (shortcutVisual) lv_anim_delete(shortcutVisual, animateShortcutScale);
    shortcutPending = false;
    shortcutSettled = false;
    shortcutVisual = nullptr;
    lastBatteryPercent = INT16_MIN;
    lastTemperatureFont = nullptr;
    // clear old children/callbacks and select the awake or dim background.
    if (screen) {
        lv_obj_clean(screen);
        lv_obj_remove_event_cb(screen, navigate);
        lv_obj_remove_event_cb(screen, drawDial);
        lv_obj_set_clickable(screen, false);
        lv_obj_set_style_bg_color(screen, lv_color_hex(displayDimmed ? 0x000000 : Background), 0);
    }
    // clear references and caches before constructing the destination view.
    batteryFill = batteryBody = batteryLabel = batteryCap = faceWeather = nullptr;
    temperatureLabel = statusLabel = dateLabel = digitalLabel = forecastList = nullptr;
    hourlyTab = dailyTab = nullptr;
    weatherModePending = false;
    lastDialUtc = lastWeatherRevision = lastWeatherHour = UINT32_MAX;
    lastOffset = INT32_MAX;
    lastTransition = UINT32_MAX;
    lastNextOffset = INT32_MAX;
    lastTimeValid = false;
    // build exactly the selected screen.
    switch (view) {
        // create the analog watch face.
        case View::Face: buildFace(); break;
        // create the scrollable app launcher.
        case View::Launcher: buildLauncher(); break;
        // create the time-only app.
        case View::Clock: buildClock(); break;
        // create hourly/daily forecast controls.
        case View::Weather: buildWeather(); break;
        // create the 2026 calendar.
        case View::Calendar: buildCalendar(); break;
    }
}

// refresh clock geometry and visible complication values.
void refreshFace() {
    char buffer[48];
    const uint32_t dialTime = model->utc;
    // redraw the dial only when time, trust or timezone changes.
    if (lastDialUtc != dialTime || lastTimeValid != model->timeValid ||
        lastOffset != watch::offsetAt(*model, model->utc)) {
        // update hand endpoints only from valid local time.
        if (model->timeValid) {
            const auto local = watch::localDateTime(*model);
            const float second = local.second;
            setHandTip(0, (local.hour % 12) * 30.0f + local.minute * 0.5f + second / 120.0f, 68);
            setHandTip(1, local.minute * 6.0f + second * 0.1f, 98);
            setHandTip(2, second * 6.0f, 107);
            // reuse the clock conversion for the awake face's two-line date.
            if (!displayDimmed) {
                std::snprintf(buffer, sizeof(buffer), "%s\n%s %u", watch::weekdayName(local.weekday),
                              watch::monthName(local.month), static_cast<unsigned>(local.day));
                text(dateLabel, buffer);
            }
        }
        // request RTC provisioning without accessing hidden complications.
        else if (!displayDimmed) text(dateLabel, "SET\nRTC");
        lv_obj_invalidate(screen);
        lastDialUtc = dialTime;
        lastTimeValid = model->timeValid;
        lastOffset = watch::offsetAt(*model, model->utc);
    }
    // stop after clock rendering when complications are hidden.
    if (displayDimmed) return;
    // update battery text, palette and fill only when charge changes.
    if (lastBatteryPercent != model->batteryPercent) {
        lastBatteryPercent = model->batteryPercent;
        const bool known = model->batteryPercent >= 0 && model->batteryPercent <= 100;
        const uint32_t batteryRgb = known ? watch::batteryColor(model->batteryPercent) : IconMuted;
        // show a percentage for a valid charge estimate.
        if (known) std::snprintf(buffer, sizeof(buffer), "%d%%", model->batteryPercent);
        // show an unknown marker without inventing charge.
        else std::snprintf(buffer, sizeof(buffer), "--%%");
        text(batteryLabel, buffer);
        lv_obj_set_style_border_color(batteryBody, lv_color_hex(batteryRgb), 0);
        lv_obj_set_style_bg_color(batteryCap, lv_color_hex(batteryRgb), 0);
        lv_obj_set_style_bg_color(batteryFill, lv_color_hex(batteryRgb), 0);
        // hide the fill for unknown or empty charge.
        if (!known || model->batteryPercent == 0) lv_obj_set_hidden(batteryFill, true);
        // size the fill proportionally, rounding nonzero levels upward.
        else {
            lv_obj_set_hidden(batteryFill, false);
            lv_obj_set_width(batteryFill, (26 * model->batteryPercent + 99) / 100);
        }
    }

    // select the observed, predicted or dated temperature and icon.
    float tempC = 0;
    uint8_t code = 0;
    const auto display = watch::weatherForDisplay(*model, tempC, code);
    const lv_font_t* font = &lv_font_montserrat_12;
    // format available temperatures with their source marker.
    if (display != watch::WeatherDisplay::Missing) {
        const long value = static_cast<long>(std::lround(watch::temperatureForDisplay(tempC, model->units)));
        // mark predictions/upload snapshots with a tilde.
        std::snprintf(buffer, sizeof(buffer), "%s%ld\xC2\xB0",
                      display == watch::WeatherDisplay::Observed ? "" : "~", value);
        lv_point_t size;
        lv_text_get_size(&size, buffer, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        // shrink wide values to leave text padding.
        if (size.x > 24) font = &lv_font_montserrat_10;
        // remeasure the smaller font before removing optional units.
        if (font == &lv_font_montserrat_10) {
            lv_text_get_size(&size, buffer, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            /* fit the sun by dropping only the degree sign; retain the number/marker.
             * Weather still shows explicit units. */
            if (size.x > 28) buffer[std::strlen(buffer) - 2] = '\0';  // two-byte UTF-8 degree sign
        }
    }
    // show unavailable temperature without a fabricated value.
    else std::snprintf(buffer, sizeof(buffer), "--");
    // apply font changes and keep temperature vertically centered.
    if (font != lastTemperatureFont) {
        lastTemperatureFont = font;
        lv_obj_set_style_text_font(temperatureLabel, font, 0);
        lv_obj_set_y(temperatureLabel, 20 - font->line_height / 2);
    }
    text(temperatureLabel, buffer);
    // redraw the icon when its condition, source or temperature changes.
    if (display != lastWeatherDisplay || code != lastWeatherCode || tempC != lastWeatherTemperature) {
        lv_obj_invalidate(faceWeather);
        lastWeatherDisplay = display;
        lastWeatherCode = code;
        lastWeatherTemperature = tempC;
    }
}

// update digital time and RTC trust status.
void refreshClock() {
    char buffer[48];
    // format local time when the clock is available.
    if (model->timeValid) {
        const watch::DateParts local = watch::localDateTime(*model);
        std::snprintf(buffer, sizeof(buffer), "%02u:%02u:%02u", static_cast<unsigned>(local.hour),
                      static_cast<unsigned>(local.minute), static_cast<unsigned>(local.second));
        text(digitalLabel, buffer);
        // identify interpolation during a temporary RTC fault.
        if (model->rtcFault) text(statusLabel, "RTC unavailable - estimated time");
        // show UTC only when no local offset is known.
        else text(statusLabel, model->offsetKnown ? "" : "UTC");
    }
    // show missing time and USB provisioning guidance.
    else {
        text(digitalLabel, "--:--:--");
        text(statusLabel, "Set RTC over USB / upload");
    }
}

// rebuild changed forecast content while retaining scroll position.
void refreshWeather() {
    // resolve timezone and bounded forecast counts.
    const auto& weather = model->weather;
    const uint32_t hour = model->timeValid ? model->utc / 3600 : 0;
    const int32_t offset = watch::offsetAt(*model, model->utc);
    const size_t dailyCount = weather.dailyCount < watch::kForecastDays ? weather.dailyCount : watch::kForecastDays;
    const size_t hourlyCount = weather.hourlyCount < watch::kForecastHours ? weather.hourlyCount : watch::kForecastHours;
    size_t firstDay = 0;
    // find the first local day that has not fully elapsed.
    while (firstDay < dailyCount && model->timeValid && weather.daily[firstDay].nextUtc <= model->utc) ++firstDay;
    size_t firstHour = 0;
    // find the first hourly interval that has not fully elapsed.
    while (firstHour < hourlyCount && model->timeValid &&
           static_cast<uint64_t>(weather.hourly[firstHour].utc) + 3600 <= model->utc) ++firstHour;
    /* rebuild only for changed data/settings or visible intervals; preserve
     * scroll position through ordinary clock updates. */
    const bool rebuildList = lastWeatherRevision != model->weatherRevision || lastUnits != model->units ||
                             lastWeatherHour != hour || lastOffset != offset ||
                             lastTransition != model->nextTransitionUtc || lastNextOffset != model->nextOffsetSeconds ||
                             lastVisibleDay != firstDay || lastVisibleHour != firstHour || lastTimeValid != model->timeValid;
    // keep existing labels and scroll state when nothing changed.
    if (!rebuildList) return;
    // save scroll position, clear old rows and refresh the rendering cache.
    const int32_t oldScroll = lv_obj_get_scroll_y(forecastList);
    lv_obj_clean(forecastList);
    statusLabel = nullptr;
    lastWeatherRevision = model->weatherRevision;
    lastWeatherHour = hour;
    lastUnits = model->units;
    lastOffset = offset;
    lastTransition = model->nextTransitionUtc;
    lastNextOffset = model->nextOffsetSeconds;
    lastVisibleDay = firstDay;
    lastVisibleHour = firstHour;
    lastTimeValid = model->timeValid;
    // use fetch metadata independently of RTC validity or observation age.
    char buffer[64];
    const uint32_t fetchedAt = weather.fetchedAt;
    // show the actual fetch time in local 24-hour format, including seconds.
    if (weather.valid && watch::validUtc(fetchedAt)) {
        const watch::DateParts fetched = watch::dateParts(fetchedAt, watch::offsetAt(*model, fetchedAt));
        std::snprintf(buffer, sizeof(buffer), "Updated %02u/%02u/%02u %02u:%02u:%02u",
                      static_cast<unsigned>(fetched.month), static_cast<unsigned>(fetched.day),
                      static_cast<unsigned>(fetched.year % 100), static_cast<unsigned>(fetched.hour),
                      static_cast<unsigned>(fetched.minute), static_cast<unsigned>(fetched.second));
    }
    // never substitute observation time or the running clock for a missing fetch.
    else std::snprintf(buffer, sizeof(buffer), "Updated --/--/-- --:--:--");
    // reserve one timestamp line above either forecast tab's rows.
    label(forecastList, buffer, 0, 0, 186, &lv_font_montserrat_12, Secondary);
    int y = 24;
    // retain refresh guidance when no forecast has been received.
    if (!weather.valid) {
        label(forecastList, "No forecast available", 0, y, 186, &lv_font_montserrat_14);
        label(forecastList, "Connect relay to refresh", 0, y + 28, 186, &lv_font_montserrat_12, Accent);
        return;
    }

    // render only the stored daily summaries.
    if (weatherMode == WeatherMode::Daily) {
        // show each remaining day with its icon and minimum/maximum.
        for (size_t i = firstDay; i < dailyCount; ++i) {
            const auto& day = weather.daily[i];
            // use the day's end after a DST change whose earlier offset is no longer stored.
            const uint32_t labelUtc = day.nextUtc - 1;
            const auto local = watch::dateParts(labelUtc, watch::offsetAt(*model, labelUtc));
            // place the local date beside the day's representative icon.
            lv_obj_t* row = box(forecastList, 0, y, 186, 49, Surface, 8);
            std::snprintf(buffer, sizeof(buffer), "%s\n%02u/%02u", watch::weekdayName(local.weekday),
                          static_cast<unsigned>(local.month), static_cast<unsigned>(local.day));
            label(row, buffer, 8, 9, 52, &lv_font_montserrat_12, Primary, LV_TEXT_ALIGN_LEFT);
            weatherIcon(row, 64, 5, day.code);
            // show both extrema with explicit units.
            char temperature[24];
            temperatureText(temperature, sizeof(temperature), day.minTempC);
            std::snprintf(buffer, sizeof(buffer), "min %s", temperature);
            label(row, buffer, 102, 8, 77, &lv_font_montserrat_12, Primary, LV_TEXT_ALIGN_RIGHT);
            temperatureText(temperature, sizeof(temperature), day.maxTempC);
            std::snprintf(buffer, sizeof(buffer), "max %s", temperature);
            label(row, buffer, 102, 28, 77, &lv_font_montserrat_12, Secondary, LV_TEXT_ALIGN_RIGHT);
            y += 55;
        }
        // explain absent, expired or fully listed daily coverage.
        const char* notice = !dailyCount ? "No daily forecast received\nConnect relay to refresh" :
                             firstDay == dailyCount ? "Daily forecast has expired\nConnect relay to refresh" :
                             "End of stored days\nConnect relay to refresh";
        label(forecastList, notice, 0, y + 4, 186, &lv_font_montserrat_12, Secondary);
        // restore scrolling after the daily rows have their final bounds.
        lv_obj_update_layout(forecastList);
        lv_obj_scroll_to_y(forecastList, oldScroll, LV_ANIM_OFF);
        return;
    }
    // start the stored hourly forecast section.
    label(forecastList, "HOURLY FORECAST", 0, y, 186, &lv_font_montserrat_12, Accent);
    y += 25;
    unsigned count = 0;
    // render hourly records whose intervals remain in range.
    for (size_t i = 0; i < hourlyCount; ++i) {
        const auto& forecast = weather.hourly[i];
        // omit fully elapsed hours; keep the current interval.
        if (model->timeValid && static_cast<uint64_t>(forecast.utc) + 3600 <= model->utc) continue;
        // render local time, conditions and temperature for this hour.
        const watch::DateParts local = watch::dateParts(forecast.utc, watch::offsetAt(*model, forecast.utc));
        lv_obj_t* row = box(forecastList, 0, y, 186, 49, Surface, 8);
        std::snprintf(buffer, sizeof(buffer), "%02u:%02u  %02u/%02u", static_cast<unsigned>(local.hour),
                      static_cast<unsigned>(local.minute), static_cast<unsigned>(local.month), static_cast<unsigned>(local.day));
        label(row, buffer, 8, 6, 113, &lv_font_montserrat_12, Primary, LV_TEXT_ALIGN_LEFT);
        label(row, watch::weatherDescription(forecast.code), 8, 28, 166, &lv_font_montserrat_12, Secondary, LV_TEXT_ALIGN_LEFT);
        temperatureText(buffer, sizeof(buffer), forecast.tempC);
        label(row, buffer, 115, 5, 64, &lv_font_montserrat_18, Primary, LV_TEXT_ALIGN_RIGHT);
        y += 55;
        ++count;
    }
    // explain absent, expired or fully listed hourly coverage.
    const char* forecastStatus = hourlyCount == 0 ?
        "No forecast received\nConnect relay to refresh" :
        count ? "End of stored hours\nConnect relay to refresh" :
                "Forecast has expired\nConnect relay to refresh";
    label(forecastList, forecastStatus, 0, y + 4, 186, &lv_font_montserrat_12, Secondary);
    // restore scrolling after the hourly rows have their final bounds.
    lv_obj_update_layout(forecastList);
    lv_obj_scroll_to_y(forecastList, oldScroll, LV_ANIM_OFF);
}
}  // namespace

// bind the long-lived model and build the initial analog face.
void begin(const watch::WatchState& state) {
    model = &state;
    displayDimmed = state.displayDimmed;
    // initialize the active LVGL screen and cached dial geometry.
    screen = lv_screen_active();
    plain(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(Background), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    prepareDial();
    // create the face and finalize layout before accepting input.
    view = View::Face;
    navigationPending = false;
    rebuild();
    update(state);
    lv_obj_update_layout(screen);
}

// apply orientation changes and refresh the current view.
void update(const watch::WatchState& state) {
    model = &state;
    // return to the face on either dim/wake transition.
    if (displayDimmed != state.displayDimmed) {
        displayDimmed = state.displayDimmed;
        /* orientation overrides accepted taps; wait for pointer release
         * to prevent app entry from a held touch after waking. */
        for (lv_indev_t* input = lv_indev_get_next(nullptr); input; input = lv_indev_get_next(input)) {
            // require held pointer contacts to release before accepting taps.
            if (lv_indev_get_type(input) == LV_INDEV_TYPE_POINTER) lv_indev_wait_release(input);
        }
        // discard queued navigation and rebuild the face in the new mode.
        navigationPending = false;
        pendingView = view = View::Face;
        rebuild();
        lv_obj_update_layout(screen);
    }
    // refresh only the currently displayed app.
    switch (view) {
        // refresh dial hands and visible complications.
        case View::Face: refreshFace(); break;
        // refresh digital time and clock status.
        case View::Clock: refreshClock(); break;
        // refresh changed forecast content.
        case View::Weather: refreshWeather(); break;
        // keep the static launcher unchanged.
        case View::Launcher: break;
        // refresh the calendar date highlight.
        case View::Calendar: calendar::update(state); break;
    }
}

// report the active screen.
View currentView() { return view; }
// queue an awake-only view change and cancel pending shortcut animation.
void requestView(View target) {
    // reject screen changes while dimmed.
    if (displayDimmed || (model && model->displayDimmed)) return;
    // remove the shortcut animation before clearing its reference.
    if (shortcutVisual) lv_anim_delete(shortcutVisual, animateShortcutScale);
    shortcutVisual = nullptr;
    shortcutPending = false;
    pendingView = target;
    navigationPending = true;
}
// apply queued screen and tab changes outside input-event dispatch.
void processNavigation() {
    // apply pending orientation changes before navigation.
    if (model && displayDimmed != model->displayDimmed) update(*model);
    // ignore navigation while the dim clock face is active.
    if (displayDimmed) return;
    // wait the full settle delay after actual shortcut animation completion.
    if (shortcutPending && shortcutSettled && lv_tick_elaps(shortcutSettledTick) >= ShortcutSettleMs)
        requestView(shortcutView);
    // replace the screen tree for an accepted destination.
    if (navigationPending) {
        navigationPending = false;
        view = pendingView;
        rebuild();
        update(*model);
        // finalize hit-test bounds; input may run before the next refresh.
        lv_obj_update_layout(screen);
    }
    // process calendar month changes after input dispatch.
    else if (view == View::Calendar) {
        calendar::processNavigation(*model);
    }
    // process queued forecast tab changes.
    else if (weatherModePending && view == View::Weather) {
        weatherModePending = false;
        // reset scroll and refresh content only for a different forecast tab.
        if (weatherMode != pendingWeatherMode) {
            weatherMode = pendingWeatherMode;
            lastWeatherRevision = UINT32_MAX;
            lv_obj_scroll_to_y(forecastList, 0, LV_ANIM_OFF);
            styleWeatherTabs();
            update(*model);
            lv_obj_update_layout(screen);
        }
    }
}
}  // namespace ui
