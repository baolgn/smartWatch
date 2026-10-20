#pragma once

#include "watch_model.h"

// LightBlue GATT service; hardware/state access stays in the watch task.
namespace manualBle {
// call after Bluefruit.begin(), before advertising.
bool begin();
// consume queued commands in the watch task.
void poll(watch::WatchState& state, uint32_t nowMs);
// report GATT initialization.
bool ready();
// count successful commands.
uint32_t appliedCount();
// watch task only; USB must copy into its snapshot.
const char* lastResult();
}  // namespace manualBle
