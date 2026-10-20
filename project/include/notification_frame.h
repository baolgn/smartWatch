#pragma once

#include <stddef.h>
#include <stdint.h>

// bounded ANCS attributes, independent of BLE and the watch model.
namespace notificationRelay {
// retain defensive capacity; the PC publisher uses the observed 256-byte path limit.
constexpr size_t kMaxNotificationBytes = 3000;
// accept only the relay's app and dedicated notification title.
constexpr const char* kSourceApp = "io.heckel.ntfy";
constexpr const char* kRelayTitle = "XIAO Relay v1";

// assemble one attribute; reject mismatched headers, overflow and trailing bytes.
class AttributeFrame {
 public:
  // arm a fresh request with its exact identity and maximum value length.
  void start(uint32_t uid, uint8_t attribute, uint16_t limit);
  // consume any fragment size, including a split header.
  void push(const uint8_t* data, size_t length);
  // report a complete validated attribute.
  bool complete() const { return complete_; }
  // report an invalid or oversized response.
  bool failed() const { return failed_; }
  // expose only received attribute bytes; the terminator is additional.
  const char* data() const { return value_; }
  // report declared length after a complete header.
  size_t length() const { return length_; }

 private:
  // retain request identity and fragmented response state.
  uint32_t uid_ = 0;
  uint8_t attribute_ = 0;
  uint16_t limit_ = 0;
  uint8_t header_[8]{};
  size_t headerBytes_ = 0;
  size_t length_ = 0;
  size_t received_ = 0;
  bool complete_ = false;
  bool failed_ = false;
  char value_[kMaxNotificationBytes + 1]{};
};

// match only the selected app and dedicated title; tolerate iOS bidi wrappers.
bool metadataMatches(const char* app, size_t appLength, const char* title, size_t titleLength);
// fingerprint metadata for diagnostics; never use this hash to authorize messages.
uint32_t metadataFingerprint(const char* text, size_t length);
// parse the complete decimal size without truncation or integer overflow.
bool parseMessageSize(const char* text, size_t length, uint16_t& size);
// permit text-wrapped binary or legacy JSON; raw binary is not notification text.
bool asciiRelayBody(const char* text, size_t length);
}  // namespace notificationRelay
