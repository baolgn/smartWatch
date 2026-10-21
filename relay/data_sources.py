"""fetch bounded JSON sources and encode validated XWR snapshots."""
from __future__ import annotations

import base64
import binascii
import importlib.util
import json
import math
from pathlib import Path
import re
import struct
import sys
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

from binary_protocol import ValueCodecError, decode_text, encode_text

# bound source parsing independently of the encoded XWR frame.
TOPIC_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9._/-]{0,31}\Z", re.ASCII)
MAX_INPUT = 256 * 1024
MAX_SAFE_INTEGER = 9007199254740991
MIN_UTC, MAX_UTC = 946684800, 4102444799
NETWORK_TIMEOUT = 15
WEATHER_HEADER = struct.Struct("<BIhIhIIII")


# expose failures without URLs, paths or payload contents.
class SourceError(ValueError):
    pass


# reject redirects instead of changing the source origin.
class NoRedirect(HTTPRedirectHandler):
    # make every redirect a handled HTTP failure.
    def redirect_request(self, request, response, code, message, headers, url):
        return None


# enforce a protocol or transport condition.
def require(condition, message):
    # stop before encoding invalid or oversized data.
    if not condition:
        raise SourceError(message)


# validate decoded values before deterministic ASCII encoding.
def validate_value(value, depth=0):
    # count each nested object and array against the source limit.
    if type(value) in (dict, list):
        require(depth < 8, "JSON exceeds eight container levels")
        # limit object keys before traversing their contents.
        if type(value) is dict:
            require(len(value) <= 32, "JSON object exceeds 32 keys")
            # require scalar Unicode keys and validate each value.
            for key, item in value.items():
                require(type(key) is str, "JSON keys must be strings")
                validate_value(key, depth + 1)
                validate_value(item, depth + 1)
        # inspect each array element without changing its order.
        else:
            # bound traversal before the encoded frame size check.
            require(len(value) <= 4096, "JSON array exceeds 4,096 entries")
            # validate every array value recursively.
            for item in value:
                validate_value(item, depth + 1)
    # reject isolated surrogate code units, including escaped input.
    elif type(value) is str:
        require(not any(0xD800 <= ord(char) <= 0xDFFF for char in value),
                "JSON contains an unpaired Unicode surrogate")
    # preserve integer precision across JSON implementations.
    elif type(value) is int:
        require(abs(value) <= MAX_SAFE_INTEGER, "Large integer identifiers must be strings")
    # reject numeric overflow and nonstandard floating-point values.
    elif type(value) is float:
        require(math.isfinite(value), "JSON numbers must be finite")
    # accept the remaining JSON scalar types only.
    else:
        require(value is None or type(value) is bool, "Unsupported JSON value")


# parse UTF-8 JSON without silently collapsing duplicate decoded keys.
def parse_json(raw):
    require(type(raw) is bytes and len(raw) <= MAX_INPUT, "JSON input exceeds 256 KiB")

    # reject duplicate names after JSON escape decoding.
    def unique_fields(pairs):
        result = {}
        # retain every distinct key exactly once.
        for key, value in pairs:
            require(key not in result, "JSON contains duplicate keys")
            result[key] = value
        return result

    # reject Python's optional nonstandard JSON number tokens.
    def invalid_constant(_value):
        raise SourceError("JSON numbers must be finite")

    # translate parser and encoding failures without exposing source data.
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_fields,
                           parse_constant=invalid_constant)
        validate_value(value)
        return value
    # deeply nested or excessively long numeric inputs are invalid too.
    except (UnicodeError, json.JSONDecodeError, RecursionError, OverflowError) as error:
        raise SourceError("Malformed UTF-8 JSON") from error
    # keep deliberate validation messages but hide Python's source excerpts.
    except ValueError as error:
        # preserve errors already sanitized by this module.
        if isinstance(error, SourceError):
            raise
        raise SourceError("Malformed JSON number") from error


# require HTTPS without embedded credentials or fragments.
def validate_url(url):
    # report invalid URL syntax without echoing potentially private queries.
    try:
        parts = urlsplit(url)
        require(parts.scheme == "https" and bool(parts.hostname) and not parts.username
                and not parts.password and not parts.fragment and parts.port in (None, 443),
                "Source URL must use HTTPS port 443 without credentials or a fragment")
        require(not any(ord(char) <= 32 or ord(char) >= 127 for char in url),
                "Source URL must contain printable ASCII without spaces")
    # invalid ports and host syntax may fail inside urlsplit.
    except ValueError as error:
        # preserve safe validation errors.
        if isinstance(error, SourceError):
            raise
        raise SourceError("Invalid source URL") from error


# fetch one HTTPS response within byte and elapsed-time limits.
def request_bytes(request, maximum):
    deadline = time.monotonic() + NETWORK_TIMEOUT
    # do not follow redirects or log request URLs.
    try:
        # release sockets promptly after consuming the bounded response.
        with build_opener(NoRedirect()).open(request, timeout=NETWORK_TIMEOUT) as response:
            require(response.status == 200, "HTTP response was not successful")
            raw = bytearray()
            # check total elapsed time between reads, not just socket inactivity.
            while len(raw) <= maximum:
                require(time.monotonic() < deadline, "HTTPS response exceeded its time budget")
                chunk = response.read1(min(8192, maximum + 1 - len(raw)))
                require(time.monotonic() <= deadline, "HTTPS response exceeded its time budget")
                # EOF completes this bounded response.
                if not chunk:
                    break
                raw.extend(chunk)
        require(len(raw) <= maximum, "HTTP response exceeds its size limit")
        return bytes(raw)
    # expose status codes, never exception strings containing private URLs.
    except HTTPError as error:
        raise SourceError("HTTP request failed with status %d" % error.code) from error
    # retain cached watch data when a network request fails.
    except (URLError, OSError, TimeoutError) as error:
        raise SourceError("HTTPS request failed or timed out") from error


# fetch and validate a generic JSON source once.
def fetch_json(url):
    validate_url(url)
    request = Request(url, headers={"User-Agent": "XIAO-Watch-Relay/1",
                                   "Accept": "application/json"})
    return parse_json(request_bytes(request, MAX_INPUT))


# import the existing upload normalizer and its bundled New York TZif data.
def weather_module():
    path = Path(__file__).resolve().parents[1] / "project" / "tools" / "upload_weather.py"
    require(path.is_file(), "Run the relay inside the watch workspace")
    spec = importlib.util.spec_from_file_location("watch_upload_weather", path)
    require(spec is not None and spec.loader is not None, "Weather adapter cannot be loaded")
    module = importlib.util.module_from_spec(spec)
    # avoid generating deployment-only bytecode files beside project sources.
    previous = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    # restore the caller's bytecode policy after loading the shared module.
    try:
        spec.loader.exec_module(module)
    # restore policy even when a damaged deployment prevents import.
    finally:
        sys.dont_write_bytecode = previous
    return module


# fetch current conditions, 24 hours and seven modal-icon daily summaries.
def fetch_weather():
    adapter = weather_module()
    query = urlencode({"latitude": adapter.LATITUDE, "longitude": adapter.LONGITUDE,
                       "timezone": adapter.TIMEZONE, "current": "temperature_2m,weather_code",
                       "hourly": "temperature_2m,weather_code",
                       "daily": "temperature_2m_min,temperature_2m_max", "forecast_days": 8,
                       "past_days": 1, "temperature_unit": "celsius", "timeformat": "unixtime"})
    provider = fetch_json(adapter.API_URL + "?" + query)
    # normalize only after the fresh response arrives; never reuse a cache.
    try:
        return adapter.normalize(provider, int(time.time()))
    # retain the normalizer's sanitized validation message.
    except adapter.WeatherError as error:
        raise SourceError("Weather validation failed: " + str(error)) from error


# encode the complete normalized forecast in 118 bytes.
def pack_weather(snapshot):
    adapter = weather_module()
    fields = {"generatedAt", "offsetSeconds", "nextTransitionUtc", "nextOffsetSeconds",
              "observedAt", "validUntil", "tempC", "code", "hourly", "daily"}
    require(type(snapshot) is dict and set(snapshot) == fields,
            "Packed weather requires the complete normalized snapshot")
    hourly, daily = snapshot["hourly"], snapshot["daily"]
    require(type(hourly) is list and len(hourly) == 24
            and type(daily) is list and len(daily) == 7,
            "Packed weather requires 24 hours and seven days")

    # reject missing or additional row fields instead of silently losing data.
    for row in hourly:
        require(type(row) is dict and set(row) == {"utc", "tempC", "code"},
                "Invalid normalized hourly weather fields")
    # retain both UTC boundaries, temperatures and modal code for every day.
    for row in daily:
        require(type(row) is dict and set(row) == {"utc", "nextUtc", "minTempC", "maxTempC", "code"},
                "Invalid normalized daily weather fields")

    # retain integer seconds and whole-minute offsets without rounding.
    for field in ("generatedAt", "observedAt", "validUntil"):
        require(adapter.valid_utc(snapshot[field]), "Invalid packed weather UTC")
    transition = snapshot["nextTransitionUtc"]
    require(type(transition) is int and (transition == 0 or adapter.valid_utc(transition)),
            "Invalid packed weather transition")
    offsets = []
    # the model supports UTC offsets from minus twelve to plus fourteen hours.
    for field in ("offsetSeconds", "nextOffsetSeconds"):
        seconds = snapshot[field]
        require(type(seconds) is int and -43200 <= seconds <= 50400 and seconds % 60 == 0,
                "Packed weather offsets must be whole minutes within UTC-12 to UTC+14")
        offsets.append(seconds // 60)

    # reconstruct hourly UTC exactly from one start and fixed one-hour steps.
    hourly_start = hourly[0]["utc"]
    require(adapter.valid_utc(hourly_start), "Invalid packed hourly start")
    # reject gaps or unsupported UTC values in any hourly row.
    for index, row in enumerate(hourly):
        require(adapter.valid_utc(row["utc"]) and row["utc"] == hourly_start + index * 3600,
                "Packed hourly timestamps must be consecutive")

    # retain DST boundaries as consecutive 23-, 24- or 25-hour durations.
    daily_start = daily[0]["utc"]
    expected, durations = daily_start, []
    # encode each day's exact length without shifting later boundaries.
    for row in daily:
        require(adapter.valid_utc(row["utc"]) and adapter.valid_utc(row["nextUtc"])
                and row["utc"] == expected, "Packed daily timestamps must be consecutive")
        duration = row["nextUtc"] - row["utc"]
        require(duration in (82800, 86400, 90000), "Packed days must span 23, 24 or 25 hours")
        durations.append(duration // 3600 - 23)
        expected = row["nextUtc"]

    # temperatures precede codes and durations in the LSB-first bitstream.
    temperatures = [snapshot["tempC"]] + [row["tempC"] for row in hourly]
    temperatures.extend(value for row in daily for value in (row["minTempC"], row["maxTempC"]))
    codes = [snapshot["code"]] + [row["code"] for row in hourly] + [row["code"] for row in daily]
    bits, position = 0, 0
    # signed twelve-bit tenths preserve every supported temperature exactly.
    for value in temperatures:
        require(adapter.valid_temperature(value), "Packed temperature is outside -100 to 70 Celsius")
        tenths = round(value * 10)
        require(value == tenths / 10, "Packed temperatures require exact tenths of Celsius")
        bits |= (tenths & 0xFFF) << position
        position += 12
    # WMO codes retain their original values rather than an icon-only mapping.
    for code in codes:
        require(adapter.valid_code(code), "Invalid packed WMO code")
        bits |= code << position
        position += 7
    # the six unused high bits remain zero for one canonical representation.
    for duration in durations:
        bits |= duration << position
        position += 2
    # prepend the versioned 29-byte header and verify the encoded snapshot.
    header = WEATHER_HEADER.pack(1, snapshot["generatedAt"], offsets[0], transition, offsets[1],
                                 snapshot["observedAt"], snapshot["validUntil"], hourly_start, daily_start)
    payload = {"packed": base64.b64encode(header + bits.to_bytes(89, "little")).decode("ascii")}
    validate_packed_weather(payload)
    return payload


# reject malformed packed weather before encoding its XWR envelope.
def validate_packed_weather(payload):
    require(type(payload) is dict and set(payload) == {"packed"}
            and type(payload["packed"]) is str, "Packed weather requires one packed field")
    encoded = payload["packed"]
    # strict decoding also rejects non-ASCII text, whitespace and extra padding.
    try:
        raw = base64.b64decode(encoded, validate=True)
    # hide payload contents when decoding fails.
    except (ValueError, binascii.Error) as error:
        raise SourceError("Packed weather must use canonical Base64") from error
    require(len(raw) == 118 and base64.b64encode(raw).decode("ascii") == encoded,
            "Packed weather must decode to 118 bytes using canonical Base64")
    # validate version, UTC offsets and current/hourly coverage.
    version, generated, offset, transition, next_offset, observed, valid_until, hourly, daily_start = WEATHER_HEADER.unpack(raw[:29])
    require(version == 1 and -720 <= offset <= 840 and -720 <= next_offset <= 840,
            "Invalid packed weather version or UTC offset")
    require(all(MIN_UTC <= stamp <= MAX_UTC for stamp in (generated, observed, valid_until, hourly, daily_start))
            and (transition == 0 or generated < transition <= MAX_UTC), "Invalid packed weather UTC range")
    require(transition != 0 or next_offset == offset, "Missing packed UTC offset transition")
    require(observed <= generated + 300 and generated < valid_until and observed < valid_until
            and valid_until - observed <= 3600 and hourly >= max(observed, generated)
            and hourly <= min(observed, generated) + 3600 and hourly + 23 * 3600 <= MAX_UTC,
            "Invalid packed current or hourly weather coverage")
    # reject nonzero padding before decoding the temperature fields.
    bits = int.from_bytes(raw[29:], "little")
    require(bits >> 706 == 0, "Packed weather padding must be zero")
    temperatures = []
    # validate signed temperatures before checking daily minimum/maximum pairs.
    for index in range(39):
        value = (bits >> (index * 12)) & 0xFFF
        value = value - 4096 if value & 0x800 else value
        require(-1000 <= value <= 700, "Packed temperature is outside -100 to 70 Celsius")
        temperatures.append(value)
    require(all(temperatures[index] <= temperatures[index + 1] for index in range(25, 39, 2)),
            "Packed daily minimum exceeds maximum")
    adapter = weather_module()
    # unknown WMO codes cannot be represented by the watch's weather model.
    for index in range(32):
        require(adapter.valid_code((bits >> (468 + index * 7)) & 0x7F), "Invalid packed WMO code")
    daily = daily_start
    # reconstructed days must match complete local dates across DST changes.
    for index in range(7):
        duration = (bits >> (692 + index * 2)) & 3
        require(duration != 3, "Invalid packed day duration")
        seconds = (23 + duration) * 3600
        end = daily + seconds
        start_offset = next_offset if transition and daily >= transition else offset
        end_offset = next_offset if transition and end >= transition else offset
        local_start, local_end = daily + start_offset * 60, end + end_offset * 60
        require(end <= MAX_UTC and local_end % 86400 == 0, "Invalid packed daily midnight")
        # later rows use the recorded offset transition at both boundaries.
        if index:
            require(local_start % 86400 == 0 and local_end - local_start == 86400,
                    "Invalid packed local-day duration")
        # today's start may precede a DST transition already passed at fetch time.
        else:
            inferred_offset = end_offset * 60 + seconds - 86400
            require(-43200 <= inferred_offset <= 50400 and daily <= generated < end
                    and (generated + offset * 60) // 86400 == local_end // 86400 - 1,
                    "Invalid packed current-day coverage")
        daily = end


# encode source data as one canonical XWR text envelope.
def envelope(topic, payload, sequence=None):
    require(type(topic) is str and TOPIC_PATTERN.fullmatch(topic) is not None,
            "Topic must match the watch's 1-32 character topic format")
    sequence = int(time.time()) if sequence is None else sequence
    require(type(sequence) is int and MIN_UTC <= sequence <= MAX_UTC,
            "Computer UTC is outside the supported RTC range")
    # weather retains its lossless compact representation inside codec 1.
    if topic == "weather":
        validate_packed_weather(payload)
        payload = base64.b64decode(payload["packed"])
    # translate codec failures without exposing source values.
    try:
        return encode_text(topic, sequence, payload, codec=int(topic == "weather"))
    except ValueCodecError as error:
        raise SourceError("Binary protocol: " + str(error)) from error


# decode and revalidate a canonical XWR envelope before serving it.
def decode_envelope(body):
    # keep parser errors free of message contents.
    try:
        frame = decode_text(body)
    except ValueCodecError as error:
        raise SourceError("Binary protocol: " + str(error)) from error
    payload = frame["payload"]
    # expose compact weather in the shared validator's expected representation.
    if frame["codec"] == 1:
        payload = {"packed": base64.b64encode(payload).decode("ascii")}
    require(body == envelope(frame["topic"], payload, frame["sequence"]),
            "Input must be the canonical validated envelope")
    return {"v": 1, "type": "data", "seq": frame["sequence"],
            "topic": frame["topic"], "payload": payload}


# read one bounded UTF-8 JSON file without exposing its path or contents.
def read_json_file(path):
    # cap allocation even if the input file is unexpectedly large.
    try:
        with Path(path).open("rb") as source:
            raw = source.read(MAX_INPUT + 1)
    except OSError as error:
        raise SourceError("Cannot read the JSON input file") from error
    return parse_json(raw)
