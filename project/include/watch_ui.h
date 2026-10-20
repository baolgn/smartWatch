#pragma once

#include "watch_model.h"

// watch UI and deferred navigation API.
namespace ui {
// available watch screens.
enum class View { Face, Launcher, Clock, Weather, Calendar };

// state must outlive the UI; all calls and LVGL operations use the watch task.
void begin(const watch::WatchState& state);
/* call outside LVGL dispatch; dim/wake returns to Face, clears navigation,
 * and waits for held touches to release. */
void update(const watch::WatchState& state);
// return the displayed screen.
View currentView();
// queue a screen change while awake.
void requestView(View view);
// apply queued navigation outside LVGL event dispatch.
void processNavigation();
}  // namespace ui
