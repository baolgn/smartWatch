#include "calendar_ui.h"

#include <cstdio>

// 2026 calendar view and deferred month navigation.
namespace calendar {
// private calendar controls and rendering helpers.
namespace {
// calendar palette and fixed non-leap-year tables.
constexpr uint32_t White = 0xFFFFFF;
constexpr uint32_t Black = 0x000000;
constexpr uint32_t Red = 0xFF0000;
constexpr uint8_t MonthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
const char* const MonthNames[] = {"January", "February", "March", "April",
    "May", "June", "July", "August", "September", "October", "November", "December"};
const char* const Weekdays[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
// borrowed LVGL objects and selected/pending month state.
lv_obj_t* parent = nullptr;
lv_obj_t* title = nullptr;
lv_obj_t* previous = nullptr;
lv_obj_t* next = nullptr;
lv_obj_t* dates[31] = {};
uint8_t month = 1;
uint8_t pendingMonth = 0;
int highlightedDay = -1;

// clear default styles and disable input/scrolling.
void plain(lv_obj_t* object) {
  lv_obj_remove_style_all(object);
  lv_obj_set_scrollable(object, false);
  lv_obj_set_clickable(object, false);
}

// create a clipped, centered calendar label.
lv_obj_t* label(lv_obj_t* owner, const char* value, int x, int y,
                int width, int height, const lv_font_t* font, uint32_t color) {
  lv_obj_t* object = lv_label_create(owner);
  plain(object);
  lv_obj_set_pos(object, x, y);
  lv_obj_set_size(object, width, height);
  lv_obj_set_style_text_font(object, font, 0);
  lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
  lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(object, LV_LABEL_LONG_MODE_CLIP);
  lv_label_set_text(object, value);
  return object;
}

// calculate the month start in the fixed 2026 calendar.
uint8_t firstWeekday(uint8_t value) {
  // January 1, 2026 is Thursday; 2026 is not a leap year.
  uint16_t elapsed = 4;
  // accumulate preceding months from the Thursday year start.
  for (uint8_t i = 1; i < value; ++i) elapsed += MonthDays[i - 1];
  return static_cast<uint8_t>(elapsed % 7);
}

// queue month changes; update objects only after input dispatch.
void selectMonth(lv_event_t* event) {
  // ignore input after the calendar is reset.
  if (!parent) return;
  const uint8_t current = pendingMonth ? pendingMonth : month;
  const bool backwards = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)) == 1;
  // queue the previous month without crossing January.
  if (backwards && current > 1) pendingMonth = current - 1;
  // queue the next month without crossing December.
  if (!backwards && current < 12) pendingMonth = current + 1;
}

// create a month arrow with release activation and pressed feedback.
lv_obj_t* monthButton(int x, bool backwards) {
  lv_obj_t* object = lv_obj_create(parent);
  plain(object);
  lv_obj_set_pos(object, x, 34);
  lv_obj_set_size(object, 40, 38);
  lv_obj_set_clickable(object, true);
  lv_obj_set_press_lock(object, false);  // sliding away cancels activation
  // style normal, pressed and disabled arrow states.
  lv_obj_set_style_radius(object, 10, 0);
  lv_obj_set_style_bg_color(object, lv_color_hex(White), 0);
  lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(object, 1, 0);
  lv_obj_set_style_border_color(object, lv_color_hex(Black), 0);
  lv_obj_set_style_text_color(object, lv_color_hex(Black), 0);
  lv_obj_set_style_bg_color(object, lv_color_hex(Red), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(object, lv_color_hex(Red), LV_STATE_PRESSED);
  lv_obj_set_style_text_color(object, lv_color_hex(White), LV_STATE_PRESSED);
  lv_obj_set_style_opa(object, LV_OPA_30, LV_STATE_DISABLED);
  // route release clicks to the requested month direction.
  lv_obj_add_event_cb(object, selectMonth, LV_EVENT_CLICKED,
                     reinterpret_cast<void*>(static_cast<uintptr_t>(backwards ? 1 : 2)));
  lv_obj_t* arrow = label(object, backwards ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT,
                          0, 0, 20, 16, &lv_font_montserrat_14, Black);
  // inherit normal/pressed text colors for legible feedback.
  lv_obj_remove_local_style_prop(arrow, LV_STYLE_TEXT_COLOR, 0);
  lv_obj_center(arrow);
  return object;
}

// highlight today only within the displayed 2026 month.
void refreshToday(const watch::WatchState& state) {
  const watch::DateParts today = watch::localDateTime(state);
  const int active = state.timeValid && today.year == 2026 && today.month == month &&
                     today.day >= 1 && today.day <= MonthDays[month - 1] ? today.day : 0;
  // skip unchanged day highlighting.
  if (active == highlightedDay) return;
  highlightedDay = active;
  const uint8_t first = firstWeekday(month);
  // update date colors, preserving red Sundays outside today.
  for (uint8_t day = 1; day <= 31; ++day) {
    lv_obj_t* object = dates[day - 1];
    const bool todayCell = active == day;
    const uint32_t foreground = todayCell ? White : ((first + day - 1) % 7 == 0 ? Red : Black);
    lv_obj_set_style_text_color(object, lv_color_hex(foreground), 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(Red), 0);
    lv_obj_set_style_bg_opa(object, todayCell ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  }
}

// position active month dates and bound month navigation.
void renderMonth(const watch::WatchState& state) {
  lv_label_set_text(title, MonthNames[month - 1]);
  // disable backwards navigation at January.
  if (month == 1) lv_obj_add_state(previous, LV_STATE_DISABLED);
  // enable backwards navigation after January.
  else lv_obj_remove_state(previous, LV_STATE_DISABLED);
  // disable forwards navigation at December.
  if (month == 12) lv_obj_add_state(next, LV_STATE_DISABLED);
  // enable forwards navigation before December.
  else lv_obj_remove_state(next, LV_STATE_DISABLED);
  const uint8_t first = firstWeekday(month);
  // lay out existing dates in their weekday slots.
  for (uint8_t day = 1; day <= 31; ++day) {
    lv_obj_t* object = dates[day - 1];
    // hide unused day labels in shorter months.
    if (day > MonthDays[month - 1]) {
      lv_obj_set_hidden(object, true);
      continue;
    }
    lv_obj_set_hidden(object, false);
    const uint8_t slot = first + day - 1;
    // at most six rows, ending at y182 above the back key.
    lv_obj_set_pos(object, 25 + (slot % 7) * 28, 92 + (slot / 7) * 15);
  }
  // reapply today highlighting and finalize the layout.
  highlightedDay = -1;
  refreshToday(state);
  lv_obj_update_layout(parent);
}
}  // namespace

// create the fixed-year calendar on a caller-owned screen.
void build(lv_obj_t* screen, const watch::WatchState& state) {
  reset();
  // stop when no destination screen is supplied.
  if (!screen) return;
  // use a non-scrollable white calendar background.
  parent = screen;
  lv_obj_set_style_bg_color(parent, lv_color_hex(White), 0);
  lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
  lv_obj_set_scrollable(parent, false);
  const watch::DateParts today = watch::localDateTime(state);
  // open the current month when the clock falls within 2026.
  if (state.timeValid && today.year == 2026 && today.month >= 1 && today.month <= 12)
    month = today.month;
  // create the year, month title and navigation arrows.
  label(parent, "2026", 83, 14, 74, 17, &lv_font_montserrat_14, Red);
  title = label(parent, "", 78, 45, 84, 19, &lv_font_montserrat_14, Black);
  previous = monthButton(38, true);
  next = monthButton(162, false);
  // create weekday headings with Sunday in red.
  for (uint8_t column = 0; column < 7; ++column)
    label(parent, Weekdays[column], 25 + column * 28, 75, 22, 15,
          &lv_font_montserrat_12, column == 0 ? Red : Black);
  // allocate reusable date labels for any month.
  for (uint8_t day = 1; day <= 31; ++day) {
    char text[3];
    std::snprintf(text, sizeof(text), "%u", static_cast<unsigned>(day));
    dates[day - 1] = label(parent, text, 0, 0, 22, 16, &lv_font_montserrat_12, Black);
    lv_obj_set_style_radius(dates[day - 1], 8, 0);
  }
  renderMonth(state);
}

// refresh today without rebuilding the calendar.
void update(const watch::WatchState& state) {
  // update only while the calendar objects exist.
  if (parent) refreshToday(state);
}

// apply pending months outside input-event dispatch.
void processNavigation(const watch::WatchState& state) {
  // skip absent views and empty navigation queues.
  if (!parent || !pendingMonth) return;
  month = pendingMonth;
  pendingMonth = 0;
  renderMonth(state);
}

// release borrowed references before the owner deletes objects.
void reset() {
  parent = title = previous = next = nullptr;
  // clear every borrowed day-label pointer.
  for (lv_obj_t*& object : dates) object = nullptr;
  month = 1;
  pendingMonth = 0;
  highlightedDay = -1;
}
}  // namespace calendar
