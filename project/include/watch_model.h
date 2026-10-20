#pragma once

#include <stddef.h>
#include <stdint.h>

// watch state and hardware-independent data helpers.
namespace watch {

// RTC range and stored forecast capacities.
constexpr uint32_t kMinUtc = 946684800UL;   // 2000-01-01; RTC lower limit
constexpr uint32_t kMaxUtc = 4102444799UL;  // 2099-12-31 23:59:59
constexpr size_t kForecastHours = 24;
constexpr size_t kForecastDays = 7;
// generic diagnostic transport limits; notification envelopes have a smaller cap.
constexpr size_t kRelayTopicBytes = 32;
constexpr size_t kGenericPayloadBytes = 1024;

// supported display units; stored weather remains Celsius.
enum class TemperatureUnit : uint8_t { Celsius, Fahrenheit };

// cached generic payloads retain their original representation.
enum class PayloadEncoding : uint8_t { Json, Binary };

// one hourly prediction with its UTC interval start.
struct HourForecast {
  uint32_t utc = 0;
  float tempC = 0;
  uint8_t code = 0;
};

// one local day with extrema and its most frequent icon.
struct DayForecast {
  uint32_t utc = 0;      // local midnight in UTC
  uint32_t nextUtc = 0;  // next local midnight; DST days may span 23/25 hours
  float minTempC = 0;
  float maxTempC = 0;
  uint8_t code = 0;      // most frequent hourly icon group that day
};

// dated current conditions and bounded forecast arrays.
struct WeatherSnapshot {
  bool valid = false;
  bool embedded = false;
  uint32_t fetchedAt = 0;
  float tempC = 0;
  uint8_t code = 0;
  uint32_t observedAt = 0;
  uint32_t validUntil = 0;
  uint32_t receivedAtMs = 0;
  uint8_t hourlyCount = 0;  // current-only data has no forecast
  HourForecast hourly[kForecastHours]{};
  uint8_t dailyCount = 0;
  DayForecast daily[kForecastDays]{};
};

// distinguish observed, predicted, dated and missing data.
enum class WeatherDisplay : uint8_t { Missing, Observed, Forecast, Snapshot };

// watch-task state shared read-only with the UI.
struct WatchState {
  bool displayDimmed = false;  // wake uses only screen-normal Z
  // local clock estimate and timezone transition, independent of display mode.
  bool timeValid = false;
  uint32_t utc = 0;
  int32_t offsetSeconds = 0;
  bool offsetKnown = false;
  uint32_t nextTransitionUtc = 0;  // optional next DST/timezone transition
  int32_t nextOffsetSeconds = 0;
  TemperatureUnit units = TemperatureUnit::Celsius;
  // transport/RTC status and watch battery estimate.
  bool bleConnected = false;
  bool rtcAvailable = false;
  bool rtcFault = false;
  int16_t batteryPercent = -1;  // -1 means unknown; 0 means empty
  float batteryVoltage = 0;
  // weather revision triggers UI refresh; clock ticks drive interpolation.
  WeatherSnapshot weather{};
  uint32_t weatherRevision = 0;
  uint32_t clockTickMs = 0;
  // track applied relay envelopes independently of weather and RTC state.
  uint32_t relayRevision = 0;
  char relayTopic[kRelayTopicBytes + 1]{};
  uint32_t relaySequence = 0;
  uint32_t relayReceivedAtMs = 0;
  // retain one unhandled topic as bounded JSON or typed binary until reboot.
  char genericTopic[kRelayTopicBytes + 1]{};
  uint32_t genericSequence = 0;
  uint16_t genericPayloadBytes = 0;
  PayloadEncoding genericEncoding = PayloadEncoding::Json;
  char genericPayload[kGenericPayloadBytes + 1]{};
};

// broken-down local date/time; zeroed when unavailable.
struct DateParts {
  int16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t weekday = 0;  // Sunday = 0
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
};

// check the RTC-supported UTC range.
bool validUtc(uint32_t utc);
// check supported timezone offsets.
bool validOffset(int32_t seconds);
// accept supported WMO weather codes.
bool validWeatherCode(uint32_t code);
/* RTC is authoritative; failures retain a ticking local estimate until a valid
 * sample restores it. timezone settings remain separate. */
void applyRtcSample(WatchState& state, bool valid, uint32_t utc, uint32_t nowMs);
// advance the local estimate between RTC samples.
void advanceClock(WatchState& state, uint32_t nowMs);
// convert UTC and an explicit offset to calendar fields.
DateParts dateTimeAt(uint32_t utc, int32_t offsetSeconds);
// expose dateTimeAt() to forecast rendering.
DateParts dateParts(uint32_t utc, int32_t offsetSeconds);
// apply the recorded offset transition at its UTC boundary.
int32_t offsetAt(const WatchState& state, uint32_t utc);
// resolve the watch clock to local fields when valid.
DateParts localDateTime(const WatchState& state);
// return a three-letter weekday or an unavailable marker.
const char* weekdayName(uint8_t weekday);
// return a three-letter month or an unavailable marker.
const char* monthName(uint8_t month);
// map a WMO code to a readable condition.
const char* weatherDescription(uint8_t code);
// reject missing/expired data, unset clocks and observations >5 min ahead.
bool weatherIsStale(const WatchState& state);
// report whether observed conditions remain current.
bool weatherFresh(const WatchState& state);
// select current data, an upload snapshot or an in-range prediction.
WeatherDisplay weatherForDisplay(const WatchState& state, float& tempC, uint8_t& code);
// convert stored Celsius to the configured display units.
float temperatureForDisplay(float tempC, TemperatureUnit units);
// estimate resting LiPo charge; return -1 for invalid voltage.
int16_t batteryPercentFromVoltage(float voltage);
// select unknown, low, medium or high battery color.
uint32_t batteryColor(int16_t percent);
// select the Celsius-based temperature color.
uint32_t temperatureColor(float tempC);

}  // namespace watch
