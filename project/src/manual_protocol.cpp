#include "manual_protocol.h"

#include <cstdio>
#include <cstring>
#include <limits.h>

// validate manual commands and stage watch data.
namespace watch {
// keep field parsers private to the manual protocol.
namespace {
// reference a comma-delimited field without copying.
struct Field { const char* text; size_t length; };

// compare the complete bounded field.
bool same(Field field, const char* value) {
  return field.length == std::strlen(value) &&
         std::memcmp(field.text, value, field.length) == 0;
}

// parse decimal digits without overflow or partial output.
bool unsignedNumber(Field field, uint32_t& value) {
  // reject empty numbers.
  if (!field.length) return false;
  uint32_t parsed = 0;
  // accumulate each decimal digit.
  for (size_t i = 0; i < field.length; ++i) {
    // reject signs, whitespace and non-digits.
    if (field.text[i] < '0' || field.text[i] > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(field.text[i] - '0');
    // check multiplication/addition before overflow.
    if (parsed > (UINT32_MAX - digit) / 10) return false;
    parsed = parsed * 10 + digit;
  }
  value = parsed;
  return true;
}

// parse signed decimal, including INT32_MIN.
bool signedNumber(Field field, int32_t& value) {
  // reject an absent value.
  if (!field.length) return false;
  const bool negative = field.text[0] == '-';
  // remove the optional minus sign before magnitude parsing.
  if (negative) { ++field.text; --field.length; }
  uint32_t magnitude = 0;
  // allow the extra negative magnitude only for INT32_MIN.
  if (!unsignedNumber(field, magnitude) ||
      magnitude > static_cast<uint32_t>(INT32_MAX) + (negative ? 1U : 0U))
    return false;
  value = negative ? static_cast<int32_t>(-static_cast<int64_t>(magnitude))
                   : static_cast<int32_t>(magnitude);
  return true;
}

// build a bounded ASCII command result.
ManualResult result(bool ok, const char* message) {
  ManualResult reply;
  reply.ok = ok;
  std::snprintf(reply.reply, sizeof(reply.reply), "%s", message);
  return reply;
}

// count received forecast rows from their bit mask.
unsigned rowCount(uint32_t mask) {
  unsigned count = 0;
  // accumulate set bits until none remain.
  while (mask) { count += mask & 1U; mask >>= 1; }
  return count;
}

// validate tenths of Celsius and supported weather codes.
bool weatherNumbers(Field temperature, Field condition, int32_t& tenths,
                    uint32_t& code) {
  return signedNumber(temperature, tenths) && tenths >= -1000 && tenths <= 700 &&
         unsignedNumber(condition, code) && validWeatherCode(code);
}
}  // namespace

// discard staged data, row tracking and deadlines.
void ManualSession::reset() {
  staged_ = WeatherSnapshot{};
  rowMask_ = baseRevision_ = startedMs_ = lastRowMs_ = 0;
  active_ = false;
}

// check idle/total staging limits with wrap-safe elapsed time.
bool ManualSession::expired(uint32_t nowMs) const {
  return active_ &&
         (static_cast<uint32_t>(nowMs - lastRowMs_) >= kManualStageIdleMs ||
          static_cast<uint32_t>(nowMs - startedMs_) >= kManualStageLifetimeMs);
}

// clear expired staging and report whether it changed.
bool ManualSession::expire(uint32_t nowMs) {
  // retain staging before either deadline.
  if (!expired(nowMs)) return false;
  reset();
  return true;
}

// validate one GATT command; only the watch task changes live/staged data.
ManualResult ManualSession::apply(const char* command, size_t length,
                                  WatchState& state, uint32_t nowMs) {
  // require a bounded, nonempty command.
  if (!command || !length || length > kMaxManualCommandBytes)
    return result(false, "ERR LENGTH");
  // split up to four nonempty printable ASCII fields.
  Field fields[4]{};
  size_t count = 0, start = 0;
  // include the end position to finish the last field.
  for (size_t i = 0; i <= length; ++i) {
    // validate bytes before checking delimiters.
    if (i < length) {
      const uint8_t value = static_cast<uint8_t>(command[i]);
      // reject control and non-ASCII bytes.
      if (value < 32 || value > 126) return result(false, "ERR ASCII");
      // continue within the current field.
      if (command[i] != ',') continue;
    }
    // reject missing or excess fields.
    if (i == start || count == 4) return result(false, "ERR FORMAT");
    fields[count++] = {command + start, i - start};
    start = i + 1;
  }
  // acknowledge connectivity without changing data.
  if (same(fields[0], "PING") && count == 1) return result(true, "OK PONG");
  // report time/weather validity and forecast staging progress.
  if (same(fields[0], "STATUS") && count == 1) {
    ManualResult reply = result(true, "");
    std::snprintf(reply.reply, sizeof(reply.reply), "T%u W%u H%u F%02u/24",
                  state.timeValid ? 1U : 0U, state.weather.valid ? 1U : 0U,
                  static_cast<unsigned>(state.weather.hourlyCount),
                  active_ && !expired(nowMs) ? rowCount(rowMask_) : 0U);
    return reply;
  }
  // cancel forecast staging without replacing live weather.
  if (same(fields[0], "CANCEL") && count == 1) {
    reset();
    return result(true, "OK CANCEL");
  }
  // change displayed temperature units.
  if (same(fields[0], "U") && count == 2) {
    // accept only Celsius/Fahrenheit selectors.
    if (!same(fields[1], "C") && !same(fields[1], "F"))
      return result(false, "ERR UNITS");
    state.units = same(fields[1], "C") ? TemperatureUnit::Celsius
                                      : TemperatureUnit::Fahrenheit;
    return result(true, "OK UNITS");
  }
  // replace current conditions with a one-hour validity window.
  if (same(fields[0], "W") && count == 3) {
    int32_t tenths = 0;
    uint32_t code = 0;
    // require valid temperature and condition values.
    if (!weatherNumbers(fields[1], fields[2], tenths, code))
      return result(false, "ERR RANGE");
    // require trusted time before timestamping conditions.
    if (!state.timeValid || !validUtc(state.utc))
      return result(false, "ERR TIME REQUIRED");
    // keep expiry within the supported UTC range.
    if (state.utc > kMaxUtc - 3600) return result(false, "ERR RANGE");
    // form fresh current conditions without inherited forecast rows.
    WeatherSnapshot next;
    next.valid = true;
    next.tempC = static_cast<float>(tenths) / 10.0f;
    next.code = static_cast<uint8_t>(code);
    next.observedAt = state.utc;
    next.validUntil = state.utc + 3600;
    next.receivedAtMs = nowMs;
    // commit conditions and invalidate previous forecast staging.
    state.weather = next;
    ++state.weatherRevision;
    reset();
    return result(true, "OK WEATHER");
  }
  // start a replacement 24-hour forecast from current conditions.
  if (same(fields[0], "F") && count == 2) {
    uint32_t first = 0;
    // bound the first and final hourly UTC timestamps.
    if (!unsignedNumber(fields[1], first) || !validUtc(first) ||
        first > kMaxUtc - (kForecastHours - 1) * 3600)
      return result(false, "ERR RANGE");
    // staging requires trusted time.
    if (!state.timeValid || !validUtc(state.utc))
      return result(false, "ERR TIME REQUIRED");
    // require base conditions for the new forecast.
    if (!state.weather.valid) return result(false, "ERR WEATHER REQUIRED");
    // start within one hour after the observation.
    if (first < state.weather.observedAt || first - state.weather.observedAt > 3600)
      return result(false, "ERR RANGE");
    // copy base conditions; clear daily/source metadata until SAVE.
    staged_ = state.weather;
    staged_.embedded = false;
    staged_.fetchedAt = 0;
    staged_.dailyCount = 0;
    // remove previous upload or relay daily summaries.
    for (size_t i = 0; i < kForecastDays; ++i) staged_.daily[i] = DayForecast{};
    staged_.hourlyCount = 0;
    // initialize consecutive hourly timestamps awaiting explicit row data.
    for (size_t i = 0; i < kForecastHours; ++i) {
      staged_.hourly[i] = HourForecast{};
      staged_.hourly[i].utc = first + static_cast<uint32_t>(i) * 3600;
    }
    // bind staging to this weather revision and its deadlines.
    rowMask_ = 0;
    baseRevision_ = state.weatherRevision;
    startedMs_ = lastRowMs_ = nowMs;
    active_ = true;
    return result(true, "OK FORECAST 00/24");
  }
  // validate and store one staged hourly row.
  if (same(fields[0], "H") && count == 4) {
    uint32_t index = 0, code = 0;
    int32_t tenths = 0;
    // require a valid row index, temperature and condition.
    if (!unsignedNumber(fields[1], index) || index >= kForecastHours ||
        !weatherNumbers(fields[2], fields[3], tenths, code))
      return result(false, "ERR RANGE");
    // reject rows without staging.
    if (!active_) return result(false, "ERR STAGE REQUIRED");
    // reject rows after either staging deadline.
    if (expired(nowMs)) return result(false, "ERR STAGE EXPIRED");
    // prevent mixing rows with replaced current conditions.
    if (baseRevision_ != state.weatherRevision)
      return result(false, "ERR WEATHER CHANGED");
    // mark the row received and renew the idle deadline.
    staged_.hourly[index].tempC = static_cast<float>(tenths) / 10.0f;
    staged_.hourly[index].code = static_cast<uint8_t>(code);
    rowMask_ |= 1UL << index;
    lastRowMs_ = nowMs;
    ManualResult reply = result(true, "");
    std::snprintf(reply.reply, sizeof(reply.reply), "OK HOUR %02u/24", rowCount(rowMask_));
    return reply;
  }
  // commit only a complete, unexpired forecast for the current conditions.
  if (same(fields[0], "SAVE") && count == 1) {
    // require staging before saving.
    if (!active_) return result(false, "ERR STAGE REQUIRED");
    // reject expired staging.
    if (expired(nowMs)) return result(false, "ERR STAGE EXPIRED");
    // reject staging based on replaced conditions.
    if (baseRevision_ != state.weatherRevision)
      return result(false, "ERR WEATHER CHANGED");
    // require all 24 explicitly supplied rows.
    if (rowMask_ != (1UL << kForecastHours) - 1)
      return result(false, "ERR HOURS MISSING");
    // retain the trusted-time requirement at commit.
    if (!state.timeValid || !validUtc(state.utc))
      return result(false, "ERR TIME REQUIRED");
    // reject invalid conditions or observations over five minutes in the future.
    if (!state.weather.valid ||
        static_cast<uint64_t>(staged_.observedAt) > static_cast<uint64_t>(state.utc) + 300)
      return result(false, "ERR TIME CHANGED");
    // replace live weather once, then discard staging.
    staged_.hourlyCount = kForecastHours;
    state.weather = staged_;
    ++state.weatherRevision;
    reset();
    return result(true, "OK FORECAST 24/24");
  }
  return result(false, "ERR COMMAND");
}
}  // namespace watch
