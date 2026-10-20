#include "notification_frame.h"

#include <string.h>

// validate ANCS framing before any notification reaches the model.
namespace notificationRelay {
// reset all partial state without clearing unused payload capacity.
void AttributeFrame::start(uint32_t uid, uint8_t attribute, uint16_t limit) {
  uid_ = uid;
  attribute_ = attribute;
  limit_ = limit;
  headerBytes_ = length_ = received_ = 0;
  complete_ = false;
  failed_ = limit > kMaxNotificationBytes;
  value_[0] = '\0';
}

// decode one header and its bounded value across arbitrary BLE fragments.
void AttributeFrame::push(const uint8_t* data, size_t count) {
  // a completed response cannot contain another tuple or trailing bytes.
  if (failed_ || count == 0) return;
  // reject invalid pointers and excess data after completion.
  if (!data || complete_) { failed_ = true; return; }
  // consume every byte, validating the full header before the value.
  for (size_t i = 0; i < count; ++i) {
    // collect the fixed command/UID/attribute/length header.
    if (headerBytes_ < sizeof(header_)) {
      header_[headerBytes_++] = data[i];
      // defer decoding until even a one-byte fragmentation is complete.
      if (headerBytes_ != sizeof(header_)) continue;
      // decode the complete little-endian identity and declared length.
      const uint32_t uid = static_cast<uint32_t>(header_[1]) |
          (static_cast<uint32_t>(header_[2]) << 8) |
          (static_cast<uint32_t>(header_[3]) << 16) |
          (static_cast<uint32_t>(header_[4]) << 24);
      length_ = static_cast<uint16_t>(header_[6]) |
          (static_cast<uint16_t>(header_[7]) << 8);
      // match the outstanding request and reject excessive declarations.
      if (header_[0] != 0 || uid != uid_ || header_[5] != attribute_ || length_ > limit_) {
        failed_ = true;
        return;
      }
      complete_ = length_ == 0;
    }
    // collect the declared value after its validated header.
    else {
      // prevent overflow even when a packet contains bytes after the value.
      if (received_ >= length_) { failed_ = true; return; }
      value_[received_++] = static_cast<char>(data[i]);
      value_[received_] = '\0';
      complete_ = received_ == length_;
    }
    // reject a second tuple in the same notification.
    if (complete_ && i + 1 != count) { failed_ = true; return; }
  }
}

// retain a diagnostic fingerprint without logging notification text.
uint32_t metadataFingerprint(const char* text, size_t length) {
  // distinguish unavailable data from a successfully retrieved empty attribute.
  if (!text) return 0;
  uint32_t hash = 2166136261u;
  // FNV-1a includes every received byte, including any unexpected NUL.
  for (size_t i = 0; i < length; ++i) {
    hash = (hash ^ static_cast<uint8_t>(text[i])) * 16777619u;
  }
  return hash;
}

// accept no other app's messages or unrelated ntfy notifications.
bool metadataMatches(const char* app, size_t appLength, const char* title, size_t titleLength) {
  // require the complete app identifier before examining the title.
  if (!app || !title || appLength != strlen(kSourceApp) ||
      memcmp(app, kSourceApp, appLength) != 0) return false;
  // remove only paired iOS directional markers, never other title bytes.
  if (titleLength >= 6 && memcmp(title, "\xE2\x80\xAD", 3) == 0 &&
      memcmp(title + titleLength - 3, "\xE2\x80\xAC", 3) == 0) {
    title += 3;
    titleLength -= 6;
  }
  return titleLength == strlen(kRelayTitle) && memcmp(title, kRelayTitle, titleLength) == 0;
}

// reject empty, nondecimal, zero and oversized notification lengths.
bool parseMessageSize(const char* text, size_t length, uint16_t& size) {
  // require a complete short decimal field.
  if (!text || length == 0 || length > 10) return false;
  uint32_t result = 0;
  // constrain each digit before multiplication can overflow.
  for (size_t i = 0; i < length; ++i) {
    // reject punctuation, signs, whitespace and embedded NUL.
    if (text[i] < '0' || text[i] > '9') return false;
    result = result * 10 + static_cast<unsigned>(text[i] - '0');
    // reject before another multiplication once the allowed bound is passed.
    if (result > kMaxNotificationBytes) return false;
  }
  // a useful relay envelope cannot be empty.
  if (result == 0) return false;
  size = static_cast<uint16_t>(result);
  return true;
}

// preserve legacy whitespace but reject NUL, DEL and raw Unicode.
bool asciiRelayBody(const char* text, size_t length) {
  // accept only a nonempty bounded body.
  if (!text || length == 0 || length > kMaxNotificationBytes) return false;
  // the selected protocol parser validates syntax after transport filtering.
  for (size_t i = 0; i < length; ++i) {
    const uint8_t c = static_cast<uint8_t>(text[i]);
    // permit printable ASCII and the four JSON whitespace characters.
    if ((c < 32 && c != '\t' && c != '\r' && c != '\n') || c >= 127) return false;
  }
  return true;
}
}  // namespace notificationRelay
