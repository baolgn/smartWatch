#pragma once

#include <stdint.h>

// hardware access belongs to the watch task; never call from BLE/USB callbacks
namespace board {
// initialize external pins, ADC and RTC without resetting time
void begin();
// atomic diagnostic label; safe outside the watch task, with no peripheral access
const char* startupStage();
// report the latest RTC transport status
bool rtcPresent();
// read validated PCF8563 UTC
bool readRtc(uint32_t& utc);
// provision and verify UTC within the supported century
bool writeRtc(uint32_t utc);

/* D0 measures the Round Display battery connector, not XIAO BAT;
   voltage cannot establish battery presence or charging */
float readBatteryVolts();

// validity, contact state and native panel coordinates
struct TouchSample {
  bool valid;
  bool pressed;
  int16_t x;
  int16_t y;
};

// native 240 x 240 coordinates at display rotation zero
TouchSample readTouch();
// watch-task diagnostic: rejected touch samples
uint32_t touchErrors();
// watch-task diagnostic: successful bus recoveries
uint32_t touchRecoveries();
}  // namespace board
