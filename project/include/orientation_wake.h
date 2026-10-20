#pragma once

#include <stdint.h>

// orientation-driven display policy
namespace watch {

/* signed screen-normal acceleration only; screen-up is about +1000 mg;
   adjustable UI defaults, not universal optimal angles */
constexpr int16_t kOrientationWakeMilliG = 819;  // cos(35 degrees) * 1000
constexpr int16_t kOrientationDimMilliG = 707;   // cos(45 degrees) * 1000
/* short holds reject stray samples; 40 ms polls need a few fresh samples,
   plus rendering latency */
constexpr uint32_t kOrientationWakeHoldMs = 40;
constexpr uint32_t kOrientationDimHoldMs = 80;
constexpr uint32_t kOrientationFilterMs = 30;
constexpr uint32_t kOrientationSampleGapMs = 250;
constexpr uint32_t kOrientationFaultMs = 2000;

// track filtered Z evidence and debounced wake/dim state
class OrientationWake {
 public:
  /* call every watch loop; freshValid requires a new sample, never cached;
     no-data/error calls still run the watchdog; true means display state changed */
  bool update(bool freshValid, int16_t screenNormalMilliG, uint32_t nowMs);
  // return the requested display state
  bool awake() const { return awake_; }
  // report whether fresh orientation evidence remains usable
  bool healthy() const { return sampled_; }
  // expose filtered Z in mg, or zero before usable data
  int16_t filteredZMilliG() const {
    return sampled_ ? static_cast<int16_t>(filteredQ8_ / 256) : 0;
  }

 private:
  // retain display state, filter value and sample/hold timing
  bool awake_ = true;  // missing/faulted sensor must leave the screen bright
  bool sampled_ = false;
  bool pending_ = false;
  int32_t filteredQ8_ = 0;
  uint32_t lastSampleMs_ = 0;
  uint32_t pendingSinceMs_ = 0;
};

}  // namespace watch
