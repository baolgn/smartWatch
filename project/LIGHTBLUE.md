# LightBlue diagnostics for XIAO Watch

The watch exposes a manual Bluetooth service through [manual_bluetooth.cpp](src/manual_bluetooth.cpp), with commands parsed by [manual_protocol.cpp](src/manual_protocol.cpp), for checking the connection, selecting display units and entering weather through [LightBlue on iPhone](https://apps.apple.com/us/app/lightblue/id557428110). Each command fits in one write of at most 20 printable ASCII bytes without a newline. The service returns a readable application result, so it does not require notification subscriptions.

LightBlue does not fetch weather or set the clock. Normal PlatformIO upload supplies the weather fallback through [platformio_weather.py](tools/platformio_weather.py) and [upload_weather.py](tools/upload_weather.py), then provisions the Round Display RTC over USB through [platformio_rtc.py](tools/platformio_rtc.py) and [sync_rtc.py](tools/sync_rtc.py). The [ntfy relay](../relay/README.md) supplies recurring XWR binary updates. The [firmware guide](README.md#reference-bluetooth-contract) documents the separate Nordic UART transport for XWR text wrappers and legacy JSON. Manual command behavior has been checked against `manual_bluetooth.cpp` and `manual_protocol.cpp`, but the complete LightBlue command workflow has not been confirmed on the physical iPhone.

## Choose the Bluetooth path

The binary relay update leaves the manual command service unchanged. Use the existing ntfy app for recurring weather updates from [publisher.py](../relay/publisher/publisher.py). LightBlue provides manual control and connection diagnostics; it does not convert incoming ntfy messages into Bluetooth writes. [notification_relay.cpp](src/notification_relay.cpp) receives those notifications through Apple's separate ANCS service on the paired iPhone.

| Path | Input and purpose |
| --- | --- |
| The manual Command characteristic accepts short controls. | Each write contains one ASCII command of at most 20 bytes with no newline, such as `PING` or `U,C`. |
| NUS accepts data from a diagnostic or alternative sender. | One complete `XW1:` Base64 wrapper or legacy JSON frame ends with LF, and its application ACK arrives as JSON through NUS TX notifications. |
| ANCS receives matching ntfy notifications from the iPhone. | The publisher sends an `XW1:` body without LF. A complete weather body occupies 192 bytes and retains 24 hourly forecasts, seven daily summaries and timezone rules. |

Do not paste XWR text into the manual Command characteristic or select hexadecimal mode for its Base64 text. To test XWR over NUS, generate a body with [binary_protocol.py](../relay/publisher/binary_protocol.py) using the [encoder examples](../relay/PROTOCOL.md#python-encoder-and-parser), then follow the [NUS framing and acknowledgement contract](README.md#reference-bluetooth-contract). [main.cpp](src/main.cpp) receives the NUS text wrapper, and [phone_protocol.cpp](src/phone_protocol.cpp) processes complete frames; NUS does not accept raw binary directly. Its receive rules differ from one-write manual commands. The 256-byte notification bound is separate from the 20-byte manual command bound.

## Connect and check the service

Power the watch, enable iPhone Bluetooth and grant LightBlue Bluetooth access when requested. Open the nearby-peripherals view and select `XIAO Watch`. Connect inside LightBlue, then discover the manual service using its full UUID. LightBlue may label custom UUIDs as unknown; the characteristic descriptors are `Command (ASCII)` and `Result (ASCII)`. The [Punch Through guide](https://punchthrough.com/how-to-use-lightblue/) describes peripheral discovery and characteristic read/write controls.

| Attribute | UUID | Operation |
| --- | --- | --- |
| The manual-control service groups both characteristics. | `A7E40001-6B18-4F8D-A91F-3E32C2D4B620` | Open this service after connection. |
| The Command characteristic accepts one command. | `A7E40002-6B18-4F8D-A91F-3E32C2D4B620` | Select ASCII/text and write the complete command with response. |
| The Result characteristic holds the latest application reply. | `A7E40003-6B18-4F8D-A91F-3E32C2D4B620` | Select ASCII/text and read the value after processing. |

Write exactly `PING` to Command without quotes, spaces or a line ending. Wait briefly, then read Result and check for `OK PONG`. If the old result remains, read again before issuing another command. Command supports Write with response, while Result supports Read; reading Command only returns its last input.

The Bluetooth write response acknowledges receipt at the characteristic, not completion of the watch operation. `manual_bluetooth.cpp` queues each command for the watch task in `main.cpp`, which calls `manual_protocol.cpp` to process it. Send one command at a time and read its application result before the next write. `READY` identifies a new session before processing; `DISCONNECTED` records an ended session, although a phone may retain a cached value after losing the link. A fresh read requires reconnection, and Result holds one reply rather than a history.

The watch permits one peripheral connection. It advertises ANCS solicitation and exposes NUS/name information in its scan response, so a filter that requires the manual service in advertising can hide it. Discover the manual service after connecting by name. This service has open BLE permissions and does not require prior pairing, although the separate notification receiver can initiate iPhone pairing for ANCS.

## Clock and weather behavior

The PCF8563 remains the primary clock. A normal PlatformIO upload sets fresh computer UTC and verifies advancing hardware time; [sync_rtc.py](tools/sync_rtc.py) provides [USB recovery](README.md#set-the-hardware-rtc-over-usb) without reflashing. Bluetooth time messages are unsupported. The former `T` command returns `ERR COMMAND`, and `STATUS` only reports time validity. A CR927 backup cell is required to retain time without main power; its installation and retention remain unverified on the assembled watch.

Write `U,C` for Celsius or `U,F` for Fahrenheit. This changes the display unit in RAM, while every manual weather temperature still uses integer tenths Celsius. For example, `235` represents 23.5 degrees Celsius and `-35` represents -3.5 degrees. Reboot restores the default Celsius setting.

Once the RTC is valid, obtain an actual recent observation and send the following command with its values. `W,235,2` is a syntax example for 23.5 degrees Celsius and partly cloudy; it is not a current observation.

```text
W,<tenthsC>,<code>
```

A successful `W` timestamps the supplied conditions with watch UTC and gives them one hour of validity. It replaces the active snapshot and clears all hourly forecasts, daily summaries and fetch metadata. The Weather header therefore uses its missing-fetch-time placeholder for this override. The face can show the supplied conditions, but the command does not generate predictions. Reboot restores the embedded fallback with its original timestamps.

### Weather values

Temperatures range from `-1000` through `700` in tenths Celsius. Condition values must use the supported WMO codes below. Another provider's icon identifier requires conversion before sending.

| Codes | Conditions |
| --- | --- |
| `0`, `1`, `2` and `3` describe cloud cover. | They represent clear, mainly clear, partly cloudy and overcast respectively. |
| `45` and `48` describe fog. | Both map to the fog icon. |
| `51`, `53` and `55` describe drizzle. | These codes select drizzle conditions. |
| `56` and `57` describe freezing drizzle. | These codes retain the freezing-drizzle classification. |
| `61`, `63` and `65` describe rain. | These codes select rain conditions. |
| `66` and `67` describe freezing rain. | These codes retain the freezing-rain classification. |
| `71`, `73`, `75` and `77` describe snow. | These codes select snow conditions. |
| `80`, `81` and `82` describe rain showers. | These codes select rain-shower conditions. |
| `85` and `86` describe snow showers. | These codes select snow-shower conditions. |
| `95` describes a thunderstorm. | This code selects the thunderstorm condition. |
| `96` and `99` describe thunderstorms with hail. | These codes retain the hail classification. |

Battery information remains local to the watch's measurement circuit. The manual service has no battery command or soccer command.

## Enter a 24-hour forecast

Prepare 24 real hourly records before starting manual entry, and finish any USB RTC adjustment first. Send current conditions with `W`, then start the staging area with `F,<firstUtc>`. The timestamp uses UTC Unix seconds and must fall between the current-weather observation time and one hour later. This starts a new staged forecast without publishing partial rows.

Write `H,<index>,<tenthsC>,<code>` once for each index from `0` through `23`, reading Result after every write. Index zero belongs to `firstUtc`, and each later row is exactly one hour later. For syntax only, `H,0,230,2` describes 23.0 degrees Celsius and partly cloudy for the first row. Substitute actual forecast values. Rewriting an index corrects that row; `OK HOUR nn/24` counts distinct received indices.

After all 24 indices are present, send `SAVE` and expect `OK FORECAST 24/24`. [manual_protocol.cpp](src/manual_protocol.cpp) validates the stage and replaces the complete hourly forecast together. The sequence requires 27 writes for `W`, `F`, 24 rows and `SAVE`; `STATUS` adds a separate check. Saving does not extend the one-hour current-weather validity interval or supply daily forecasts. The complete `weather` topic through the [generic relay](../relay/README.md) carries both hourly and daily data.

`CANCEL` discards the stage. An unfinished stage also expires after five minutes without another accepted `H` row, after 30 minutes total, or on disconnect. `PING` and `STATUS` do not extend the deadline. A new `W` discards staging, and a new accepted `F` replaces it. Invalid commands preserve it unless the runtime has independently cleared an expired stage. If another weather transport replaces the underlying snapshot during entry, `H` or `SAVE` rejects the mismatched stage with `ERR WEATHER CHANGED`.

## Command reference

Each row describes one complete write to Command, as implemented in [manual_protocol.cpp](src/manual_protocol.cpp) and declared in [manual_protocol.h](include/manual_protocol.h). Angle-bracket fields stand for replacement values and are not literal input. Commands use uppercase letters and decimal integers separated by commas without spaces or a trailing newline. Never split a manual command across writes; NUS fragmentation belongs to the separate relay service.

| Command | Effect |
| --- | --- |
| `PING` checks the application. | Result becomes `OK PONG` without changing data. |
| `U,C` or `U,F` selects display units. | Result becomes `OK UNITS`, and stored weather remains Celsius. |
| `W,<tenthsC>,<code>` replaces current conditions. | Result becomes `OK WEATHER`, and previous hourly/daily forecasts are cleared. |
| `F,<firstUtc>` starts a forecast stage. | Result becomes `OK FORECAST 00/24` while the published data remains available. |
| `H,<index>,<tenthsC>,<code>` supplies one row. | Result becomes `OK HOUR nn/24` after an accepted index from 0 through 23. |
| `SAVE` commits a complete valid stage. | Result becomes `OK FORECAST 24/24` after publishing all rows. |
| `CANCEL` discards staging. | Result becomes `OK CANCEL` without replacing published weather. |
| `STATUS` reports state. | Result reports time validity as `T`, weather presence as `W`, stored hourly count as `H` and staged row count as `F`. |

For example, `T1 W1 H24 F00/24` means usable time, a stored weather snapshot, 24 stored hourly records and no staged rows. `W1` reports snapshot presence rather than freshness. `H24` remains the stored count even after elapsed rows disappear from the visible forecast.

## Troubleshooting

When the watch is absent, confirm power, Bluetooth permission and proximity, refresh LightBlue's nearby-peripherals view, and disconnect another client if it occupies the watch's connection. A paired watch can retain a system connection after leaving an app. The [LightBlue connection FAQ](https://punchthrough.com/lightblue-faq/) describes connection and permission checks. When a firmware upload changes the services, disconnect and rediscover them; restart LightBlue if its old service list remains cached.

When Read is unavailable, confirm that the characteristic begins with `A7E40003`. NUS TX begins with `6E40` and uses notifications instead. When a write succeeds but the interface does not change, read Result after a short pause and check its value. Verify ASCII encoding, the complete Command UUID, uppercase input, comma placement and the 20-byte limit.

| Result | Recovery |
| --- | --- |
| `ERR TIME REQUIRED` reports an unset or unusable clock. | Provision the RTC over USB before entering weather. |
| `ERR WEATHER REQUIRED` reports missing base conditions. | Send a real observation with `W` before starting `F`. |
| `ERR HOURS MISSING` reports incomplete staging. | Supply the missing indices before repeating `SAVE`. |
| `ERR STAGE REQUIRED` or `ERR STAGE EXPIRED` reports absent or expired staging. | Start again with `F` and enter all 24 rows within the deadlines. |
| `ERR WEATHER CHANGED` or `ERR TIME CHANGED` reports a changed staging basis. | Verify the current weather and RTC, then restart entry against current data. |
| `ERR RANGE`, `ERR FORMAT`, `ERR UNITS`, `ERR LENGTH`, `ERR ASCII` or `ERR COMMAND` reports rejected input. | Correct the complete command and resend it. Newlines are invalid ASCII command bytes. |
| `ERR QUEUE FULL` reports lost queued input. | Stop writing, read Result and restart incomplete forecast entry with `F`. Firmware clears queued commands and staging while retaining already applied data. |

For recurring weather, disconnect LightBlue before using the paired iPhone notification route. Enable ntfy Notification Center, Show Previews set to Always, and the watch's Share System Notifications setting. A visible lock-screen notification does not by itself establish watch delivery. The [relay guide](../relay/README.md) distinguishes server publication, iPhone receipt and watch acceptance. If the ntfy subscription name is missing, recover `relay/publisher/.local/ntfy_topic.txt` through [prepare_pc.ps1](../relay/publisher/prepare_pc.ps1) as described in the [publisher guide](../relay/publisher/README.md); creating another topic would separate the phone subscription from the running publisher.

USB diagnostics confirmed one 192-byte XWR weather delivery through the iPhone on October 19, 2026, with matching sequence and fingerprint, one applied update and no ANCS errors, rejections or dropped messages. That check did not record the phone's lock state or include a new visual confirmation. Earlier locked-phone checks used legacy JSON; they do not establish a separate LightBlue command test or sustained binary delivery cadence.
