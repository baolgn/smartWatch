#include "packed_weather.h"

#include <stdint.h>

// decode weather packing independently of generic transport and semantic validation.
namespace watch {
// keep fixed wire limits and byte-order helpers private.
namespace {
// fix the version-one header and encoded payload lengths.
constexpr size_t kBinaryBytes = 118;
constexpr size_t kBase64Bytes = 160;
constexpr size_t kHeaderBytes = 29;

// accept only the standard Base64 alphabet.
int base64Value(char byte) {
  // decode uppercase letters without locale-dependent classification.
  if (byte >= 'A' && byte <= 'Z') return byte - 'A';
  // decode lowercase letters.
  if (byte >= 'a' && byte <= 'z') return byte - 'a' + 26;
  // decode decimal digits.
  if (byte >= '0' && byte <= '9') return byte - '0' + 52;
  // decode the standard plus symbol.
  if (byte == '+') return 62;
  // decode the standard slash symbol.
  if (byte == '/') return 63;
  return -1;
}

// decode exactly 118 bytes, rejecting noncanonical padding and embedded NUL.
bool decodeBase64(ArduinoJson::JsonString text, uint8_t* output) {
  // the final one-byte group requires two padding characters.
  if (text.size() != kBase64Bytes) return false;
  const char* bytes = text.c_str();
  size_t written = 0;
  // every quartet has a fixed output size except the final group.
  for (size_t i = 0; i < kBase64Bytes; i += 4) {
    const int a = base64Value(bytes[i]), b = base64Value(bytes[i + 1]);
    // padding is never valid in the first two positions.
    if (a < 0 || b < 0) return false;
    output[written++] = static_cast<uint8_t>((a << 2) | (b >> 4));
    // canonical Base64 requires unused low bits to be zero.
    if (i + 4 == kBase64Bytes) {
      // only one decoded byte remains.
      if (bytes[i + 2] != '=' || bytes[i + 3] != '=' || (b & 15)) return false;
    }
    // decode all three bytes of an interior quartet.
    else {
      const int c = base64Value(bytes[i + 2]), d = base64Value(bytes[i + 3]);
      // interior padding, whitespace and URL-safe aliases are invalid.
      if (c < 0 || d < 0) return false;
      output[written++] = static_cast<uint8_t>((b << 4) | (c >> 2));
      output[written++] = static_cast<uint8_t>((c << 6) | d);
    }
  }
  return written == kBinaryBytes;
}

// load unaligned little-endian timestamps without pointer casts.
uint32_t unsigned32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
      (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

// sign-extend the two-byte timezone-minute field portably.
int32_t signed16(const uint8_t* bytes) {
  const uint32_t value = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8);
  return value & 0x8000 ? static_cast<int32_t>(value) - 65536 : static_cast<int32_t>(value);
}

// consume the fixed 706-bit payload least-significant bit first.
class BitReader {
 public:
  // bind the already validated fixed-size byte buffer.
  explicit BitReader(const uint8_t* bytes) : bytes_(bytes) {}
  // callers use only the fixed 12-, 7- and 2-bit schema fields.
  uint16_t read(uint8_t width) {
    uint16_t value = 0;
    // assemble fields spanning arbitrary byte boundaries.
    for (uint8_t i = 0; i < width; ++i, ++position_)
      value |= static_cast<uint16_t>((bytes_[position_ / 8] >> (position_ % 8)) & 1) << i;
    return value;
  }

 private:
  // fixed-schema calls never exceed the 89-byte stream.
  const uint8_t* bytes_;
  size_t position_ = 0;
};
}  // namespace

// reconstruct the existing schema; readRelayWeather remains its semantic authority.
PackedWeatherStatus expandPackedWeather(ArduinoJson::JsonDocument& document,
                                        ArduinoJson::JsonVariant payload) {
  // ordinary weather payloads keep their original validation path.
  if (!payload.is<ArduinoJson::JsonObject>() || payload["packed"].isUnbound())
    return PackedWeatherStatus::Unpacked;
  const ArduinoJson::JsonObject object = payload.as<ArduinoJson::JsonObject>();
  // reject extra fields, nonstrings and encoded aliases before changing the document.
  if (object.size() != 1 || !object["packed"].is<const char*>()) return PackedWeatherStatus::Invalid;
  uint8_t bytes[kBinaryBytes]{};
  // copy decoded bytes before clearing the JSON string that owns the input pointer.
  if (!decodeBase64(object["packed"].as<ArduinoJson::JsonString>(), bytes)) return PackedWeatherStatus::Invalid;
  return expandPackedWeatherBytes(document, payload, bytes, sizeof(bytes));
}

// share exact packed-weather validation between legacy and XW1 transports.
PackedWeatherStatus expandPackedWeatherBytes(ArduinoJson::JsonDocument& document,
                                             ArduinoJson::JsonVariant payload,
                                             const uint8_t* bytes, size_t length) {
  // reject malformed sizes, versions and reserved bits before reading fields.
  if (!bytes || length != kBinaryBytes || bytes[0] != 1 ||
      (bytes[kBinaryBytes - 1] & 0xFC)) return PackedWeatherStatus::Invalid;

  // retain all fields before replacing the owning JSON payload.
  BitReader bits(bytes + kHeaderBytes);
  int16_t temperatures[39]{};
  uint8_t codes[32]{}, durations[7]{};
  // decode current, hourly and alternating daily minimum/maximum tenths.
  for (auto& temperature : temperatures) {
    const uint16_t value = bits.read(12);
    temperature = static_cast<int16_t>(value & 0x800 ? static_cast<int>(value) - 4096 : value);
  }
  // retain WMO codes unchanged for the shared weather validator.
  for (auto& code : codes) code = static_cast<uint8_t>(bits.read(7));
  // accumulate UTC bounds widely before narrowing to the normalized schema.
  uint64_t dayEnd = unsigned32(bytes + 25);
  const uint32_t hourlyStart = unsigned32(bytes + 21);
  // prevent UTC addition from wrapping before semantic range checks.
  if (static_cast<uint64_t>(hourlyStart) + 23 * 3600ULL > UINT32_MAX)
    return PackedWeatherStatus::Invalid;
  // reconstruct contiguous local days, preserving 23/24/25-hour DST spans.
  for (auto& duration : durations) {
    duration = static_cast<uint8_t>(bits.read(2));
    // code three is reserved, not a 26-hour day.
    if (duration == 3) return PackedWeatherStatus::Invalid;
    dayEnd += static_cast<uint64_t>(23 + duration) * 3600;
    // the last endpoint must still fit the normalized uint32 schema.
    if (dayEnd > UINT32_MAX) return PackedWeatherStatus::Invalid;
  }

  // replace packing with the full adapter's ten normalized fields.
  ArduinoJson::JsonObject object = payload.to<ArduinoJson::JsonObject>();
  object["generatedAt"] = unsigned32(bytes + 1);
  object["offsetSeconds"] = signed16(bytes + 5) * 60;
  object["nextTransitionUtc"] = unsigned32(bytes + 7);
  object["nextOffsetSeconds"] = signed16(bytes + 11) * 60;
  object["observedAt"] = unsigned32(bytes + 13);
  object["validUntil"] = unsigned32(bytes + 17);
  object["tempC"] = temperatures[0] / 10.0;
  object["code"] = codes[0];
  ArduinoJson::JsonArray hourly = object["hourly"].to<ArduinoJson::JsonArray>();
  // hourly predictions are exactly one hour apart.
  for (size_t i = 0; i < 24; ++i) {
    ArduinoJson::JsonObject row = hourly.add<ArduinoJson::JsonObject>();
    row["utc"] = hourlyStart + static_cast<uint32_t>(i) * 3600;
    row["tempC"] = temperatures[i + 1] / 10.0;
    row["code"] = codes[i + 1];
  }
  // start daily rows at the source's first local midnight.
  ArduinoJson::JsonArray daily = object["daily"].to<ArduinoJson::JsonArray>();
  uint32_t dayStart = unsigned32(bytes + 25);
  // day bounds remain contiguous, including known DST changes.
  for (size_t i = 0; i < 7; ++i) {
    const uint32_t nextDay = dayStart + static_cast<uint32_t>(23 + durations[i]) * 3600;
    ArduinoJson::JsonObject row = daily.add<ArduinoJson::JsonObject>();
    row["utc"] = dayStart;
    row["nextUtc"] = nextDay;
    row["minTempC"] = temperatures[25 + 2 * i] / 10.0;
    row["maxTempC"] = temperatures[26 + 2 * i] / 10.0;
    row["code"] = codes[25 + i];
    dayStart = nextDay;
  }
  return document.overflowed() ? PackedWeatherStatus::NoMemory : PackedWeatherStatus::Expanded;
}
}  // namespace watch
