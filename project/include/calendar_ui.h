#pragma once

#include <lvgl.h>
#include "watch_model.h"

/* fixed-year calendar; watch/UI task only. caller owns the screen/back button
 * and calls reset() before deleting objects. */
namespace calendar {
// create the 2026 calendar on the caller-owned screen.
void build(lv_obj_t* screen, const watch::WatchState& state);
// refresh the current-date highlight.
void update(const watch::WatchState& state);
// apply queued month changes after event dispatch.
void processNavigation(const watch::WatchState& state);
// clear borrowed object pointers before their deletion.
void reset();
}  // namespace calendar
