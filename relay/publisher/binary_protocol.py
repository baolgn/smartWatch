"""encode and parse bounded XWR v1 relay frames without external packages."""
from __future__ import annotations

import argparse
import base64
import binascii
import json
import math
from pathlib import Path
import re
import struct
import sys
import zlib

# fit one binary frame inside the tested 256-byte notification path.
PREFIX = b"XW1:"
MAX_NOTIFICATION_BYTES = 256
MAX_FRAME_BYTES = 189
MAX_CONTAINERS = 8
WEATHER_BYTES = 118
TOPIC_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9._/-]{0,31}\Z", re.ASCII)
HEADER = struct.Struct("<2sBBIBH")
CRC = struct.Struct("<I")
SIGNED = struct.Struct("<q")
FLOAT = struct.Struct("<d")
LENGTH = struct.Struct("<H")


# expose validation failures without including private input.
class ValueCodecError(ValueError):
    pass


# enforce frame and value invariants before consuming more input.
def _require(condition, message):
    # stop on the first invalid field.
    if not condition:
        raise ValueCodecError(message)


# encode scalar Unicode without permitting isolated surrogate code units.
def _utf8(value, maximum):
    _require(type(value) is str and len(value) <= maximum, "invalid string length or type")
    # translate encoding failures without exposing the string.
    try:
        raw = value.encode("utf-8", "strict")
    # UTF-8 cannot encode isolated surrogate code units.
    except UnicodeError:
        raise ValueCodecError("invalid UTF-8 string") from None
    _require(len(raw) <= maximum, "encoded string exceeds its byte limit")
    return raw


# decode strict UTF-8, rejecting overlong encodings and surrogate sequences.
def _text(raw):
    # retain Unicode scalar values without normalization.
    try:
        return raw.decode("utf-8", "strict")
    # report invalid bytes without including their contents.
    except UnicodeError:
        raise ValueCodecError("invalid UTF-8 string") from None


# validate transport-independent metadata and the topic's codec contract.
def _metadata(topic, sequence, codec):
    _require(type(topic) is str and TOPIC_PATTERN.fullmatch(topic) is not None,
             "invalid relay topic")
    _require(type(sequence) is int and 0 <= sequence <= 0xFFFFFFFF,
             "sequence must be an unsigned 32-bit integer")
    _require(type(codec) is int and codec in (0, 1), "unsupported payload codec")
    _require((topic == "weather") == (codec == 1), "topic does not match payload codec")
    return topic.encode("ascii")


# bound allocations while constructing a typed value.
def _append(output, raw):
    _require(len(output) + len(raw) <= MAX_FRAME_BYTES, "value exceeds frame capacity")
    output.extend(raw)


# encode one typed value; depth counts surrounding array and object containers.
def _encode_value(value, output, depth):
    # represent the three fixed scalar values without length fields.
    if value is None or type(value) is bool:
        _append(output, bytes((0 if value is None else 2 if value else 1,)))
    # preserve the complete signed 64-bit integer range.
    elif type(value) is int:
        _require(-(1 << 63) <= value < (1 << 63), "integer exceeds signed 64-bit range")
        _append(output, b"\x03" + SIGNED.pack(value))
    # preserve finite binary64 values, including signed zero.
    elif type(value) is float:
        _require(math.isfinite(value), "floating-point value must be finite")
        _append(output, b"\x04" + FLOAT.pack(value))
    # distinguish UTF-8 text from uninterpreted byte strings.
    elif type(value) in (str, bytes):
        raw = _utf8(value, MAX_FRAME_BYTES) if type(value) is str else value
        _require(len(raw) <= MAX_FRAME_BYTES, "string exceeds frame capacity")
        _append(output, bytes((5 if type(value) is str else 8,)) + LENGTH.pack(len(raw)) + raw)
    # bound containers before traversing their children.
    elif type(value) in (list, dict):
        _require(depth < MAX_CONTAINERS, "value exceeds eight container levels")
        _require(len(value) <= (32 if type(value) is dict else 255), "too many container entries")
        _append(output, bytes((7 if type(value) is dict else 6, len(value))))
        # preserve object insertion order and require UTF-8 string keys.
        if type(value) is dict:
            # each key has a one-byte encoded length.
            for key, item in value.items():
                raw = _utf8(key, MAX_FRAME_BYTES)
                _append(output, bytes((len(raw),)) + raw)
                _encode_value(item, output, depth + 1)
        # retain array ordering and element types.
        else:
            # recursively encode every array element.
            for item in value:
                _encode_value(item, output, depth + 1)
    # reject implicit coercion of unsupported Python types.
    else:
        raise ValueCodecError("unsupported value type")


# parse one bounded payload with a checked cursor.
class _Reader:
    # retain only the frame's bounded payload and current byte offset.
    def __init__(self, raw):
        self.raw = raw
        self.offset = 0

    # consume exactly the requested number of bytes.
    def take(self, count):
        _require(count <= len(self.raw) - self.offset, "truncated typed value")
        start = self.offset
        self.offset += count
        return self.raw[start:self.offset]

    # consume a single unsigned byte.
    def byte(self):
        return self.take(1)[0]

    # decode one value and reject trailing input in the caller.
    def value(self, depth=0):
        tag = self.byte()
        # decode the fixed scalar tags without payload bytes.
        if tag <= 2:
            return (None, False, True)[tag]
        # unpack signed integers without loss of precision.
        if tag == 3:
            return SIGNED.unpack(self.take(8))[0]
        # reject every NaN and infinity bit pattern.
        if tag == 4:
            value = FLOAT.unpack(self.take(8))[0]
            _require(math.isfinite(value), "floating-point value must be finite")
            return value
        # consume length-prefixed UTF-8 or opaque bytes.
        if tag in (5, 8):
            raw = self.take(LENGTH.unpack(self.take(2))[0])
            return _text(raw) if tag == 5 else raw
        _require(tag in (6, 7), "unknown typed value tag")
        _require(depth < MAX_CONTAINERS, "value exceeds eight container levels")
        count = self.byte()
        # decode an ordered array within the frame's remaining capacity.
        if tag == 6:
            return [self.value(depth + 1) for _ in range(count)]
        _require(count <= 32, "object exceeds 32 entries")
        result = {}
        # reject duplicate decoded keys before accepting their values.
        for _ in range(count):
            key = _text(self.take(self.byte()))
            _require(key not in result, "duplicate object key")
            result[key] = self.value(depth + 1)
        return result


# encode a complete raw frame; weather payloads retain their existing 118-byte format.
def encode_frame(topic, sequence, payload, codec=0):
    topic_raw = _metadata(topic, sequence, codec)
    # weather interpretation belongs to the existing application validator.
    if codec == 1:
        _require(type(payload) is bytes and len(payload) == WEATHER_BYTES,
                 "weather payload must contain 118 bytes")
        raw = payload
    # generic topics carry one complete typed value.
    else:
        output = bytearray()
        _encode_value(payload, output, 0)
        raw = bytes(output)
    frame = HEADER.pack(b"XW", 1, codec, sequence, len(topic_raw), len(raw)) + topic_raw + raw
    _require(len(frame) + CRC.size <= MAX_FRAME_BYTES, "frame exceeds 189 bytes")
    return frame + CRC.pack(zlib.crc32(frame))


# validate a raw frame and return its topic, sequence, codec and decoded payload.
def decode_frame(raw):
    _require(type(raw) is bytes and HEADER.size + 1 + CRC.size <= len(raw) <= MAX_FRAME_BYTES,
             "invalid frame length or type")
    magic, version, codec, sequence, topic_length, payload_length = HEADER.unpack_from(raw)
    _require(magic == b"XW" and version == 1, "unsupported frame signature or version")
    _require(1 <= topic_length <= 32, "invalid topic length")
    _require(HEADER.size + topic_length + payload_length + CRC.size == len(raw),
             "frame lengths do not match")
    _require(zlib.crc32(raw[:-CRC.size]) == CRC.unpack(raw[-CRC.size:])[0], "frame CRC mismatch")
    topic = _text(raw[HEADER.size:HEADER.size + topic_length])
    _metadata(topic, sequence, codec)
    payload = raw[HEADER.size + topic_length:-CRC.size]
    # preserve packed weather for the application-level weather validator.
    if codec == 1:
        _require(len(payload) == WEATHER_BYTES, "weather payload must contain 118 bytes")
    # parse exactly one generic value without unused trailing bytes.
    else:
        reader = _Reader(payload)
        payload = reader.value()
        _require(reader.offset == len(reader.raw), "trailing typed value bytes")
    return {"topic": topic, "sequence": sequence, "codec": codec, "payload": payload}


# wrap a raw frame in canonical ASCII for ntfy and ANCS transport.
def encode_notification(topic, sequence, payload, codec=0):
    body = PREFIX + base64.b64encode(encode_frame(topic, sequence, payload, codec))
    _require(len(body) <= MAX_NOTIFICATION_BYTES, "notification exceeds 256 bytes")
    return body


# accept only canonical padded standard Base64 with the XW1 prefix.
def decode_notification(body):
    # accept text callers without permitting non-ASCII notification content.
    if type(body) is str:
        # bound conversion work before ASCII encoding.
        _require(len(body) <= MAX_NOTIFICATION_BYTES, "notification exceeds 256 bytes")
        # convert ASCII without exposing invalid Unicode input.
        try:
            body = body.encode("ascii", "strict")
        # report encoding errors without including notification contents.
        except UnicodeError:
            raise ValueCodecError("notification must contain ASCII") from None
    _require(type(body) is bytes and len(body) <= MAX_NOTIFICATION_BYTES,
             "invalid notification length or type")
    _require(body.startswith(PREFIX), "unsupported notification prefix")
    encoded = body[len(PREFIX):]
    # reject invalid characters, padding and incomplete Base64 quartets.
    try:
        raw = base64.b64decode(encoded, validate=True)
    # hide malformed content while retaining a useful parser error.
    except (binascii.Error, ValueError):
        raise ValueCodecError("invalid notification Base64") from None
    _require(base64.b64encode(raw) == encoded, "noncanonical notification Base64")
    return decode_frame(raw)


# expose opaque bytes explicitly in the diagnostic JSON representation.
def _diagnostic_value(value):
    # hex output preserves every opaque byte without text decoding.
    if type(value) is bytes:
        return {"$bytes_hex": value.hex()}
    # retain array order while rendering nested byte values.
    if type(value) is list:
        return [_diagnostic_value(item) for item in value]
    # retain object fields while rendering nested byte values.
    if type(value) is dict:
        return {key: _diagnostic_value(item) for key, item in value.items()}
    return value


# parse one notification or raw frame from a file or standard input.
def main(argv=None):
    parser = argparse.ArgumentParser(description="Parse one XWR v1 relay message.")
    parser.add_argument("file", nargs="?", default="-", help="input file; default: standard input")
    parser.add_argument("--raw", action="store_true", help="read raw binary instead of XW1 text")
    arguments = parser.parse_args(argv)
    maximum = MAX_FRAME_BYTES if arguments.raw else MAX_NOTIFICATION_BYTES + 2
    # limit file reads and never include input or file contents in errors.
    try:
        # standard input supports piping a single notification.
        if arguments.file == "-":
            raw = sys.stdin.buffer.read(maximum + 1)
        # close named files after their bounded read.
        else:
            with Path(arguments.file).open("rb") as source:
                raw = source.read(maximum + 1)
        _require(len(raw) <= maximum, "parser input exceeds its size limit")
        # allow one terminal line ending for text files and shell pipelines only.
        if not arguments.raw:
            raw = raw[:-2] if raw.endswith(b"\r\n") else raw[:-1] if raw.endswith(b"\n") else raw
        result = decode_frame(raw) if arguments.raw else decode_notification(raw)
        print(json.dumps(_diagnostic_value(result), ensure_ascii=True, allow_nan=False, indent=2))
        return 0
    # parser failures expose only sanitized validation messages.
    except ValueCodecError as error:
        print("parse failed: " + str(error), file=sys.stderr)
        return 1
    # filesystem failures must not reveal private paths.
    except OSError:
        print("parse failed: input or output unavailable", file=sys.stderr)
        return 1


# run the parser only for direct command-line invocation.
if __name__ == "__main__":
    raise SystemExit(main())
