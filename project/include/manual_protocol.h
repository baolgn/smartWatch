#pragma once

#include "watch_model.h"

// watch model and manual-command protocol.
namespace watch {
// bound GATT payloads and staged-forecast lifetime.
constexpr size_t kMaxManualCommandBytes = 20;
constexpr uint32_t kManualStageIdleMs = 5UL * 60 * 1000;
constexpr uint32_t kManualStageLifetimeMs = 30UL * 60 * 1000;

// bounded ASCII reply and command outcome.
struct ManualResult {
  bool ok = false;
  char reply[21]{};
};

// stage complete forecasts without changing live data prematurely.
class ManualSession {
 public:
  /* watch task only: one printable ASCII command per GATT write, no newline.
   * invalid commands preserve live/staged data; Bluetooth cannot set time. */
  ManualResult apply(const char* command, size_t length, WatchState& state,
                     uint32_t nowMs);
  // discard staging, including when the BLE session ends.
  void reset();
  // poll each watch loop; return true when expired staging is cleared.
  bool expire(uint32_t nowMs);

 private:
  // check idle and total staging deadlines.
  bool expired(uint32_t nowMs) const;
  // track staged rows, source revision and deadlines.
  WeatherSnapshot staged_{};
  uint32_t rowMask_ = 0;
  uint32_t baseRevision_ = 0;
  uint32_t startedMs_ = 0;
  uint32_t lastRowMs_ = 0;
  bool active_ = false;
};
}  // namespace watch
