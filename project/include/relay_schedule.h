#pragma once

#include <stdint.h>

// request NUS refreshes independently of the PC source schedule.
namespace watch {
// space connected NUS refresh requests fifteen minutes apart.
constexpr uint32_t kRelayIntervalMs = 15UL * 60UL * 1000UL;

// request fresh data on subscription and every fifteen connected minutes.
class RelaySchedule {
 public:
  // make a new BLE session immediately eligible for a refresh request.
  void reset() { queued_ = false; lastQueuedMs_ = 0; }
  // use unsigned elapsed time across millis() wraparound; never burst missed polls.
  bool due(uint32_t nowMs) const {
    return !queued_ || static_cast<uint32_t>(nowMs - lastQueuedMs_) >= kRelayIntervalMs;
  }
  // start the next interval only after the complete request is queued.
  void queued(uint32_t nowMs) { queued_ = true; lastQueuedMs_ = nowMs; }

 private:
  // retain one wrap-safe interval origin within the current connection.
  bool queued_ = false;
  uint32_t lastQueuedMs_ = 0;
};
}  // namespace watch
