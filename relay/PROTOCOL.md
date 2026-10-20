# XWR binary relay protocol

XWR version 1 carries a topic, publication sequence and typed payload in a binary frame. [binary_protocol.py](publisher/binary_protocol.py) implements the Python encoder/parser, and [binary_protocol.cpp](../project/src/binary_protocol.cpp) implements the firmware parser declared in [binary_protocol.h](../project/include/binary_protocol.h). The notification route wraps that frame in Base64 because ntfy and iPhone notification attributes carry text. The watch validates the complete frame before applying weather or replacing its generic cache. A source can still return JSON over HTTPS; [publisher.py](publisher/publisher.py) converts its decoded value into XWR before sending it.

The publisher caps the complete notification body at **256 bytes** because the tested phone path truncated longer bodies. This measured operating bound is not a universal ANCS limit. The publisher separately allows up to 256 KiB when reading an HTTPS JSON response or local JSON input; neither limit describes the board's RAM capacity. XWR sends one bounded frame per update; it does not fragment or reassemble larger application payloads.

```mermaid
flowchart LR
    source[Decoded HTTPS JSON value] --> codec[Typed value or weather encoder]
    codec --> raw[Binary header, topic, payload and CRC]
    raw --> text[XW1 prefix and Base64]
    text --> transport[ntfy and iPhone ANCS]
    transport --> parse[Frame and payload validation]
    parse --> weather[Weather model]
    parse --> cache[Latest generic payload]
```

## Notification wrapper and size

The body is ASCII `XW1:` followed immediately by canonical padded standard Base64. Its alphabet uses `+` and `/`, with `=` only where padding is required. Unused padding bits must be zero. The wire body contains no newline, whitespace or trailing data. The parser rejects URL-safe substitutions and alternate encodings of the same bytes.

For a raw frame of `n` bytes, the notification uses `4 + 4 × ceil(n / 3)` bytes. The 256-byte body limit therefore permits at most 189 raw bytes. A frame's fixed header and CRC consume 15 bytes, so a topic of `t` bytes leaves at most `174 - t` bytes for the encoded payload. Payload tags and length prefixes count toward that space. A one-character topic can carry an opaque value containing at most 170 bytes after its three-byte tag and length prefix.

The weather frame contains an 11-byte header, seven topic bytes, 118 payload bytes and a four-byte CRC. Its 140 raw bytes become 192 notification bytes. The previous JSON weather envelope used 240 bytes with a ten-digit sequence, so this representation saves 48 bytes, or 20 percent, for the same weather data. Generic values have different overheads; binary encoding does not guarantee a smaller result for every short scalar or object.

## Binary frame

All multibyte header and checksum fields use little-endian byte order. Header offsets below start at the first raw frame byte, after Base64 decoding. Python represents the 11-byte header with `struct.Struct("<2sBBIBH")`.

| Offset | Width | Field | Interpretation |
| --- | --- | --- | --- |
| `0` | `2` bytes | `magic` | The bytes are ASCII `XW`, or hexadecimal `58 57`. |
| `2` | `1` byte | `version` | The value is `1`. Other versions are rejected. |
| `3` | `1` byte | `codec` | The value is `0` for a typed generic payload or `1` for packed weather. |
| `4` | `4` bytes | `sequence` | The field is an unsigned 32-bit integer. Notification publication uses Unix UTC seconds. |
| `8` | `1` byte | `topic_length` | The field gives the topic's ASCII byte length, from 1 through 32. |
| `9` | `2` bytes | `payload_length` | The field gives the encoded payload's byte length. The complete frame must still fit within 189 bytes. |
| `11` | `topic_length` bytes | `topic` | The topic begins with an ASCII letter or digit and then permits ASCII letters, digits and `._/-`. |
| `11 + topic_length` | `payload_length` bytes | `payload` | The selected codec determines the payload's representation. |
| `11 + topic_length + payload_length` | `4` bytes | `crc32` | The checksum covers every preceding raw frame byte and excludes itself. |

The topic expression is `[A-Za-z0-9][A-Za-z0-9._/-]{0,31}` with an exact end-of-input match. Topics are case-sensitive. The exact topic `weather` requires codec `1`; every other topic requires codec `0`. There is no terminator after the topic or payload. The declared lengths must consume exactly the complete frame before its CRC.

The checksum uses CRC-32/ISO-HDLC with width 32, normal polynomial `0x04C11DB7`, reflected implementation polynomial `0xEDB88320`, initial value `0xFFFFFFFF`, reflected input and output, and final XOR `0xFFFFFFFF`. Its check value for ASCII `123456789` is `0xCBF43926`. Python computes the stored numeric value with `zlib.crc32(frame_without_crc)` and packs it as `<I`. CRC detects accidental corruption; it does not authenticate a publisher or encrypt a message.

`body_fingerprint` in [publisher.py](publisher/publisher.py) and `metadataFingerprint` in [notification_frame.cpp](../project/src/notification_frame.cpp) calculate a `body_hash` fingerprint, reported in publisher logs and USB status from [main.cpp](../project/src/main.cpp). [notification_relay.cpp](../project/src/notification_relay.cpp) retains the receiver's result. That diagnostic value uses FNV-1a over the complete ASCII notification body, including its prefix. It differs from the CRC stored inside the decoded packet and is not an additional wire field.

## Typed generic payload

Codec `0` contains exactly one typed value. Every value starts with one tag byte. Arrays and objects contain recursively encoded values without alignment or padding. Multibyte integers, floating-point bit patterns and lengths use little-endian order.

| Tag | Value | Encoding after the tag |
| --- | --- | --- |
| `0` | The value is null. | No bytes follow for this value. |
| `1` | The value is false. | No bytes follow for this value. |
| `2` | The value is true. | No bytes follow for this value. |
| `3` | The value is a signed integer. | Eight bytes encode a two's-complement signed 64-bit integer. |
| `4` | The value is a real number. | Eight bytes encode a finite IEEE-754 binary64 value. |
| `5` | The value is text. | A two-byte unsigned length precedes that many UTF-8 bytes. |
| `6` | The value is an array. | A one-byte element count precedes that many complete typed values. |
| `7` | The value is an object. | A one-byte entry count precedes each key and typed value in sequence. Each key has a one-byte UTF-8 byte length followed by its bytes. |
| `8` | The value is opaque bytes. | A two-byte unsigned length precedes that many uninterpreted bytes. |

Integers range from `-9223372036854775808` through `9223372036854775807`. Boolean tags remain distinct from integer values. Floating-point values preserve binary64 bits, including negative zero; infinity and NaN are invalid. Text uses strict UTF-8 and rejects overlong encodings, isolated surrogate values and code points beyond `U+10FFFF`. Empty text, empty keys and embedded NUL are permitted. Readers must use explicit lengths rather than treating these fields as NUL-terminated strings.

Objects contain at most 32 entries, and arrays declare at most 255 elements. The 189-byte frame limit normally imposes a smaller practical count. A payload can nest at most eight array or object containers, counting a root container as the first. Object keys must be unique after UTF-8 decoding; the parser does not normalize Unicode or impose key sorting. Unknown tags, incomplete values and trailing payload bytes fail validation. The encoder preserves array order and object insertion order.

Opaque bytes require an encoder caller to provide bytes explicitly. An HTTPS JSON source supplies JSON values and does not acquire a byte-string type from a particular key name. Generic binary payloads remain encoded in one model cache with an explicit binary encoding marker. Another accepted unknown topic replaces that cache; the cache does not create an application or interpret a domain-specific schema.

## Packed weather payload

Codec `1` retains the existing 118-byte weather representation directly. It removes the inner Base64 string and JSON `packed` wrapper from the binary frame; only the outer notification wrapper uses Base64. `binary_protocol.cpp` checks the reserved topic and payload size, [packed_weather.cpp](../project/src/packed_weather.cpp) expands the payload, and `readRelayWeather` in [phone_protocol.cpp](../project/src/phone_protocol.cpp) validates the decoded forecast before changing the model.

The 29-byte weather header uses Python format `<BIhIhIIII`. Its UTC fields are unsigned 32-bit seconds. Its offsets are signed 16-bit minutes. The following offsets are relative to the weather payload, not the enclosing XWR frame.

| Offset | Width | Field | Interpretation |
| --- | --- | --- | --- |
| `0` | `1` byte | `version` | The packed weather version is `1`. |
| `1` | `4` bytes | `generatedAt` | The field records the actual fetch time in UTC. |
| `5` | `2` bytes | `offsetMinutes` | The field gives the supplied local offset before a transition. |
| `7` | `4` bytes | `nextTransitionUtc` | The field gives the next UTC transition, or zero when none is supplied. |
| `11` | `2` bytes | `nextOffsetMinutes` | The field gives the local offset after that transition. |
| `13` | `4` bytes | `observedAt` | The field records the current-condition observation time in UTC. |
| `17` | `4` bytes | `validUntil` | The field records the current-condition validity deadline in UTC. |
| `21` | `4` bytes | `firstHourlyUtc` | The field starts 24 consecutive hourly rows, spaced 3,600 seconds apart. |
| `25` | `4` bytes | `firstDailyUtc` | The field starts seven consecutive local-day summaries. |

The remaining 89 bytes hold an LSB-first bitstream. Its first 468 bits encode 39 signed 12-bit temperatures in Celsius tenths: the current temperature, 24 hourly temperatures and seven minimum/maximum pairs. Its next 224 bits encode 32 unsigned 7-bit WMO codes in current, hourly and daily order. Seven two-bit daily durations follow. Values `0`, `1` and `2` mean 23, 24 and 25 hours, while `3` is invalid. The final six padding bits must be zero.

`pack_weather` in [publisher.py](publisher/publisher.py) requires exact temperature tenths and whole-minute offsets rather than rounding additional precision away. It preserves the daily modal-icon rule implemented by [upload_weather.py](../project/tools/upload_weather.py) and does not replace it with a maximum-severity code. `packed_weather.cpp` checks version, durations, padding and timestamp arithmetic before `readRelayWeather` in `phone_protocol.cpp` checks temperatures, supported codes, sorted coverage, local-day boundaries and freshness. Neither loading a binary snapshot nor applying its timezone metadata writes the RTC.

## Python encoder and parser

The dependency-free [binary_protocol.py](publisher/binary_protocol.py) exposes the following APIs. Its `ValueCodecError` exception reports validation failures without including the input payload. These codec APIs validate representation; [publisher.py](publisher/publisher.py) supplies publication and weather freshness checks.

| API | Behavior |
| --- | --- |
| `encode_frame(topic, sequence, payload, codec=0)` | This function returns one raw frame as Python `bytes`. |
| `decode_frame(raw)` | This function validates raw `bytes` and returns a dictionary containing `topic`, `sequence`, `codec` and `payload`. |
| `encode_notification(topic, sequence, payload, codec=0)` | This function returns the complete `XW1:` notification as ASCII `bytes`. |
| `decode_notification(body)` | This function accepts ASCII `bytes` or a Python string and returns the same decoded fields as `decode_frame`. |

Generic values decode to Python `None`, `bool`, `int`, `float`, `str`, `list`, `dict` and `bytes` according to their tags. Packed weather decodes to its 118 raw bytes; `binary_protocol.py` does not expand weather into forecast rows. `publisher.py` validates packed weather before publication, while `packed_weather.cpp` and `phone_protocol.cpp` expand and validate received weather on the watch. The wire codec accepts the full unsigned 32-bit sequence range, whereas notification publication restricts sequences to supported UTC dates and [notification_protocol.cpp](../project/src/notification_protocol.cpp) enforces the receiver's freshness window.

The following example runs in a Python interpreter started at the workspace root. It builds and parses a generic value locally without accessing a source or sending a notification. The example's `soccer` topic demonstrates transport only; no soccer application is implemented.

```python
# load the workspace codec without installing packages.
import sys
import time
sys.dont_write_bytecode = True
sys.path.insert(0, "relay/publisher")
from binary_protocol import encode_notification, decode_notification

# encode one generic update with fresh UTC sequencing.
body = encode_notification("soccer", int(time.time()), {"home": 2, "away": 1})
print(body.decode("ascii"))

# inspect the parsed fields without publishing the body.
message = decode_notification(body)
print(message["topic"], message["sequence"], message["payload"])
```

The standalone parser in `publisher/binary_protocol.py` reads one notification from a file or standard input. Run the following command from the workspace root, replacing `message.txt` with a file containing one complete body. Use `python` instead of `python3` when that command names the selected Windows interpreter.

```sh
python3 -B relay/publisher/binary_protocol.py message.txt
```

For raw binary input, add `--raw` and supply the binary file. Omitting the filename or using `-` reads standard input. Text input may end with one LF or CRLF in this command-line tool; the strict codec APIs do not accept that line ending as part of a notification. The command prints diagnostic JSON, rendering bytes as `{"$bytes_hex":"..."}`. This display representation does not define another wire format, and a JSON object with that key remains an ordinary object when passed to the encoder.

## Interoperability vector

The generic topic `notes`, sequence `1` and payload `{"count":2,"ok":true}` produce the following 41-byte raw frame. Its encoded payload occupies 21 bytes, and its trailing bytes `87 cc 30 d2` store CRC32 value `0xD230CC87` in little-endian order.

```text
58570100010000000515006e6f746573070205636f756e74030200000000000000026f6b0287cc30d2
```

The corresponding notification body occupies 60 ASCII bytes.

```text
XW1:WFcBAAEAAAAFFQBub3RlcwcCBWNvdW50AwIAAAAAAAAAAm9rAofMMNI=
```

This sequence supports repeatable parser checks and a fresh NUS session. It is not a current publication timestamp, so ANCS freshness checks reject it. A live notification must use current computer UTC and a new sequence for its topic. The following commands exercise only the local parser and do not send data to ntfy or the watch.

On Windows PowerShell, select PlatformIO's Python and pipe the vector to the parser.

```powershell
$protocolPython = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'
'XW1:WFcBAAEAAAAFFQBub3RlcwcCBWNvdW50AwIAAAAAAAAAAm9rAofMMNI=' | & $protocolPython -B relay/publisher/binary_protocol.py
```

On macOS or Linux, use the selected Python 3.9-or-newer interpreter from the workspace root.

```bash
printf '%s\n' 'XW1:WFcBAAEAAAAFFQBub3RlcwcCBWNvdW50AwIAAAAAAAAAAm9rAofMMNI=' | python3 -B relay/publisher/binary_protocol.py
```

The decoded result contains topic `notes`, sequence `1`, codec `0` and the original map with integer `count` equal to `2` and boolean `ok` equal to `true`.

## Firmware parser and storage

[binary_protocol.h](../project/include/binary_protocol.h) declares raw-frame, text-wrapper and typed-value parsers implemented in [binary_protocol.cpp](../project/src/binary_protocol.cpp). They use bounded recursive validation and borrowed views without heap allocation. A caller must keep each backing buffer alive and unmodified while any envelope, value or iterator refers to it. The text parser writes decoded frame bytes into caller-owned storage.

| API | Behavior |
| --- | --- |
| `watch::parseBinaryFrame(data, length, envelope)` | This function validates a raw frame of at most 189 bytes and exposes its topic, sequence, codec and borrowed payload. |
| `watch::parseBinaryText(text, length, storage, capacity, envelope)` | This function validates an `XW1:` body of at most 256 bytes, decodes into the supplied buffer and validates its frame. |
| `watch::parseBinaryValue(data, length, value)` | This function validates exactly one typed generic value with a separate 189-byte input bound. |
| `watch::BinaryValue` | This view exposes `type()`, `boolean()`, `integer()`, `real()`, `bytes()`, `count()`, `iterator()`, `encoded()` and `encodedBytes()`. Scalar accessors require the matching tag. |
| `watch::BinaryIterator::next(...)` | This method traverses array children or object entries in wire order and optionally exposes each borrowed UTF-8 key and length. |
| `watch::binaryCrc32(data, length)` | This function computes the protocol checksum for a supplied byte range. |

The `bytes()` accessor handles either text or opaque bytes; `type()` distinguishes them. The parser clears the returned envelope or value view on failure. It does not promise to erase partially decoded scratch storage. Duplicate-key validation rescans prior bounded entries rather than allocating per-container key tables; its work remains bounded by 32 keys, eight container levels and the 189-byte frame cap.

Representation parsing does not apply data or enforce publication age. In [phone_protocol.cpp](../project/src/phone_protocol.cpp), `ProtocolSession::apply(...)` dispatches an XWR text body or legacy JSON into the application's shared model and ordering rules, while `ProtocolSession::applyBinary(...)` accepts a raw frame from a caller that already preserves message boundaries. Neither function adds a raw-byte GATT service. The ANCS entry point `applyNotificationBody(...)` in [notification_protocol.cpp](../project/src/notification_protocol.cpp) also requires a trusted RTC and checks publication freshness before committing. [packed_weather.cpp](../project/src/packed_weather.cpp) uses a bounded ArduinoJson allocation to expand weather for `readRelayWeather` in `phone_protocol.cpp`; allocation-free parsing applies to the binary frame and generic value views.

Generic consumers must inspect `WatchState::genericEncoding` and use `genericPayloadBytes` as the length, as declared in [watch_model.h](../project/include/watch_model.h). `PayloadEncoding::Binary` stores the typed payload bytes, without the outer XWR header or CRC, while `PayloadEncoding::Json` stores a legacy JSON value. Binary text and opaque values may contain NUL, so string-length functions cannot measure that cache. Weather uses separate model storage and does not replace its encoding marker.

Application processing stages both data and topic history before committing. A notification requires a trusted RTC and a publication sequence no more than 900 seconds behind or 300 seconds ahead of that clock. A repeated accepted sequence reports a duplicate and retains the existing value. An older sequence fails. Eight topic histories persist across ANCS reconnections until reboot; a ninth distinct topic fails instead of silently evicting history. Invalid or stale input changes neither application data nor accepted sequence history.

## Compatibility and delivery checks

[publisher.py](publisher/publisher.py) selects binary by default. `--wire-format json` produces the earlier JSON envelope for firmware migration. The updated firmware accepts either format on the notification route and keeps its separate LightBlue manual service in [manual_bluetooth.cpp](../project/src/manual_bluetooth.cpp) and [manual_protocol.cpp](../project/src/manual_protocol.cpp). [main.cpp](../project/src/main.cpp) receives Nordic UART Service input and passes complete frames to `phone_protocol.cpp`, accepting an `XW1:` body followed by LF with the same 256-byte text bound before that delimiter. Its older JSON frames retain their 8,192-byte bound and 1,024-byte generic payload limit. NUS acknowledgements and polling messages remain JSON for diagnostic clients. NUS sequences increase per topic within a connection rather than representing publication UTC, and that transport resets its eight-topic history on reconnect.

The ntfy application identifier remains `io.heckel.ntfy`, and the title remains exactly `XIAO Relay v1`. The random ntfy channel is separate from an XWR topic. Successful ntfy HTTP publication, ANCS subscription and watch-side application acceptance remain separate events. The [relay guide](README.md) describes pairing, notification permissions and USB counters; the [publisher guide](publisher/README.md) describes generation and sending.

The two locked-phone weather deliveries recorded on October 18, 2026 used the earlier 240-byte JSON envelope. A 192-byte XWR weather notification passed through the iPhone on October 19, 2026, with publisher and watch reports matching sequence `1792456927` and fingerprint `F0A302BE`. The watch accepted the update without ANCS errors or rejections and retained 24 hourly and seven daily records. That transfer established binary application acceptance without a separate screen or phone lock-state confirmation. Repeated natural intervals and long-duration reconnection behavior remain unverified.
