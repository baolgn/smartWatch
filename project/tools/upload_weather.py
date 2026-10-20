"""fetch/embed weather at upload using the Python standard library.

Open-Meteo data: https://open-meteo.com/ (CC BY 4.0).
this upload fallback also supplies normalization for the PC notification relay.
"""
from __future__ import annotations

from bisect import bisect_left
from collections import Counter
from datetime import datetime, time as daytime, timedelta, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import tempfile
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen
from zoneinfo import ZoneInfo

# fix the supported UTC range, location and response-size limit.
MIN_UTC, MAX_UTC = 946684800, 4102444799
LATITUDE, LONGITUDE = 40.7547, -73.9925
TIMEZONE = "America/New_York"
API_URL = "https://api.open-meteo.com/v1/forecast"
MAX_BYTES = 1024 * 1024
# map WMO conditions to the watch's icon groups.
ICON_GROUPS = {
    0: 0, 1: 0, 2: 2, 3: 3, 45: 45, 48: 45,
    51: 51, 53: 51, 55: 51, 56: 66, 57: 66, 66: 66, 67: 66,
    61: 61, 63: 61, 65: 61, 80: 61, 81: 61, 82: 61,
    71: 71, 73: 71, 75: 71, 77: 71, 85: 71, 86: 71,
    95: 95, 96: 95, 99: 95,
}


# report provider, timezone, cache and generation errors uniformly.
class WeatherError(ValueError):
    """weather fetch, snapshot, timezone, cache or generation failure."""


# enforce a validation rule with a user-facing failure message.
def require(condition, message):
    # abort normalization when the required condition fails.
    if not condition:
        raise WeatherError(message)


# accept integer UTC seconds within the RTC calendar range.
def valid_utc(value):
    return type(value) is int and MIN_UTC <= value <= MAX_UTC


# accept finite numeric temperatures within the supported Celsius range.
def valid_temperature(value):
    return type(value) in (int, float) and math.isfinite(value) and -100 <= value <= 70


# accept only integer WMO codes with a known icon mapping.
def valid_code(value):
    return type(value) is int and value in ICON_GROUPS


# load the bundled New York timezone independently of the host timezone.
def new_york():
    # read the bundled timezone rules or report a deployment error.
    try:
        # close the TZif file after constructing the timezone object.
        with (Path(__file__).parent / "data" / "america_new_york.tzif").open("rb") as source:
            return ZoneInfo.from_file(source, key=TIMEZONE)
    # translate missing or invalid timezone data into a weather error.
    except (OSError, ValueError) as error:
        raise WeatherError("Bundled America/New_York timezone data is unavailable") from error


# resolve the UTC offset at a particular instant.
def utc_offset(stamp, zone):
    return int(datetime.fromtimestamp(stamp, zone).utcoffset().total_seconds())


# locate the next timezone transition with one-second precision.
def next_transition(stamp, zone):
    """find the next offset change to the UTC second within 370 days."""
    before, offset = stamp, utc_offset(stamp, zone)
    limit = min(MAX_UTC, stamp + 370 * 86400)
    # scan at most 370 days without exceeding the RTC range.
    while before < limit:
        after = min(before + 86400, limit)
        # refine the first interval containing an offset change.
        if utc_offset(after, zone) != offset:
            left, right = before, after
            # bisect until the transition is isolated to one second.
            while right - left > 1:
                middle = (left + right) // 2
                # advance the boundary that still uses the old offset.
                if utc_offset(middle, zone) == offset:
                    left = middle
                # retain the boundary using the new offset.
                else:
                    right = middle
            return right, utc_offset(right, zone)
        before = after
    return 0, offset


# convert a local date boundary to UTC, accounting for DST.
def midnight(day, zone):
    return int(datetime.combine(day, daytime(), tzinfo=zone).timestamp())


# parse JSON while rejecting duplicate fields and non-finite numbers.
def strict_json(raw):
    # reject JSON NaN and infinity tokens.
    def invalid_number(value):
        raise WeatherError("Non-finite JSON number")

    # construct an object only when every field name is unique.
    def unique_fields(pairs):
        result = {}
        # validate each field before inserting it into the object.
        for key, value in pairs:
            require(key not in result, "Duplicate JSON field")
            result[key] = value
        return result

    # decode the payload with strict object and number hooks.
    try:
        return json.loads(raw, parse_constant=invalid_number, object_pairs_hook=unique_fields)
    # translate malformed encoding, syntax or nesting into a weather error.
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise WeatherError("Malformed weather JSON") from error


# validate provider data and derive the fixed hourly/daily upload snapshot.
def normalize(payload, fetched_at):
    # validate response identity, fetch time and the provider timezone.
    require(valid_utc(fetched_at), "Invalid fetch timestamp")
    require(type(payload) is dict and not payload.get("error"), "Invalid provider response")
    require(payload.get("timezone") == TIMEZONE, "Provider timezone is not America/New_York")
    api_offset = payload.get("utc_offset_seconds")
    require(type(api_offset) is int and api_offset in (-18000, -14400), "Invalid provider UTC offset")
    # require Unix timestamps, Celsius temperatures and WMO conditions.
    for name in ("current_units", "hourly_units"):
        units = payload.get(name)
        require(type(units) is dict and units.get("time") == "unixtime"
                and units.get("temperature_2m") == "\u00b0C"
                and units.get("weather_code") == "wmo code", "Expected Unix/Celsius/WMO units")
    units = payload.get("daily_units")
    # validate daily temperature units independently of hourly units.
    require(type(units) is dict and units.get("time") == "unixtime"
            and units.get("temperature_2m_min") == "\u00b0C"
            and units.get("temperature_2m_max") == "\u00b0C", "Invalid daily units")
    current, hourly, daily = (payload.get(name) for name in ("current", "hourly", "daily"))
    require(all(type(value) is dict for value in (current, hourly, daily)), "Missing weather sections")
    observed = current.get("time")
    # require fresh observations within the RTC range.
    require(valid_utc(observed) and observed <= fetched_at + 300 and observed + 3600 <= MAX_UTC
            and fetched_at < observed + 3600, "Current weather is stale or has an invalid timestamp")
    require(valid_temperature(current.get("temperature_2m")) and valid_code(current.get("weather_code")),
            "Invalid current weather values")
    # require bounded, equally sized hourly arrays.
    times, temps, codes = (hourly.get(key) for key in ("time", "temperature_2m", "weather_code"))
    require(all(type(values) is list for values in (times, temps, codes)), "Missing hourly arrays")
    require(24 <= len(times) == len(temps) == len(codes) <= 400, "Invalid hourly array lengths")
    rows = []
    # validate consecutive hourly timestamps and corresponding weather values.
    for index, (stamp, temp, code) in enumerate(zip(times, temps, codes)):
        require(valid_utc(stamp) and valid_temperature(temp) and valid_code(code), "Invalid hourly record")
        require(index == 0 or stamp == times[index - 1] + 3600, "Hourly timestamps must be consecutive")
        rows.append({"utc": stamp, "tempC": float(temp), "code": code})
    # locate 24 future hours in the validated sorted timestamps.
    first = bisect_left(times, max(fetched_at, observed))
    require(len(rows) - first >= 24, "Fewer than 24 future forecast hours")
    require(times[first] <= observed + 3600, "Current timestamp and hourly forecast disagree")

    # resolve today in New York and validate the daily arrays.
    zone = new_york()
    today = datetime.fromtimestamp(fetched_at, zone).date()
    dates, lows, highs = (daily.get(key) for key in ("time", "temperature_2m_min", "temperature_2m_max"))
    require(all(type(values) is list for values in (dates, lows, highs)), "Missing daily arrays")
    require(7 <= len(dates) == len(lows) == len(highs) <= 17, "Invalid daily array lengths")
    by_day = {}
    previous_day = None
    # index consecutive daily low/high temperatures by local date.
    for stamp, low, high in zip(dates, lows, highs):
        require(valid_utc(stamp) and valid_temperature(low) and valid_temperature(high)
                and low <= high, "Invalid daily min/max or timestamp")
        actual = datetime.fromtimestamp(stamp, zone)
        fixed = datetime.fromtimestamp(stamp + api_offset, timezone.utc)
        # accept API-offset labels or local midnights; DST days can differ from 86400 s.
        label = actual if actual.time() == daytime() else fixed
        require(label.time() == daytime(), "Daily timestamp is not a midnight date label")
        day = label.date()
        require(previous_day is None or day == previous_day + timedelta(days=1),
                "Daily dates must be unique and consecutive")
        by_day[day] = (float(low), float(high))
        previous_day = day
    daily_rows = []
    # derive seven daily summaries from complete local-day hourly coverage.
    for day_index in range(7):
        day = today + timedelta(days=day_index)
        require(day in by_day, "Missing one of today's seven daily forecasts")
        start, end = midnight(day, zone), midnight(day + timedelta(days=1), zone)
        require(valid_utc(start) and valid_utc(end), "Daily range outside supported RTC dates")
        # slice the sorted rows between the two local-midnight boundaries.
        day_rows = rows[bisect_left(times, start):bisect_left(times, end)]
        # require every hour of each 23-, 24- or 25-hour local day.
        expected = list(range(start, end, 3600))
        require(len(expected) in (23, 24, 25) and [row["utc"] for row in day_rows] == expected,
                "Incomplete hourly coverage for daily icon calculation")
        groups = [ICON_GROUPS[row["code"]] for row in day_rows]
        counts = Counter(groups)
        # max breaks frequency ties with the day's first occurrence.
        icon = max(groups, key=counts.get)
        low, high = by_day[day]
        daily_rows.append({"utc": start, "nextUtc": end, "minTempC": low,
                           "maxTempC": high, "code": icon})
    # attach the next DST transition and preserve provider/fetch timestamps.
    transition, next_offset = next_transition(fetched_at, zone)
    return {"generatedAt": fetched_at, "offsetSeconds": utc_offset(fetched_at, zone),
            "nextTransitionUtc": transition, "nextOffsetSeconds": next_offset,
            "observedAt": observed, "validUntil": observed + 3600,
            "tempC": float(current["temperature_2m"]), "code": current["weather_code"],
            "hourly": rows[first:first + 24], "daily": daily_rows}


# fetch a bounded Open-Meteo response and normalize it before embedding.
def fetch_weather():
    # request current conditions, hourly data and daily temperature bounds.
    query = urlencode({"latitude": LATITUDE, "longitude": LONGITUDE, "timezone": TIMEZONE,
                       "current": "temperature_2m,weather_code", "hourly": "temperature_2m,weather_code",
                       "daily": "temperature_2m_min,temperature_2m_max", "forecast_days": 8,
                       "past_days": 1, "temperature_unit": "celsius", "timeformat": "unixtime"})
    request = Request(API_URL + "?" + query, headers={"User-Agent": "XIAO-Watch-Upload/1"})
    # apply a network timeout and limit the downloaded response size.
    try:
        # close the HTTP response after reading the bounded payload.
        with urlopen(request, timeout=15) as response:
            require(response.status == 200, "Weather provider returned an unexpected HTTP status")
            raw = response.read(MAX_BYTES + 1)
    # cancel the upload when a fresh fetch fails.
    except (HTTPError, URLError, OSError) as error:
        raise WeatherError("Cannot fetch fresh Open-Meteo weather; upload cancelled") from error
    require(len(raw) <= MAX_BYTES, "Weather response exceeds size limit")
    fetched = int(time.time())
    payload = strict_json(raw)
    normalized = normalize(payload, fetched)
    return payload, normalized


# serialize deterministic ASCII JSON for cache hashing.
def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False).encode("ascii")


# replace files atomically without rewriting identical contents.
def atomic_write(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    # leave an identical file untouched to avoid unnecessary rebuilds.
    if path.exists() and path.read_bytes() == data:
        return
    temporary = None
    # write a neighboring temporary file before atomic replacement.
    try:
        # close the temporary file before replacing the destination.
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + ".", suffix=".tmp", delete=False) as target:
            temporary = Path(target.name)
            target.write(data)
        os.replace(temporary, path)
    # clean up any temporary file left by a failed replacement.
    finally:
        # delete only the temporary file created by this write.
        if temporary is not None and temporary.exists():
            temporary.unlink()


# cache the original provider response with fetch time and integrity hash.
def save_cache(path, payload, snapshot):
    body = {"schema": 1, "fetchedAt": snapshot["generatedAt"], "response": payload}
    body["sha256"] = hashlib.sha256(canonical(body)).hexdigest()
    atomic_write(path, canonical(body) + b"\n")


# validate cached data and rebuild its normalized snapshot.
def load_cache(path):
    path = Path(path)
    # allow offline builds with no prior weather snapshot.
    if not path.exists():
        return None
    require(path.stat().st_size <= MAX_BYTES, "Weather cache exceeds size limit")
    body = strict_json(path.read_bytes())
    require(type(body) is dict and set(body) == {"schema", "fetchedAt", "response", "sha256"}
            and type(body["schema"]) is int and body["schema"] == 1, "Unsupported weather cache")
    digest = body.pop("sha256")
    require(type(digest) is str and digest == hashlib.sha256(canonical(body)).hexdigest(), "Weather cache checksum mismatch")
    return normalize(body["response"], body["fetchedAt"])


# emit the C++ snapshot loader and its documentation.
def render_header(snapshot):
    lines = ["/* generated by tools/upload_weather.py; do not edit.",
             " * weather: https://open-meteo.com/ ; CC BY 4.0.",
             " * daily icon: most frequent hourly icon group; earliest breaks ties. */",
             "#pragma once", '#include "watch_model.h"', "// embedded upload snapshot and loader.\nnamespace uploadweather {"]
    lines.append("// preserve the snapshot's availability, UTC and display offsets.\nconstexpr bool available = %s;" % ("true" if snapshot else "false"))
    # emit timestamp/offset constants, using zero for an unavailable snapshot.
    for field, kind in (("generatedAt", "uint32_t"), ("offsetSeconds", "int32_t"),
                        ("nextTransitionUtc", "uint32_t"), ("nextOffsetSeconds", "int32_t")):
        lines.append("constexpr %s %s = %d;" % (kind, field, snapshot[field] if snapshot else 0))
    lines.append("// restore weather without resetting its original fetch time.\ninline void load(watch::WatchState& state, uint32_t nowMs) {")
    # emit weather assignments only when validated data exists.
    if snapshot:
        lines.extend(["  // replace weather with the validated upload snapshot.\n  state.weather = watch::WeatherSnapshot{};", "  auto& weather = state.weather;",
                      "  weather.valid = true;", "  weather.embedded = true;",
                      "  weather.fetchedAt = generatedAt;", "  weather.receivedAtMs = nowMs;"])
        lines.append("  // restore observed conditions and their validity window.")
        # emit observation validity and condition fields.
        for field in ("observedAt", "validUntil", "code"):
            lines.append("  weather.%s = %d;" % (field, snapshot[field]))
        lines.append("  weather.tempC = %sf;" % repr(snapshot["tempC"]))
        lines.append("  // restore all 24 hourly forecasts.\n  weather.hourlyCount = 24;")
        # emit all 24 hourly forecast records.
        for i, row in enumerate(snapshot["hourly"]):
            lines.extend(["  weather.hourly[%d].utc = %d;" % (i, row["utc"]),
                          "  weather.hourly[%d].tempC = %sf;" % (i, repr(row["tempC"])),
                          "  weather.hourly[%d].code = %d;" % (i, row["code"])])
        lines.append("  // restore seven local days and their most common icons.\n  weather.dailyCount = 7;")
        # emit all seven local-day summaries.
        for i, row in enumerate(snapshot["daily"]):
            # emit each daily UTC boundary and modal icon code.
            for field in ("utc", "nextUtc", "code"):
                lines.append("  weather.daily[%d].%s = %d;" % (i, field, row[field]))
            # emit daily minimum/maximum temperatures as C++ floats.
            for field in ("minTempC", "maxTempC"):
                lines.append("  weather.daily[%d].%s = %sf;" % (i, field, repr(row[field])))
        lines.append("  // notify the UI that weather changed.\n  ++state.weatherRevision;")
    # emit an inert loader when no snapshot is available.
    else:
        lines.append("  // no embedded snapshot; preserve existing weather.\n  (void)state; (void)nowMs;")
    lines.extend(["}", "}  // namespace uploadweather", ""])
    return "\n".join(lines).encode("ascii")


# require fresh upload data while allowing cached or empty offline builds.
def prepare(project_dir, build_dir, targets, fetcher=fetch_weather, report=print):
    targets = set(targets)
    uploading = "upload" in targets
    require(not (uploading and targets.intersection({"nobuild", "clean"})),
            "Weather uploads require a fresh build; run clean separately and omit nobuild")
    # skip generation during a standalone clean.
    if "clean" in targets:
        return None
    cache = Path(project_dir) / ".pio" / "weather" / "last_upload.json"
    header = Path(build_dir) / "generated" / "upload_weather_data.h"
    # fetch and revalidate weather before every normal upload.
    if uploading:
        report("Fetching fresh ZIP 10018 weather before compilation...")
        payload, snapshot = fetcher()
        # revalidate the full response, including injected fetchers.
        snapshot = normalize(payload, snapshot["generatedAt"])
        save_cache(cache, payload, snapshot)
        report("Embedding current weather, next 24 hours and seven local days from Open-Meteo.")
    # use the dated cache only for a non-upload build.
    else:
        # load and validate the previous upload cache.
        try:
            snapshot = load_cache(cache)
        # fall back to unavailable weather if the cache is invalid.
        except (WeatherError, OSError) as error:
            report("Weather cache is invalid; building without embedded weather: %s" % error)
            snapshot = None
        # report cached data without changing its original timestamp.
        if snapshot:
            report("Offline build: using the last upload snapshot; its original timestamp is preserved.")
        # report that weather remains unavailable until a fresh upload.
        else:
            report("Offline build: no upload snapshot; weather will be unavailable until an online upload.")
    atomic_write(header, render_header(snapshot))
    return header
