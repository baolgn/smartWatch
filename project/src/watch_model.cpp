#include "watch_model.h"

#include <math.h>

// watch state, calendar arithmetic and display data selection.
namespace watch {

// accept only the RTC calendar range.
bool validUtc(uint32_t utc) { return utc >= kMinUtc && utc <= kMaxUtc; }

// bound timezone offsets to UTC-12 through UTC+14.
bool validOffset(int32_t seconds) {
  return seconds >= -12 * 3600 && seconds <= 14 * 3600;
}

// validate supported WMO condition codes.
bool validWeatherCode(uint32_t code) {
  // recognize forecast-provider condition codes.
  switch (code) {
    // accept clear/cloud/fog and supported precipitation/storm codes.
    case 0: case 1: case 2: case 3: case 45: case 48:
    case 51: case 53: case 55: case 56: case 57:
    case 61: case 63: case 65: case 66: case 67:
    case 71: case 73: case 75: case 77:
    case 80: case 81: case 82: case 85: case 86:
    case 95: case 96: case 99:
      return true;
    // reject unspecified condition codes.
    default:
      return false;
  }
}

// accept authoritative RTC samples without changing timezone settings.
void applyRtcSample(WatchState& state, bool valid, uint32_t utc, uint32_t nowMs) {
  state.rtcFault = !valid || !validUtc(utc);
  // retain the local clock estimate on invalid RTC reads.
  if (state.rtcFault) return;
  state.utc = utc;
  state.timeValid = true;
  state.clockTickMs = nowMs;
}

// interpolate whole seconds between RTC reads.
void advanceClock(WatchState& state, uint32_t nowMs) {
  // avoid elapsed-time accumulation before clock provisioning.
  if (!state.timeValid) {
    state.clockTickMs = nowMs;
    return;
  }
  // unsigned subtraction handles millis() wrap if serviced every <49.7 days.
  const uint32_t seconds = (nowMs - state.clockTickMs) / 1000;
  // invalidate the clock before exceeding the RTC range.
  if (seconds > kMaxUtc - state.utc) {
    state.timeValid = false;
    state.clockTickMs = nowMs;
    return;
  }
  state.utc += seconds;
  state.clockTickMs += seconds * 1000;
}

// split offset-adjusted UTC; share one conversion body across clock/forecast wrappers.
__attribute__((noinline)) DateParts dateTimeAt(uint32_t utc, int32_t offsetSeconds) {
  DateParts date;
  // return zeroed fields for invalid time or timezone.
  if (!validUtc(utc) || !validOffset(offsetSeconds)) return date;
  // validated UTC/offset bounds keep local seconds within uint32_t.
  const uint32_t local = static_cast<uint32_t>(static_cast<int64_t>(utc) + offsetSeconds);
  const uint32_t epochDays = local / 86400;
  const uint32_t daySeconds = local % 86400;
  date.hour = daySeconds / 3600;
  date.minute = (daySeconds % 3600) / 60;
  date.second = daySeconds % 60;
  date.weekday = static_cast<uint8_t>((epochDays + 4) % 7);

  // Gregorian intermediates also fit 32 bits; avoid costly 64-bit division.
  const uint32_t shifted = epochDays + 719468;
  const uint32_t era = shifted / 146097;
  const uint32_t dayOfEra = shifted - era * 146097;
  const uint32_t yearOfEra =
      (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
  int32_t year = static_cast<int32_t>(yearOfEra + era * 400);
  const uint32_t dayOfYear =
      dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
  const uint32_t monthMarch = (5 * dayOfYear + 2) / 153;
  date.day = static_cast<uint8_t>(dayOfYear - (153 * monthMarch + 2) / 5 + 1);
  date.month = static_cast<uint8_t>(monthMarch < 10 ? monthMarch + 3 : monthMarch - 9);
  year += date.month <= 2;
  date.year = static_cast<int16_t>(year);
  return date;
}

// use the watch clock and its active timezone offset.
DateParts localDateTime(const WatchState& state) {
  return state.timeValid ? dateTimeAt(state.utc, offsetAt(state, state.utc)) : DateParts{};
}

// delegate forecast date conversion to dateTimeAt().
DateParts dateParts(uint32_t utc, int32_t offsetSeconds) {
  return dateTimeAt(utc, offsetSeconds);
}

// select the offset on either side of the stored transition.
int32_t offsetAt(const WatchState& state, uint32_t utc) {
  return state.nextTransitionUtc != 0 && utc >= state.nextTransitionUtc
             ? state.nextOffsetSeconds : state.offsetSeconds;
}

// map Sunday-based weekday indices to short names.
const char* weekdayName(uint8_t weekday) {
  static const char* const names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  return weekday < 7 ? names[weekday] : "---";
}

// map one-based month indices to short names.
const char* monthName(uint8_t month) {
  static const char* const names[] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  return month >= 1 && month <= 12 ? names[month - 1] : "---";
}

// describe supported WMO codes for the Weather app.
const char* weatherDescription(uint8_t code) {
  // group codes by displayed condition.
  switch (code) {
    // clear sky.
    case 0: return "Clear";
    // mainly clear sky.
    case 1: return "Mainly clear";
    // partial cloud cover.
    case 2: return "Partly cloudy";
    // overcast sky.
    case 3: return "Overcast";
    // fog, including rime deposition.
    case 45: case 48: return "Fog";
    // drizzle at any supported intensity.
    case 51: case 53: case 55: return "Drizzle";
    // freezing drizzle.
    case 56: case 57: return "Freezing drizzle";
    // rain at any supported intensity.
    case 61: case 63: case 65: return "Rain";
    // freezing rain.
    case 66: case 67: return "Freezing rain";
    // snowfall or snow grains.
    case 71: case 73: case 75: case 77: return "Snow";
    // rain showers.
    case 80: case 81: case 82: return "Rain showers";
    // snow showers.
    case 85: case 86: return "Snow showers";
    // thunderstorm without reported hail.
    case 95: return "Thunderstorm";
    // thunderstorm with hail.
    case 96: case 99: return "Storm with hail";
    // unrecognized conditions.
    default: return "Unknown";
  }
}

// reject missing/expired weather and observations >5 min ahead of the clock.
bool weatherIsStale(const WatchState& state) {
  return !state.weather.valid || !state.timeValid ||
         state.utc >= state.weather.validUntil ||
         static_cast<uint64_t>(state.weather.observedAt) >
             static_cast<uint64_t>(state.utc) + 300;
}

// invert the observed-data freshness check.
bool weatherFresh(const WatchState& state) { return !weatherIsStale(state); }

// choose observed weather, a dated upload snapshot or an in-range forecast.
WeatherDisplay weatherForDisplay(const WatchState& state, float& tempC, uint8_t& code) {
  const WeatherSnapshot& weather = state.weather;
  // stop when no valid snapshot exists.
  if (!weather.valid) return WeatherDisplay::Missing;
  // validate observed temperature and condition before display.
  const bool validCurrent = isfinite(weather.tempC) && weather.tempC >= -100.0f &&
                            weather.tempC <= 70.0f && validWeatherCode(weather.code);
  // use current observations or identify clockless embedded data as a snapshot.
  if (validCurrent && (weatherFresh(state) || (weather.embedded && !state.timeValid))) {
    tempC = weather.tempC;
    code = weather.code;
    /* without a clock, label the dated snapshot; never claim current data
     * or reset the RTC to an old build time. */
    return state.timeValid ? WeatherDisplay::Observed : WeatherDisplay::Snapshot;
  }
  // require time before selecting an hourly prediction.
  if (!state.timeValid) return WeatherDisplay::Missing;
  const size_t count = weather.hourlyCount < kForecastHours ? weather.hourlyCount : kForecastHours;
  // search only the stored hourly records.
  for (size_t i = 0; i < count; ++i) {
    const HourForecast& hour = weather.hourly[i];
    // accept a valid prediction only during its one-hour interval.
    if (validUtc(hour.utc) && state.utc >= hour.utc &&
        static_cast<uint64_t>(state.utc) < static_cast<uint64_t>(hour.utc) + 3600 &&
        isfinite(hour.tempC) && hour.tempC >= -100.0f && hour.tempC <= 70.0f &&
        validWeatherCode(hour.code)) {
      tempC = hour.tempC;
      code = hour.code;
      return WeatherDisplay::Forecast;
    }
  }
  return WeatherDisplay::Missing;
}

// convert Celsius only when Fahrenheit is selected.
float temperatureForDisplay(float tempC, TemperatureUnit units) {
  return units == TemperatureUnit::Fahrenheit ? tempC * 1.8f + 32.0f : tempC;
}

// estimate charge from resting LiPo voltage, without fuel-gauge accuracy.
int16_t batteryPercentFromVoltage(float voltage) {
  // reject implausible voltage; verify the ADC/divider and battery path before use.
  if (!isfinite(voltage) || voltage < 2.5f || voltage > 4.4f) return -1;
  // piecewise voltage/charge points for a resting single-cell LiPo.
  static const float volts[] = {3.30f, 3.50f, 3.60f, 3.70f, 3.75f, 3.80f,
                                3.85f, 3.90f, 4.00f, 4.10f, 4.20f};
  static const int16_t percent[] = {0, 5, 10, 20, 30, 40, 50, 60, 75, 90, 100};
  // clamp voltages below the curve to empty.
  if (voltage <= volts[0]) return 0;
  // find the voltage segment enclosing the reading.
  for (size_t i = 1; i < sizeof(volts) / sizeof(volts[0]); ++i) {
    // interpolate and round within the selected segment.
    if (voltage <= volts[i]) {
      const float fraction = (voltage - volts[i - 1]) / (volts[i] - volts[i - 1]);
      return static_cast<int16_t>(percent[i - 1] +
             fraction * (percent[i] - percent[i - 1]) + 0.5f);
    }
  }
  return 100;
}

// map charge ranges to battery icon colors.
uint32_t batteryColor(int16_t percent) {
  // use gray when charge is unknown.
  if (percent < 0) return 0x88929E;
  // mark low charge red.
  if (percent <= 20) return 0xFF5A68;
  // mark intermediate charge amber.
  if (percent <= 50) return 0xFFC857;
  return 0x5EE3A0;
}

// map Celsius thresholds to weather text colors.
uint32_t temperatureColor(float tempC) {
  // use gray when temperature is unavailable.
  if (!isfinite(tempC)) return 0x88929E;
  // use blue for cold conditions.
  if (tempC < 10.0f) return 0x66BFFF;
  // use green for mild conditions.
  if (tempC < 25.0f) return 0x5EE3A0;
  // use amber for warm conditions.
  if (tempC < 32.0f) return 0xFFC857;
  return 0xFF5A68;
}

}  // namespace watch
