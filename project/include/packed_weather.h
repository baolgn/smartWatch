#pragma once

#include <ArduinoJson.h>

// expand the weather adapter's compact representation before normal validation.
namespace watch {

// distinguish ordinary payloads, malformed packing and bounded allocation failure.
enum class PackedWeatherStatus : uint8_t { Unpacked, Expanded, Invalid, NoMemory };

// replace one exact packed object with the existing normalized weather schema.
PackedWeatherStatus expandPackedWeather(ArduinoJson::JsonDocument& document,
                                        ArduinoJson::JsonVariant payload);
// expand the same 118-byte weather format without a JSON/Base64 wire envelope.
PackedWeatherStatus expandPackedWeatherBytes(ArduinoJson::JsonDocument& document,
                                             ArduinoJson::JsonVariant payload,
                                             const uint8_t* bytes, size_t length);

}  // namespace watch
