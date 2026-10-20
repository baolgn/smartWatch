"""publish XWR binary data through ntfy; sending requires --send."""
from __future__ import annotations

import argparse
import base64
import binascii
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import struct
import sys
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

from binary_protocol import ValueCodecError, decode_notification, encode_notification

# share source JSON limits and the watch's notification transport bound.
TITLE = "XIAO Relay v1"
TOPIC_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9._/-]{0,31}\Z", re.ASCII)
CHANNEL_PATTERN = re.compile(r"[A-Za-z0-9_-]{32,64}\Z", re.ASCII)
MAX_INPUT = 256 * 1024
MAX_GENERIC = 1024
MAX_MESSAGE = 256
MAX_APNS_ESTIMATE = 3500
MAX_SAFE_INTEGER = 9007199254740991
MIN_UTC, MAX_UTC = 946684800, 4102444799
NETWORK_TIMEOUT = 15
WEATHER_HEADER = struct.Struct("<BIhIhIIII")


# expose failures without URLs, channel names or payload contents.
class PublisherError(ValueError):
    pass


# reject redirects instead of forwarding a secret topic or changing origins.
class NoRedirect(HTTPRedirectHandler):
    # make every redirect a handled HTTP failure.
    def redirect_request(self, request, response, code, message, headers, url):
        return None


# enforce a protocol or transport condition.
def require(condition, message):
    # stop before publishing invalid or oversized data.
    if not condition:
        raise PublisherError(message)


# validate decoded values before deterministic ASCII encoding.
def validate_value(value, depth=0):
    # count containers; the enclosing relay envelope consumes one level.
    if type(value) in (dict, list):
        require(depth < 8, "JSON exceeds eight container levels including the envelope")
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
            # bound work before the payload and notification byte checks.
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
        raise PublisherError("JSON numbers must be finite")

    # translate parser and encoding failures without exposing source data.
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_fields,
                           parse_constant=invalid_constant)
        validate_value(value)
        return value
    # deeply nested or excessively long numeric inputs are invalid too.
    except (UnicodeError, json.JSONDecodeError, RecursionError, OverflowError) as error:
        raise PublisherError("Malformed UTF-8 JSON") from error
    # keep deliberate validation messages but hide Python's source excerpts.
    except ValueError as error:
        # preserve errors already sanitized by this module.
        if isinstance(error, PublisherError):
            raise
        raise PublisherError("Malformed JSON number") from error


# serialize Unicode as escapes, including DEL, with no real line breaks.
def ascii_json(value):
    validate_value(value)
    return json.dumps(value, ensure_ascii=True, allow_nan=False,
                      separators=(",", ":")).encode("ascii")


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
        if isinstance(error, PublisherError):
            raise
        raise PublisherError("Invalid source URL") from error


# perform one bounded HTTPS request; never retry an uncertain publish.
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
        raise PublisherError("HTTP request failed with status %d" % error.code) from error
    # retain cached watch data when a network request fails.
    except (URLError, OSError, TimeoutError) as error:
        raise PublisherError("HTTPS request failed or timed out") from error


# fetch and validate a generic JSON source once.
def fetch_json(url):
    validate_url(url)
    request = Request(url, headers={"User-Agent": "XIAO-Notification-Relay/1",
                                   "Accept": "application/json"})
    return parse_json(request_bytes(request, MAX_INPUT))


# import the existing upload normalizer and its bundled New York TZif data.
def weather_module():
    path = Path(__file__).resolve().parents[2] / "project" / "tools" / "upload_weather.py"
    require(path.is_file(), "Run this publisher inside the watch workspace")
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
    # provider validation messages contain no source/channel secrets.
    except adapter.WeatherError as error:
        raise PublisherError("Weather validation failed: " + str(error)) from error


# preserve a complete normalized forecast in the observed ANCS size limit.
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


# reject malformed packed weather before canonical envelope publication.
def validate_packed_weather(payload):
    require(type(payload) is dict and set(payload) == {"packed"}
            and type(payload["packed"]) is str, "Weather notifications require one packed field")
    encoded = payload["packed"]
    # strict decoding also rejects non-ASCII text, whitespace and extra padding.
    try:
        raw = base64.b64decode(encoded, validate=True)
    # hide payload contents when decoding fails.
    except (ValueError, binascii.Error) as error:
        raise PublisherError("Packed weather must use canonical Base64") from error
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


# conservatively estimate ntfy's APNS message with bounded metadata padding.
def estimate_apns(body):
    message = body.decode("ascii")
    preview = message if len(message) <= 100 else message[:97] + "..."
    payload = {"aps": {"mutable-content": 1, "alert": {"title": TITLE, "body": preview}},
               "id": "x" * 32, "sequence_id": "x" * 64, "time": MAX_UTC,
               "event": "message", "topic": "x" * 64, "priority": 3,
               "title": TITLE, "message": message, "content_type": "text/plain"}
    return len(json.dumps(payload, ensure_ascii=True, separators=(",", ":")).encode("ascii"))


# match the watch's FNV-1a fingerprint without disclosing message contents.
def body_fingerprint(body):
    fingerprint = 2166136261
    # include every byte with the firmware's unsigned 32-bit wraparound.
    for byte in body:
        fingerprint = ((fingerprint ^ byte) * 16777619) & 0xFFFFFFFF
    return fingerprint


# build a binary envelope, retaining JSON only for older firmware migration.
def envelope(topic, payload, sequence=None, wire_format="binary"):
    require(type(topic) is str and TOPIC_PATTERN.fullmatch(topic) is not None,
            "Topic must match the watch's 1-32 character topic format")
    sequence = int(time.time()) if sequence is None else sequence
    require(type(sequence) is int and MIN_UTC <= sequence <= MAX_UTC,
            "Computer UTC is outside the supported RTC range")
    require(wire_format in ("binary", "json"), "Unsupported wire format")
    # keep legacy JSON within its original cache bound.
    if topic != "weather" and wire_format == "json":
        require(len(ascii_json(payload)) <= MAX_GENERIC, "Generic payload exceeds 1,024 ASCII bytes")
    # weather uses its lossless compact adapter on the notification route.
    elif topic == "weather":
        validate_packed_weather(payload)
    # encode binary fields once; Base64 adapts bytes to notification text.
    if wire_format == "binary":
        raw_payload = base64.b64decode(payload["packed"]) if topic == "weather" else payload
        # expose codec failures through the publisher's sanitized error path.
        try:
            body = encode_notification(topic, sequence, raw_payload, codec=int(topic == "weather"))
        # malformed or oversized values must never reach the network.
        except ValueCodecError as error:
            raise PublisherError("Binary protocol: " + str(error)) from error
    # preserve an explicit migration option without changing old receivers.
    else:
        body = ascii_json({"v": 1, "type": "data", "seq": sequence, "topic": topic, "payload": payload})
    # cap the complete body and notification metadata independently.
    require(len(body) <= MAX_MESSAGE, "Notification body exceeds the observed 256-byte ANCS limit")
    require(estimate_apns(body) <= MAX_APNS_ESTIMATE, "Escaped notification exceeds APNS safety budget")
    return body


# parse and revalidate either supported publication format before sending.
def decode_envelope(body):
    # decode the binary packet before weather validation or metadata logging.
    if type(body) is bytes and body.startswith(b"XW1:"):
        # keep parser errors free of message contents.
        try:
            frame = decode_notification(body)
        # match the caller's ordinary validation failure handling.
        except ValueCodecError as error:
            raise PublisherError("Binary protocol: " + str(error)) from error
        payload = frame["payload"]
        # reuse complete weather validation without reparsing source JSON.
        if frame["codec"] == 1:
            payload = {"packed": base64.b64encode(payload).decode("ascii")}
        decoded = {"v": 1, "type": "data", "seq": frame["sequence"],
                   "topic": frame["topic"], "payload": payload}
        wire_format = "binary"
    # older firmware may still receive the original JSON envelope explicitly.
    else:
        decoded = parse_json(body)
        require(type(decoded) is dict and set(decoded) == {"v", "type", "seq", "topic", "payload"}
                and decoded["v"] == 1 and type(decoded["v"]) is int and decoded["type"] == "data",
                "Only complete v1 data envelopes may be published")
        wire_format = "json"
    require(body == envelope(decoded["topic"], decoded["payload"], decoded["seq"], wire_format),
            "Publish input must be the canonical validated envelope")
    return decoded


# publish once to the user-selected random channel without echoing its name.
def publish(body, channel):
    require(type(channel) is str and CHANNEL_PATTERN.fullmatch(channel) is not None,
            "Set NTFY_TOPIC to a random 32-64 character channel using letters, digits, _ or -")
    decoded = decode_envelope(body)
    age = int(time.time()) - decoded["seq"]
    require(-300 <= age <= 900, "Do not publish stale or future-dated envelopes")
    # submit once and verify ntfy echoes the complete message.
    request = Request("https://ntfy.sh/" + channel, data=body, method="POST",
                      headers={"Title": TITLE, "Priority": "3", "Content-Type": "text/plain",
                               "User-Agent": "XIAO-Notification-Relay/1"})
    result = parse_json(request_bytes(request, 16384))
    require(type(result) is dict and result.get("event") == "message"
            and result.get("topic") == channel and result.get("title") == TITLE
            and result.get("message") == body.decode("ascii") and result.get("priority", 3) == 3,
            "Publish response did not confirm the complete message; do not retry blindly")


# validate a bounded list of independent sources without accepting credentials.
def source_config(path):
    config = read_json_file(path)
    require(type(config) is dict and set(config) == {"sources"}
            and type(config["sources"]) is list and 1 <= len(config["sources"]) <= 8,
            "Config must contain one to eight sources")
    topics = set()
    # each source owns one unique topic within the watch's eight-topic limit.
    for source in config["sources"]:
        require(type(source) is dict, "Each configured source must be an object")
        topic = source.get("topic")
        require(type(topic) is str and TOPIC_PATTERN.fullmatch(topic) is not None
                and topic not in topics, "Configured topics must be valid and unique")
        topics.add(topic)
        # use the shared normalizer for the reserved weather topic only.
        if source.get("adapter") == "weather":
            require(source == {"topic": "weather", "adapter": "weather"},
                    "Weather source must contain only its topic and adapter")
        # other sources are public HTTPS JSON values with their own schemas.
        else:
            require(set(source) == {"topic", "url"} and topic != "weather"
                    and type(source["url"]) is str, "Generic source requires topic and HTTPS URL")
            validate_url(source["url"])
    return config["sources"]


# read one bounded UTF-8 JSON file without exposing its path or contents.
def read_json_file(path):
    # preserve only sanitized local I/O failures.
    try:
        # cap allocation even if the input file is unexpectedly large.
        with Path(path).open("rb") as source:
            raw = source.read(MAX_INPUT + 1)
    # report missing or inaccessible input without printing user paths.
    except OSError as error:
        raise PublisherError("Cannot read the JSON input file") from error
    return parse_json(raw)


# collect explicit CLI settings; default operation is a weather dry run.
def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--weather", action="store_true", help="use ZIP 10018 weather (default)")
    group.add_argument("--url", help="fetch a generic HTTPS JSON source")
    group.add_argument("--file", help="read a generic UTF-8 JSON payload")
    group.add_argument("--config", help="read a sources configuration JSON file")
    parser.add_argument("--topic", help="generic payload topic; weather is reserved")
    parser.add_argument("--output", help="write one text-wrapped frame per line to this file")
    parser.add_argument("--wire-format", choices=("binary", "json"), default="binary",
                        help="use XWR binary (default), or JSON for older firmware")
    parser.add_argument("--send", action="store_true", help="publish through NTFY_TOPIC")
    parser.add_argument("--interval", type=int, choices=[900], help="explicitly repeat every 900 seconds")
    return parser.parse_args(argv)


# assemble one fresh message per selected source.
def messages(options):
    # configuration and weather use fixed topics rather than a CLI override.
    require(not options.topic or options.url is not None or options.file is not None,
            "Use --topic only with --url or --file")
    # fetch all configured sources before any message is sent.
    if options.config:
        sources = source_config(options.config)
        # keep an explicit continuous run below ntfy's 250/day free quota.
        require(options.interval is None or len(sources) <= 2,
                "A free 15-minute loop supports at most two sources (192 messages/day)")
        result = []
        # preserve source order for deterministic logs and publication.
        for source in sources:
            payload = pack_weather(fetch_weather()) if source.get("adapter") == "weather" else fetch_json(source["url"])
            result.append(envelope(source["topic"], payload, wire_format=options.wire_format))
        return result
    # require a non-reserved topic for raw generic input.
    if options.url is not None or options.file is not None:
        require(options.topic is not None and options.topic != "weather",
                "Generic input requires --topic; use --weather for complete weather")
        payload = fetch_json(options.url) if options.url is not None else read_json_file(options.file)
        return [envelope(options.topic, payload, wire_format=options.wire_format)]
    # use fresh packed weather when no generic source was selected.
    return [envelope("weather", pack_weather(fetch_weather()), wire_format=options.wire_format)]


# run explicitly; dry runs never publish or start a background service.
def main(argv=None):
    options = arguments(argv)
    # keep channel access and network work inside controlled error reporting.
    try:
        require(options.interval is None or options.send,
                "Repeated operation requires --send; dry runs execute once")
        channel = os.environ.get("NTFY_TOPIC", "")
        # reject missing channels before fetching if publication is requested.
        if options.send:
            require(CHANNEL_PATTERN.fullmatch(channel) is not None,
                    "Set NTFY_TOPIC to a random 32-64 character channel before --send")
        # repeat only when the user explicitly requested a foreground loop.
        while True:
            started = time.monotonic()
            # network failures preserve watch data and retry at the next interval.
            try:
                bodies = messages(options)
                # save data only to the explicit output file; never include secrets.
                if options.output:
                    Path(options.output).write_bytes(b"\n".join(bodies) + b"\n")
                # publish validated messages, then report only non-sensitive counts.
                for body in bodies:
                    # every send remains an explicit user-selected action.
                    if options.send:
                        publish(body, channel)
                    # log sequence, topic and fingerprint without payload contents.
                    metadata = decode_envelope(body)
                    status = "published; watch acceptance unverified" if options.send else "dry run; not published"
                    print("%s: seq=%d topic=%s body_bytes=%d body_hash=%08X estimated_apns_bytes=%d" %
                          (status, metadata["seq"], metadata["topic"], len(body),
                           body_fingerprint(body), estimate_apns(body)), flush=True)
            # keep an explicit loop alive across transient failures.
            except (PublisherError, OSError) as error:
                # a one-shot failure must remain visible to Task Scheduler.
                if options.interval is None:
                    raise
                message = str(error) if isinstance(error, PublisherError) else "Local input/output failed"
                print("error: " + message + "; next attempt in 15 minutes", file=sys.stderr, flush=True)
            # finish after one invocation unless a loop was requested.
            if options.interval is None:
                return 0
            time.sleep(max(1, options.interval - (time.monotonic() - started)))
    # Ctrl+C stops the explicit foreground loop without creating a service.
    except KeyboardInterrupt:
        print("stopped", file=sys.stderr)
        return 130
    # hide path/URL details while making scheduler failures visible.
    except (PublisherError, OSError) as error:
        message = str(error) if isinstance(error, PublisherError) else "Local input/output failed"
        print("error: " + message, file=sys.stderr)
        return 1


# importable helpers do not fetch or publish data.
if __name__ == "__main__":
    raise SystemExit(main())
