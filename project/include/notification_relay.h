#pragma once

#include "notification_frame.h"

// ANCS transport owns BLE work; the watch task alone applies relay data.
namespace notificationRelay {
// cached counters never contain unrelated notification contents.
struct Diagnostics {
  bool ready = false;
  bool secured = false;
  bool subscribed = false;  // CCCD writes acknowledged; delivery still needs a notification.
  uint32_t received = 0;
  uint32_t filtered = 0;
  uint32_t filteredApp = 0;
  uint32_t filteredTitle = 0;
  uint32_t lastAppHash = 0;  // FNV-1a; diagnostic only.
  uint32_t lastTitleHash = 0;  // only populated for the exact ntfy app.
  uint16_t lastAppLength = 0;
  uint16_t lastTitleLength = 0;
  uint8_t lastMetadataFlags = 0;  // bit 0: app NUL; bit 1: title NUL; bit 2: paired bidi.
  uint16_t lastReportedBodyLength = 0;
  uint16_t lastBodyLength = 0;  // last complete response, before size/protocol checks.
  uint32_t lastBodyHash = 0;  // FNV-1a; diagnostic only, no content logged.
  uint32_t errors = 0;
  uint32_t dropped = 0;
  uint32_t stackFree = 0;
};
// register UUIDs and start the raw ATT worker after Bluefruit.begin().
bool begin();
// place ANCS solicitation in the primary advertisement.
bool addSolicitation();
// copy one complete body; commit only while generation() still matches.
bool takeFrame(char* buffer, size_t capacity, size_t& length, uint32_t& generation);
// identify the current connection/service generation for atomic model commits.
uint32_t generation();
// return a short synchronized diagnostic snapshot.
Diagnostics diagnostics();
}  // namespace notificationRelay
