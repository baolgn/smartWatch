#include "phone_protocol.h"
#include "strict_json.h"
#include "packed_weather.h"

#include <ArduinoJson.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// bounded phone framing, validation and acknowledgements.
namespace watch {
// private allocation, syntax and weather validators.
namespace {

// keep envelope copies inside the model's topic and payload buffers.
static_assert(kBinaryTopicBytes == kRelayTopicBytes && kMaxBinaryFrameBytes <= kGenericPayloadBytes,
              "XW1 limits must fit the watch model");

// cap ArduinoJson memory per message, including alignment/header overhead.
class BoundedAllocator : public ArduinoJson::Allocator {
 public:
  // allocate payload and aligned size header within the message cap.
  void* allocate(size_t bytes) override {
    // reject payload/header sizes exceeding remaining capacity.
    if (bytes > kJsonHeapLimit - used_ || sizeof(Header) > kJsonHeapLimit - used_ - bytes)
      return nullptr;
    Header* header = static_cast<Header*>(malloc(sizeof(Header) + bytes));
    // leave accounting unchanged if allocation fails.
    if (!header) return nullptr;
    header->bytes = bytes;
    used_ += sizeof(Header) + bytes;
    return header + 1;
  }

  // release a block and restore its payload/header budget.
  void deallocate(void* pointer) override {
    // ignore empty allocations.
    if (!pointer) return;
    Header* header = static_cast<Header*>(pointer) - 1;
    used_ -= sizeof(Header) + header->bytes;
    free(header);
  }

  // resize within the cap while preserving failed allocations.
  void* reallocate(void* pointer, size_t bytes) override {
    // treat an empty pointer as a new allocation.
    if (!pointer) return allocate(bytes);
    // release blocks resized to zero.
    if (!bytes) {
      deallocate(pointer);
      return nullptr;
    }
    // charge only growth; an existing block already owns its header budget.
    Header* old = static_cast<Header*>(pointer) - 1;
    const size_t previous = old->bytes;
    // reject growth beyond remaining capacity.
    if (bytes > previous && bytes - previous > kJsonHeapLimit - used_) return nullptr;
    Header* replacement = static_cast<Header*>(realloc(old, sizeof(Header) + bytes));
    // ArduinoJson assumes shrinking succeeds; keep the larger block on failure.
    if (!replacement) return bytes <= previous ? pointer : nullptr;
    replacement->bytes = bytes;
    used_ = used_ - previous + bytes;
    return replacement + 1;
  }

 private:
  // align payloads while retaining the requested allocation size.
  union Header {
    size_t bytes;
    long double alignment;
    void* pointerAlignment;
  };
  // track payload plus header bytes for this message.
  size_t used_ = 0;
};

// require an unsigned 32-bit JSON integer.
bool unsignedField(ArduinoJson::JsonVariantConst field, uint32_t& result) {
  // reject absent, negative or noninteger values.
  if (!field.is<uint32_t>()) return false;
  result = field.as<uint32_t>();
  return true;
}

// compare complete JSON strings, including possible embedded NUL bytes.
bool stringEquals(ArduinoJson::JsonVariantConst field, const char* expected) {
  // reject nonstring values.
  if (!field.is<const char*>()) return false;
  const ArduinoJson::JsonString text = field.as<ArduinoJson::JsonString>();
  return text.size() == strlen(expected) && memcmp(text.c_str(), expected, text.size()) == 0;
}

// reject numeric overflow anywhere in a generic payload.
bool finiteNumbers(ArduinoJson::JsonVariantConst value) {
  // inspect each object member recursively.
  if (value.is<ArduinoJson::JsonObjectConst>()) {
    // the syntax check already bounds depth and member count.
    for (ArduinoJson::JsonPairConst pair : value.as<ArduinoJson::JsonObjectConst>()) {
      // stop at the first overflowing number.
      if (!finiteNumbers(pair.value())) return false;
    }
  // inspect each array element recursively.
  } else if (value.is<ArduinoJson::JsonArrayConst>()) {
    // validate every nested numeric value.
    for (ArduinoJson::JsonVariantConst child : value.as<ArduinoJson::JsonArrayConst>()) {
      // stop at the first overflowing number.
      if (!finiteNumbers(child)) return false;
    }
  // allow only finite numeric conversions; strings/bools/null remain generic.
  } else if (value.is<double>() && !isfinite(value.as<double>())) return false;
  return true;
}

// validate a topic identifier before copying it into diagnostics and ACKs.
bool topicField(ArduinoJson::JsonVariantConst field, char* destination) {
  // topics must be strings.
  if (!field.is<const char*>()) return false;
  const ArduinoJson::JsonString topic = field.as<ArduinoJson::JsonString>();
  // require a nonempty identifier that fits its fixed buffer.
  if (!topic.size() || topic.size() > kRelayTopicBytes) return false;
  // allow ASCII letters/digits and internal dot, underscore, slash or hyphen.
  for (size_t i = 0; i < topic.size(); ++i) {
    const char c = topic.c_str()[i];
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    // the first character must be alphanumeric.
    if (!alnum && (i == 0 || (c != '.' && c != '_' && c != '/' && c != '-'))) return false;
  }
  memcpy(destination, topic.c_str(), topic.size());
  destination[topic.size()] = '\0';
  return true;
}

// read finite Celsius values within the protocol range.
bool temperatureField(ArduinoJson::JsonVariantConst field, float& result) {
  // require a numeric value without boolean coercion.
  if (!field.is<double>() || field.is<bool>()) return false;
  const double value = field.as<double>();
  // reject nonfinite or unsupported temperatures.
  if (!isfinite(value) || value < -100.0 || value > 70.0) return false;
  result = static_cast<float>(value);
  return true;
}

// read a supported weather condition code.
bool codeField(ArduinoJson::JsonVariantConst field, uint8_t& result) {
  uint32_t value;
  // validate before narrowing to one byte.
  if (!unsignedField(field, value) || !validWeatherCode(value)) return false;
  result = static_cast<uint8_t>(value);
  return true;
}

// validate current conditions and exactly 24 consecutive hourly forecasts.
bool readWeather(ArduinoJson::JsonObjectConst object, const WatchState& state,
                 WeatherSnapshot& snapshot, uint32_t nowMs, size_t expectedFields = 8) {
  // require the complete schema, valid UTC range and bounded conditions.
  if (object.size() != expectedFields || !unsignedField(object["observedAt"], snapshot.observedAt) ||
      !unsignedField(object["validUntil"], snapshot.validUntil) ||
      !validUtc(snapshot.observedAt) || !validUtc(snapshot.validUntil) ||
      snapshot.validUntil <= snapshot.observedAt ||
      snapshot.validUntil - snapshot.observedAt > 86400 ||
      !temperatureField(object["tempC"], snapshot.tempC) ||
      !codeField(object["code"], snapshot.code)) return false;
  // reject observations over five minutes ahead of a trusted clock.
  if (state.timeValid && static_cast<uint64_t>(snapshot.observedAt) >
      static_cast<uint64_t>(state.utc) + 300) return false;
  // require a forecast array.
  if (!object["hourly"].is<ArduinoJson::JsonArrayConst>()) return false;
  const ArduinoJson::JsonArrayConst hourly = object["hourly"].as<ArduinoJson::JsonArrayConst>();
  // reject partial or oversized forecasts.
  if (hourly.size() != kForecastHours) return false;
  // validate each row before marking the snapshot usable.
  for (size_t i = 0; i < kForecastHours; ++i) {
    // each row must be an object.
    if (!hourly[i].is<ArduinoJson::JsonObjectConst>()) return false;
    const ArduinoJson::JsonObjectConst row = hourly[i].as<ArduinoJson::JsonObjectConst>();
    HourForecast& hour = snapshot.hourly[i];
    // require exactly UTC, temperature and condition fields.
    if (row.size() != 3 || !unsignedField(row["utc"], hour.utc) || !validUtc(hour.utc) ||
        !temperatureField(row["tempC"], hour.tempC) || !codeField(row["code"], hour.code))
      return false;
    // anchor the first forecast to its observation.
    if (i == 0) {
      // allow a first hour no more than one hour after the observation.
      if (hour.utc < snapshot.observedAt || hour.utc - snapshot.observedAt > 3600) return false;
    }
    // require every later row exactly one hour apart.
    else if (static_cast<uint64_t>(hour.utc) !=
               static_cast<uint64_t>(snapshot.hourly[i - 1].utc) + 3600) return false;
  }
  // mark only the completely validated snapshot ready.
  snapshot.valid = true;
  snapshot.hourlyCount = kForecastHours;
  snapshot.receivedAtMs = nowMs;
  return true;
}

// validate the normalized relay snapshot, including all seven local dates.
bool readRelayWeather(ArduinoJson::JsonVariantConst payload, WatchState& next,
                      uint32_t nowMs) {
  // weather payloads use the same ten fields as upload normalization.
  if (!payload.is<ArduinoJson::JsonObjectConst>()) return false;
  const ArduinoJson::JsonObjectConst object = payload.as<ArduinoJson::JsonObjectConst>();
  next.weather = WeatherSnapshot{};
  // validate common current conditions and all 24 hourly rows first.
  if (!readWeather(object, next, next.weather, nowMs, 10)) return false;
  WeatherSnapshot& weather = next.weather;
  // validate fetch provenance, current-data lifetime and timezone types.
  if (!unsignedField(object["generatedAt"], weather.fetchedAt) || !validUtc(weather.fetchedAt) ||
      static_cast<uint64_t>(weather.observedAt) > static_cast<uint64_t>(weather.fetchedAt) + 300 ||
      weather.fetchedAt >= weather.validUntil || weather.validUntil - weather.observedAt > 3600 ||
      !object["offsetSeconds"].is<int32_t>() || !object["nextOffsetSeconds"].is<int32_t>() ||
      !unsignedField(object["nextTransitionUtc"], next.nextTransitionUtc)) return false;
  next.offsetSeconds = object["offsetSeconds"].as<int32_t>();
  next.nextOffsetSeconds = object["nextOffsetSeconds"].as<int32_t>();
  // retain only supported offsets and a future, bounded transition.
  if (!validOffset(next.offsetSeconds) || !validOffset(next.nextOffsetSeconds) ||
      (next.nextTransitionUtc && (!validUtc(next.nextTransitionUtc) || next.nextTransitionUtc <= weather.fetchedAt)) ||
      (!next.nextTransitionUtc && next.nextOffsetSeconds != next.offsetSeconds)) return false;
  // reject expired or delayed relay data when the RTC has trusted time.
  if (next.timeValid && (next.utc >= weather.validUntil ||
      static_cast<uint64_t>(weather.fetchedAt) > static_cast<uint64_t>(next.utc) + 300 ||
      static_cast<uint64_t>(next.utc) > static_cast<uint64_t>(weather.fetchedAt) + 900)) return false;
  // the first future hour must also be anchored to the fetch time.
  if (weather.hourly[0].utc < weather.fetchedAt ||
      static_cast<uint64_t>(weather.hourly[0].utc) > static_cast<uint64_t>(weather.fetchedAt) + 3600 ||
      !object["daily"].is<ArduinoJson::JsonArrayConst>()) return false;
  const ArduinoJson::JsonArrayConst daily = object["daily"].as<ArduinoJson::JsonArrayConst>();
  // require today and the following six complete local dates.
  if (daily.size() != kForecastDays) return false;
  // validate every daily row before publishing any weather or offset changes.
  for (size_t i = 0; i < kForecastDays; ++i) {
    // daily rows must be objects.
    if (!daily[i].is<ArduinoJson::JsonObjectConst>()) return false;
    const ArduinoJson::JsonObjectConst row = daily[i].as<ArduinoJson::JsonObjectConst>();
    DayForecast& day = weather.daily[i];
    // require exact UTC bounds, extrema and one supported modal icon.
    if (row.size() != 5 || !unsignedField(row["utc"], day.utc) || !validUtc(day.utc) ||
        !unsignedField(row["nextUtc"], day.nextUtc) || !validUtc(day.nextUtc) || day.nextUtc <= day.utc ||
        !temperatureField(row["minTempC"], day.minTempC) || !temperatureField(row["maxTempC"], day.maxTempC) ||
        day.minTempC > day.maxTempC || !codeField(row["code"], day.code)) return false;
    // compare UTC duration with both source-local midnight boundaries.
    const uint32_t seconds = day.nextUtc - day.utc;
    const int64_t localStart = static_cast<int64_t>(day.utc) + offsetAt(next, day.utc);
    const int64_t localEnd = static_cast<int64_t>(day.nextUtc) + offsetAt(next, day.nextUtc);
    // future dates use known offsets; today's start may precede an earlier DST change.
    if ((seconds != 23 * 3600 && seconds != 24 * 3600 && seconds != 25 * 3600) ||
        localEnd % 86400 || (i && (localStart % 86400 || localEnd - localStart != 86400)) ||
        (i && day.utc != weather.daily[i - 1].nextUtc)) return false;
    // infer today's midnight offset from its end and match the fetch's local date.
    if (i == 0) {
      const int32_t startOffset = offsetAt(next, day.nextUtc) + static_cast<int32_t>(seconds) - 86400;
      const int64_t localFetch = static_cast<int64_t>(weather.fetchedAt) + offsetAt(next, weather.fetchedAt);
      // allow the earlier offset only within this complete source-local day.
      if (!validOffset(startOffset) || weather.fetchedAt < day.utc || weather.fetchedAt >= day.nextUtc ||
          localFetch / 86400 != localEnd / 86400 - 1) return false;
    }
  }
  // publish complete daily coverage and the validated display offset.
  weather.dailyCount = kForecastDays;
  next.offsetKnown = true;
  ++next.weatherRevision;
  return true;
}

}  // namespace

// assemble one LF-terminated frame and discard malformed input through LF.
FrameEvent LineFramer::push(uint8_t byte, uint32_t nowMs) {
  // release the previously consumed frame before accepting a new byte.
  if (ready_) {
    length_ = 0;
    ready_ = false;
  }
  lastByteMs_ = nowMs;
  // ignore remaining bytes of a rejected frame.
  if (discarding_) {
    // resume normal framing after its LF.
    if (byte == '\n') reset();
    return FrameEvent::None;
  }
  // terminate a complete nonempty frame.
  if (byte == '\n') {
    // ignore blank lines.
    if (length_ == 0) return FrameEvent::None;
    data_[length_] = '\0';
    ready_ = true;
    return FrameEvent::Ready;
  }
  // allow printable ASCII plus CR/tab; reject NUL and other control bytes.
  if (byte == 0 || (byte < 32 && byte != '\r' && byte != '\t') || byte >= 127) {
    discardToNewline();
    return FrameEvent::InvalidByte;
  }
  // preserve the terminator slot when the frame is full.
  if (length_ == kMaxFrameBytes) {
    discardToNewline();
    return FrameEvent::TooLong;
  }
  data_[length_++] = static_cast<char>(byte);
  return FrameEvent::None;
}

// time out incomplete frames without treating trailing bytes as a new message.
bool LineFramer::expire(uint32_t nowMs) {
  // discard a partial frame after its idle deadline.
  if (!ready_ && !discarding_ && length_ != 0 && nowMs - lastByteMs_ >= kFrameTimeoutMs) {
    discardToNewline();
    return true;
  }
  return false;
}

// clear frame contents, timing and completion/discard state.
void LineFramer::reset() {
  length_ = 0;
  lastByteMs_ = 0;
  discarding_ = false;
  ready_ = false;
  data_[0] = '\0';
}

// reset buffered data and ignore bytes through LF.
void LineFramer::discardToNewline() {
  reset();
  discarding_ = true;
}

// forget sequence history when the BLE session changes.
void ProtocolSession::resetSession() {
  // clear each message family's sequence state.
  for (size_t i = 0; i < sizeof(haveSequence_) / sizeof(haveSequence_[0]); ++i) {
    lastSequence_[i] = 0;
    haveSequence_[i] = false;
  }
  // permit fresh sequences for all topics after reconnecting.
  for (size_t i = 0; i < kRelayTopicsPerSession; ++i) topics_[i] = TopicSequence{};
  topicCount_ = 0;
}

// dispatch text without reserving both formats' staged model stack frames.
ProtocolResult ProtocolSession::apply(const char* json, size_t length,
                                      WatchState& state, uint32_t nowMs) {
  // XW1 uses the same ASCII line transport with a binary payload.
  if (json && length >= 4 && memcmp(json, "XW1:", 4) == 0) {
    uint8_t storage[kMaxBinaryFrameBytes];
    BinaryEnvelope envelope;
    // reject malformed wrappers before allocating weather or staging state.
    if (!parseBinaryText(json, length, storage, sizeof(storage), envelope)) {
      ProtocolResult invalid;
      invalid.status = ProtocolStatus::InvalidBinary;
      return invalid;
    }
    return applyBinaryEnvelope(envelope, state, nowMs);
  }
  return applyJson(json, length, state, nowMs);
}

// validate legacy JSON into a copy before committing state and sequence.
ProtocolResult ProtocolSession::applyJson(const char* json, size_t length,
                                          WatchState& state, uint32_t nowMs) {
  // retain rejection by default and locate the original generic payload bytes.
  ProtocolResult result;
  size_t payloadStart = 0, payloadLength = 0;
  // reject absent, oversized or syntactically invalid frames before allocation.
  if (!strictJsonSyntax(json, length, &payloadStart, &payloadLength)) {
    result.status = ProtocolStatus::InvalidJson;
    return result;
  }
  // deserialize with bounded heap use and nesting depth.
  BoundedAllocator allocator;
  ArduinoJson::JsonDocument document(&allocator);
  const ArduinoJson::DeserializationError error =
      ArduinoJson::deserializeJson(document, json, length, ArduinoJson::DeserializationOption::NestingLimit(kJsonDepthLimit));
  // distinguish allocation failure from malformed JSON.
  if (error) {
    result.status = error == ArduinoJson::DeserializationError::NoMemory
                        ? ProtocolStatus::NoMemory : ProtocolStatus::InvalidJson;
    return result;
  }
  // only object roots are protocol messages.
  if (!document.is<ArduinoJson::JsonObject>()) return result;
  const ArduinoJson::JsonObjectConst object = document.as<ArduinoJson::JsonObjectConst>();
  result.hasSequence = unsignedField(object["seq"], result.sequence);
  // classify complete string values without accepting NUL-suffixed aliases.
  if (stringEquals(object["type"], "weather")) result.kind = MessageKind::Weather;
  // retain the existing settings schema.
  else if (stringEquals(object["type"], "settings")) result.kind = MessageKind::Settings;
  // dispatch generic relay envelopes independently of legacy families.
  else if (stringEquals(object["type"], "data")) result.kind = MessageKind::Data;
  uint32_t version = 0;
  // require a sequence, known type and supported version.
  if (!result.hasSequence || result.kind == MessageKind::Unknown ||
      !unsignedField(object["v"], version) || version != 1) return result;
  // legacy messages retain their unescaped-string contract.
  if (result.kind != MessageKind::Data && memchr(json, '\\', length)) {
    result.status = ProtocolStatus::InvalidJson;
    return result;
  }
  // reject numbers that overflow the device's JSON numeric representation.
  if (!finiteNumbers(object)) return result;

  // stage changes without mutating live state on rejection.
  WatchState next = state;
  // settings accept only the units field.
  if (result.kind == MessageKind::Settings) {
    // enforce the exact settings schema.
    if (object.size() != 4 || !object["units"].is<const char*>()) return result;
    const char* units = object["units"].as<const char*>();
    // select Celsius.
    if (strcmp(units, "C") == 0) next.units = TemperatureUnit::Celsius;
    // select Fahrenheit.
    else if (strcmp(units, "F") == 0) next.units = TemperatureUnit::Fahrenheit;
    // reject unknown units.
    else return result;
  // generic topics share framing but have independent handlers and sequence histories.
  } else if (result.kind == MessageKind::Data) {
    // require the exact envelope and a complete payload, including JSON null.
    if (object.size() != 5 || !topicField(object["topic"], result.topic) ||
        object["payload"].isUnbound() || !payloadLength) return result;
    // normalize weather into existing bounded arrays without touching RTC time.
    if (strcmp(result.topic, "weather") == 0) {
      // expand compact notifications before reusing the complete weather validator.
      const PackedWeatherStatus packed = expandPackedWeather(document, document["payload"]);
      // malformed packing and allocation failure never consume state or sequences.
      if (packed == PackedWeatherStatus::Invalid || packed == PackedWeatherStatus::NoMemory) {
        result.status = packed == PackedWeatherStatus::NoMemory ? ProtocolStatus::NoMemory
                                                                : ProtocolStatus::InvalidMessage;
        return result;
      }
      // reject the entire envelope if any forecast or offset field is invalid.
      if (!readRelayWeather(object["payload"], next, nowMs)) return result;
    // cache other topics verbatim until an application handler is added.
    } else {
      // retain one bounded generic JSON value without truncation.
      if (payloadLength > kGenericPayloadBytes) {
        result.status = ProtocolStatus::PayloadTooLarge;
        return result;
      }
      strcpy(next.genericTopic, result.topic);
      next.genericSequence = result.sequence;
      next.genericPayloadBytes = static_cast<uint16_t>(payloadLength);
      next.genericEncoding = PayloadEncoding::Json;
      memcpy(next.genericPayload, json + payloadStart, payloadLength);
      next.genericPayload[payloadLength] = '\0';
    }
  // legacy weather replaces uploaded days/metadata as before.
  } else {
    next.weather = WeatherSnapshot{};
    // require the complete valid 24-hour forecast.
    if (!readWeather(object, state, next.weather, nowMs)) return result;
    ++next.weatherRevision;
  }

  // resolve a bounded per-topic history after full payload validation.
  if (result.kind == MessageKind::Data) return commitData(result, state, next, nowMs);

  // compare legacy sequences within their original message families.
  const size_t index = static_cast<size_t>(result.kind) - 1;
  // enforce ordering once a previous sequence exists.
  if (haveSequence_[index]) {
    // acknowledge duplicates without applying them again.
    if (result.sequence == lastSequence_[index]) {
      result.status = ProtocolStatus::Duplicate;
      return result;
    }
    // reject older messages.
    if (result.sequence < lastSequence_[index]) {
      result.status = ProtocolStatus::OutOfOrder;
      return result;
    }
  }
  // commit once, after schema/range/sequence validation.
  state = next;
  haveSequence_[index] = true;
  lastSequence_[index] = result.sequence;
  result.status = ProtocolStatus::Applied;
  return result;
}

// parse raw packets for transports that already preserve binary boundaries.
ProtocolResult ProtocolSession::applyBinary(const uint8_t* bytes, size_t length,
                                            WatchState& state, uint32_t nowMs) {
  BinaryEnvelope envelope;
  // validate lengths, CRC and typed values before reading application fields.
  if (!parseBinaryFrame(bytes, length, envelope)) {
    ProtocolResult invalid;
    invalid.status = ProtocolStatus::InvalidBinary;
    return invalid;
  }
  return applyBinaryEnvelope(envelope, state, nowMs);
}

// stage binary payloads through the same weather rules and topic history.
ProtocolResult ProtocolSession::applyBinaryEnvelope(const BinaryEnvelope& envelope,
                                                    WatchState& state, uint32_t nowMs) {
  ProtocolResult result;
  result.kind = MessageKind::Data;
  result.hasSequence = true;
  result.sequence = envelope.sequence;
  strcpy(result.topic, envelope.topic);
  WatchState next = state;
  // packed weather expands internally without any JSON text on the wire.
  if (envelope.codec == 1) {
    BoundedAllocator allocator;
    ArduinoJson::JsonDocument document(&allocator);
    // bind the member before converting its proxy to a mutable variant.
    document["payload"].to<ArduinoJson::JsonObject>();
    const PackedWeatherStatus status = expandPackedWeatherBytes(document, document["payload"],
                                                                 envelope.payload, envelope.payloadBytes);
    // expansion and semantic failures leave live state and sequences untouched.
    if (status != PackedWeatherStatus::Expanded) {
      result.status = status == PackedWeatherStatus::NoMemory ? ProtocolStatus::NoMemory
                                                             : ProtocolStatus::InvalidMessage;
      return result;
    }
    // reuse all existing coverage, WMO, DST and RTC freshness checks.
    if (!readRelayWeather(document["payload"], next, nowMs)) return result;
  // generic consumers receive validated typed bytes without JSON conversion.
  } else {
    strcpy(next.genericTopic, result.topic);
    next.genericSequence = result.sequence;
    next.genericEncoding = PayloadEncoding::Binary;
    next.genericPayloadBytes = static_cast<uint16_t>(envelope.payloadBytes);
    memcpy(next.genericPayload, envelope.payload, envelope.payloadBytes);
    next.genericPayload[envelope.payloadBytes] = '\0';
  }
  return commitData(result, state, next, nowMs);
}

// commit relay state only after shared topic ordering accepts the whole message.
ProtocolResult ProtocolSession::commitData(ProtocolResult result, WatchState& state,
                                           WatchState& next, uint32_t nowMs) {
  size_t index = 0;
  // find an existing topic without evicting other sequence histories.
  while (index < topicCount_ && strcmp(topics_[index].topic, result.topic)) ++index;
  // compare sequences only within the same topic.
  if (index < topicCount_) {
    // duplicates leave cached data and receipt timestamps unchanged.
    if (result.sequence == topics_[index].sequence) {
      result.status = ProtocolStatus::Duplicate;
      return result;
    }
    // older messages never consume a sequence.
    if (result.sequence < topics_[index].sequence) {
      result.status = ProtocolStatus::OutOfOrder;
      return result;
    }
  // a ninth topic requires the transport's next explicit session reset.
  } else if (topicCount_ == kRelayTopicsPerSession) {
    result.status = ProtocolStatus::TopicLimit;
    return result;
  }
  // record accepted provenance before committing the staged model.
  strcpy(next.relayTopic, result.topic);
  next.relaySequence = result.sequence;
  next.relayReceivedAtMs = nowMs;
  ++next.relayRevision;
  state = next;
  strcpy(topics_[index].topic, result.topic);
  topics_[index].sequence = result.sequence;
  // reserve new history only after every application check succeeds.
  if (index == topicCount_) ++topicCount_;
  result.status = ProtocolStatus::Applied;
  return result;
}

// map message families to stable JSON names.
const char* messageKindName(MessageKind kind) {
  // select the protocol's lowercase type name.
  switch (kind) {
    // name weather replacements.
    case MessageKind::Weather: return "weather";
    // name settings changes.
    case MessageKind::Settings: return "settings";
    // name generic relay envelopes.
    case MessageKind::Data: return "data";
    // identify unsupported families.
    default: return "unknown";
  }
}

// map processing outcomes to stable acknowledgement names.
const char* protocolStatusName(ProtocolStatus status) {
  // select the wire status.
  switch (status) {
    // report an applied message.
    case ProtocolStatus::Applied: return "applied";
    // report an already accepted sequence.
    case ProtocolStatus::Duplicate: return "duplicate";
    // report a sequence older than the last accepted message.
    case ProtocolStatus::OutOfOrder: return "out_of_order";
    // report invalid JSON syntax.
    case ProtocolStatus::InvalidJson: return "invalid_json";
    // report malformed binary framing, checksum or typed values.
    case ProtocolStatus::InvalidBinary: return "invalid_binary";
    // report a schema or value rejection.
    case ProtocolStatus::InvalidMessage: return "invalid_message";
    // report exhausted allocation budget.
    case ProtocolStatus::NoMemory: return "no_memory";
    // bound sequence tracking without silently evicting a topic.
    case ProtocolStatus::TopicLimit: return "topic_limit";
    // reject generic data that cannot fit its cache.
    case ProtocolStatus::PayloadTooLarge: return "payload_too_large";
    // map unexpected outcomes to rejection.
    default: return "invalid_message";
  }
}

// format one LF-terminated JSON ACK; report 0 on insufficient output space.
size_t formatAcknowledgement(const ProtocolResult& result, char* buffer,
                             size_t capacity) {
  // require writable output storage.
  if (!buffer || capacity == 0) return 0;
  // applied and duplicate messages count as successful acknowledgements.
  const char* ok = result.status == ProtocolStatus::Applied ||
                   result.status == ProtocolStatus::Duplicate ? "true" : "false";
  char sequence[16];
  // echo a validated sequence.
  if (result.hasSequence) snprintf(sequence, sizeof(sequence), "%lu", static_cast<unsigned long>(result.sequence));
  // use JSON null when no sequence could be read.
  else strcpy(sequence, "null");
  // echo validated topics so relays can match replies from independent streams.
  char topic[kRelayTopicBytes + 12] = {};
  // topic validation excludes quotes, backslashes and controls.
  if (result.kind == MessageKind::Data && result.topic[0])
    snprintf(topic, sizeof(topic), ",\"topic\":\"%s\"", result.topic);
  // encode the complete acknowledgement in the caller's buffer.
  const int length = snprintf(buffer, capacity,
      "{\"v\":1,\"type\":\"ack\",\"for\":\"%s\",\"seq\":%s,\"ok\":%s,\"status\":\"%s\"%s}\n",
      messageKindName(result.kind), sequence, ok, protocolStatusName(result.status), topic);
  // suppress malformed/truncated acknowledgement output.
  if (length < 0 || static_cast<size_t>(length) >= capacity) {
    buffer[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(length);
}

}  // namespace watch
