#include "orientation_wake.h"

// orientation-driven display policy
namespace watch {

// filter fresh Z samples, debounce thresholds and recover bright after faults
bool OrientationWake::update(bool freshValid, int16_t screenNormalMilliG,
                             uint32_t nowMs) {
  // retain the prior state and wrap-safe time since the last sample
  const bool wasAwake = awake_;
  const uint32_t gapMs = nowMs - lastSampleMs_;

  // faults/long gaps invalidate posture; unsigned elapsed time handles millis() wraparound
  if (sampled_ && gapMs >= kOrientationFaultMs) {
    sampled_ = false;
    pending_ = false;
    awake_ = true;
  }
  // gaps break the consecutive-sample hold
  if (sampled_ && gapMs > kOrientationSampleGapMs) pending_ = false;

  // reject invalid/out-of-range +/-2 g samples; errors must not mimic 0 g/face-down
  if (!freshValid || screenNormalMilliG < -2000 || screenNormalMilliG > 2000)
    return wasAwake != awake_;

  // seed the filter from the first sample after a gap
  if (!sampled_ || gapMs > kOrientationSampleGapMs) {
    filteredQ8_ = static_cast<int32_t>(screenNormalMilliG) * 256;
    pending_ = false;
  }
  // duplicate timestamps cannot establish sustained posture
  else if (gapMs == 0) {
    return wasAwake != awake_;
  }
  /* time-based first-order low-pass; Q8 avoids 1 mg rounding stalls;
     gap <= 250 ms and |input| <= 2000 mg keep the product below INT32_MAX */
  else {
    const int32_t difference =
        static_cast<int32_t>(screenNormalMilliG) * 256 - filteredQ8_;
    int32_t step = difference * static_cast<int32_t>(gapMs) /
                   static_cast<int32_t>(kOrientationFilterMs + gapMs);
    // advance sub-1/256 mg steps to avoid truncation stalls just below a threshold
    if (step == 0 && difference != 0) step = difference > 0 ? 1 : -1;
    filteredQ8_ += step;
  }
  // retain fresh evidence for the next update
  sampled_ = true;
  lastSampleMs_ = nowMs;

  // separate wake/dim thresholds preserve state inside the hysteresis band
  const bool changeRequested =
      awake_ ? filteredQ8_ < static_cast<int32_t>(kOrientationDimMilliG) * 256
             : filteredQ8_ >= static_cast<int32_t>(kOrientationWakeMilliG) * 256;
  // cancel a hold when posture no longer requests a transition
  if (!changeRequested) {
    pending_ = false;
  }
  // start timing the requested posture
  else if (!pending_) {
    pending_ = true;
    pendingSinceMs_ = nowMs;
  }
  // commit after the state-specific hold duration
  else if (nowMs - pendingSinceMs_ >=
             (awake_ ? kOrientationDimHoldMs : kOrientationWakeHoldMs)) {
    awake_ = !awake_;
    pending_ = false;
  }
  return wasAwake != awake_;
}

}  // namespace watch
