#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>
#include <lvgl.h>
#include <atomic>

// keep the vendor display and 4,800-byte buffer; use guarded touch below.
#define USE_TFT_ESPI_LIBRARY
#include <lv_xiao_round_screen.h>
#include "board_support.h"
#include "phone_protocol.h"
#include "relay_schedule.h"
#include "manual_bluetooth.h"
#include "rtc_usb_protocol.h"
#include "orientation_sensor.h"
#include "orientation_wake.h"
#include "backlight.h"
#include "display_transport.h"
#include "watch_ui.h"
#include "upload_weather_data.h"

// keep watch state and hardware callbacks private to this translation unit.
namespace {
// share the model only through watch-task updates and diagnostic snapshots.
watch::WatchState state;
watch::LineFramer receiver;
watch::ProtocolSession protocol;
BLEUart phoneUart(2048);
bool bluetoothReady = false;
// track orientation samples and requested backlight levels.
watch::OrientationWake orientationWake;
bool backlightReady = false;
uint32_t lastOrientationSampleMs = 0;
uint32_t lastOrientationRetryMs = 0;
int16_t faceUpMilliG = 0;
constexpr uint8_t kAwakeBacklight = 255;
constexpr uint8_t kDimBacklight = 10;  // ~4% PWM duty; brightness is nonlinear.
// track BLE connection/overflow generations and partial outgoing frames.
volatile uint32_t connectionGeneration = 0;
volatile uint32_t overflowGeneration = 0;
volatile uint32_t connectionStartOverflow = 0;
uint32_t handledGeneration = 0;
uint32_t handledOverflow = 0;
bool sessionClosing = false;
uint32_t closingGeneration = 0;
char pendingAck[256] = {};
size_t ackLength = 0;
size_t ackSent = 0;
uint32_t ackStarted = 0;
// share the NUS stream between ACKs and periodic refresh polls.
watch::RelaySchedule relaySchedule;
uint32_t relayPollsQueued = 0;

// report the current startup phase over USB.
enum class BootStage : uint8_t {
    Starting, BoardInit, RtcRead, BatteryRead, OrientationInit, BluetoothInit,
    DisplayInit, UiInit, UiReady, Running, TaskCreateFailed
};
// cache hardware and UI diagnostics for the USB task.
struct UsbStatus {
    // track startup and watch-task progress independently of USB output.
    BootStage stage = BootStage::Starting;
    uint32_t capturedMs = 0;
    uint32_t loops = 0;
    uint32_t progressMs = 0;
    uint32_t stackFree = 0;
    // retain clock trust and battery readings from the hardware owner.
    uint32_t utc = 0;
    int32_t batteryMv = 0;
    int16_t batteryPercent = -1;
    bool rtcPresent = false;
    bool rtcTrusted = false;
    bool rtcFault = false;
    bool timeValid = false;
    // cache BLE identity and service readiness for diagnostics.
    bool bleReady = false;
    bool bleConnected = false;
    bool addressKnown = false;
    uint8_t address[6] = {};
    uint8_t addressType = 0;
    uint8_t bleError = 0;  // 1=stack, 2=UART, 3=advertising, 4=manual.
    bool manualReady = false;
    uint32_t manualApplied = 0;
    char manualResult[21] = {};
    // report UI input health and bounded weather coverage.
    ui::View currentView = ui::View::Face;
    uint32_t touchErrors = 0;
    uint32_t touchRecoveries = 0;
    bool weatherEmbedded = false;
    uint8_t weatherHours = 0;
    uint8_t weatherDays = 0;
    uint32_t weatherFetchedAt = 0;
    // copy relay metadata only; payloads stay in the watch task.
    uint32_t relayRevision = 0;
    uint32_t relaySequence = 0;
    uint32_t relayReceivedAtMs = 0;
    uint32_t relayPollsQueued = 0;
    uint16_t genericPayloadBytes = 0;
    bool genericBinary = false;
    char relayTopic[33] = {};
    // retain single-axis wake, PWM and display-failure diagnostics.
    bool orientationReady = false;
    bool orientationHealthy = false;
    bool displayDimmed = false;
    bool backlightReady = false;
    int16_t rawZMilliG = 0;
    int16_t faceUpMilliG = 0;
    int16_t filteredZMilliG = 0;
    uint32_t orientationErrors = 0;
    const char* orientationFailureStage = "none";
    uint8_t orientationFailureRegister = 0;
    uint8_t orientationFailureObserved = 0;
    uint32_t orientationFailureDetail = 0;
    uint8_t backlight = 255;
    int16_t backlightHighPermille = -1;
    uint32_t maxFlushUs = 0;
    uint32_t maxRenderMs = 0;
    uint32_t displayErrors = 0;
};
// retain progress, BLE identity and atomic render diagnostics.
UsbStatus usbStatus;
uint32_t watchLoops = 0;
uint32_t watchProgressMs = 0;
uint8_t diagnosticBleError = 0;
ble_gap_addr_t diagnosticAddress = {};
bool diagnosticAddressKnown = false;
std::atomic<const char*> workPhase{"startup"};
std::atomic<uint32_t> displayFlushes{0};
uint32_t maxFlushUs = 0;
uint32_t maxRenderMs = 0;
uint32_t displayErrors = 0;

// queue one USB command; the watch task owns Wire/RTC/model, USB owns serial I/O.
enum class RtcJobState : uint8_t { Idle, Pending, Working, Reply };
// carry one RTC request and reply across the USB/watch task boundary.
struct RtcJob {
    RtcJobState phase = RtcJobState::Idle;
    rtcusb::Command command;
    uint32_t generation = 0;
    uint32_t receivedMs = 0;
    uint32_t utc = 0;
    bool ok = false;
};
// retain USB framing and session generation between task iterations.
RtcJob rtcJob;
uint32_t usbGeneration = 0;
rtcusb::Framer rtcInput;

// sample screen-normal Z and advance the orientation policy even without fresh data.
void sampleOrientation() {
    const uint32_t nowMs = millis();
    bool fresh = false;
    int16_t sample = 0;
    // poll the initialized IMU at most once every 40 ms.
    if (board::orientationReady() &&
        static_cast<uint32_t>(nowMs - lastOrientationSampleMs) >= 40) {
        lastOrientationSampleMs = nowMs;
        fresh = board::readFaceUpMilliG(sample) == board::OrientationRead::Fresh;
        // publish only newly received acceleration.
        if (fresh) faceUpMilliG = sample;
    }
    orientationWake.update(fresh, sample, millis());
}

// apply posture changes to PWM and the UI outside drawing callbacks.
void applyOrientation() {
    bool dimmed = backlightReady && board::orientationReady() && !orientationWake.awake();
    // avoid redundant backlight writes and UI resets.
    if (dimmed == state.displayDimmed) return;
    // fall back to the awake view if PWM control fails.
    if (!board::setBacklight(dimmed ? kDimBacklight : kAwakeBacklight)) {
        backlightReady = false;
        dimmed = false;
    }
    state.displayDimmed = dimmed;
    // commit views outside LVGL event/draw dispatch.
    ui::update(state);
}

// flush an LVGL strip synchronously and sample orientation between transfers.
void displayFlush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    static_assert(LV_DRAW_BUF_STRIDE_ALIGN == 1 && LV_COLOR_DEPTH == 16,
                  "Display transport requires tightly packed RGB565 pixels");
    workPhase.store("display_flush", std::memory_order_relaxed);
    const uint32_t startedUs = micros();
    /* send one blocking SPIM3 transfer per strip; pushImage uses per-byte DMA,
     * and vendor endWrite/dmaWait targets the wrong peripheral, SPIM0. */
    if (!board::writeDisplayRect(area->x1, area->y1,
                                area->x2 - area->x1 + 1,
                                area->y2 - area->y1 + 1, pixels)) {
        ++displayErrors;  // rejected LVGL bounds; no out-of-buffer reads.
    }
    const uint32_t durationUs = micros() - startedUs;
    // retain the longest observed strip transfer.
    if (durationUs > maxFlushUs) maxFlushUs = durationUs;
    // release the completed LVGL buffer and count the flush.
    lv_display_flush_ready(display);
    displayFlushes.fetch_add(1, std::memory_order_relaxed);
    /* redraws can exceed debounce time; sample the separate IMU bus between
     * strips without changing the UI during drawing. */
    sampleOrientation();
    workPhase.store("render", std::memory_order_relaxed);
}

/* only the watch task reads hardware/model state; USB uses a locked snapshot.
 * keep formatting and USB I/O outside the critical section. */
void publishStatus(BootStage stage) {
    // collect watch-owned state before entering the short snapshot lock.
    UsbStatus next;
    next.stage = stage;
    next.capturedMs = millis();
    next.loops = watchLoops;
    next.progressMs = watchProgressMs;
    next.stackFree = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
    next.utc = state.utc;
    next.batteryMv = static_cast<int32_t>(state.batteryVoltage * 1000.0f + 0.5f);
    next.batteryPercent = state.batteryPercent;
    next.rtcPresent = state.rtcAvailable;
    next.rtcTrusted = state.timeValid && !state.rtcFault;
    next.rtcFault = state.rtcFault;
    next.timeValid = state.timeValid;
    next.bleReady = bluetoothReady;
    next.bleConnected = state.bleConnected;
    next.addressKnown = diagnosticAddressKnown;
    memcpy(next.address, diagnosticAddress.addr, sizeof(next.address));
    next.addressType = diagnosticAddress.addr_type;
    next.bleError = diagnosticBleError;
    next.manualReady = manualBle::ready();
    next.manualApplied = manualBle::appliedCount();
    memcpy(next.manualResult, manualBle::lastResult(), sizeof(next.manualResult));
    next.currentView = ui::currentView();
    next.touchErrors = board::touchErrors();
    next.touchRecoveries = board::touchRecoveries();
    next.weatherEmbedded = state.weather.embedded;
    next.weatherHours = state.weather.hourlyCount;
    next.weatherDays = state.weather.dailyCount;
    next.weatherFetchedAt = state.weather.fetchedAt;
    // expose generic transport progress without printing arbitrary payloads.
    next.relayRevision = state.relayRevision;
    next.relaySequence = state.relaySequence;
    next.relayReceivedAtMs = state.relayReceivedAtMs;
    next.relayPollsQueued = relayPollsQueued;
    next.genericPayloadBytes = state.genericPayloadBytes;
    next.genericBinary = state.genericEncoding == watch::PayloadEncoding::Binary;
    memcpy(next.relayTopic, state.relayTopic, sizeof(next.relayTopic));
    // report sensor freshness separately from requested backlight state.
    next.orientationReady = board::orientationReady();
    next.orientationHealthy = orientationWake.healthy();
    next.displayDimmed = state.displayDimmed;
    next.backlightReady = backlightReady;
    next.rawZMilliG = board::orientationRawZMilliG();
    next.faceUpMilliG = faceUpMilliG;
    next.filteredZMilliG = orientationWake.filteredZMilliG();
    next.orientationErrors = board::orientationErrors();
    next.orientationFailureStage = board::orientationFailureStage();
    next.orientationFailureRegister = board::orientationFailureRegister();
    next.orientationFailureObserved = board::orientationFailureObserved();
    next.orientationFailureDetail = board::orientationFailureDetail();
    next.backlight = board::backlightLevel();
    next.backlightHighPermille = board::backlightHighPermille();
    next.maxFlushUs = maxFlushUs;
    next.maxRenderMs = maxRenderMs;
    next.displayErrors = displayErrors;
    // copy the complete snapshot atomically for the USB reader.
    taskENTER_CRITICAL();
    usbStatus = next;
    taskEXIT_CRITICAL();
}

// service USB status and RTC requests without touching hardware from this task.
void usbDiagnostics() {
    // retain nonblocking output and connection state between USB iterations.
    static bool wasConnected = false;
    static bool requested = false;
    static uint32_t lastReportMs = 0;
    static char output[2304];
    static size_t length = 0;
    static size_t sent = 0;
    const bool connected = Serial;
    // drop stale framing and pending work while USB is disconnected.
    if (!connected) {
        // dropping DTR leaves TinyUSB's receive FIFO intact.
        tud_cdc_read_flush();
        // invalidate the departed USB session once.
        if (wasConnected) {
            taskENTER_CRITICAL();
            ++usbGeneration;
            rtcJob.phase = RtcJobState::Idle;
            taskEXIT_CRITICAL();
        }
        rtcInput.reset();
        wasConnected = false;
        requested = false;
        length = sent = 0;
        return;
    }
    const uint32_t nowMs = millis();
    // request an initial status report for the new USB connection.
    if (!wasConnected) {
        tud_cdc_read_flush();  // discard stale bytes before reporting readiness.
        requested = true;
    }
    wasConnected = true;
    // limit serial parsing to 32 bytes per USB iteration.
    for (unsigned budget = 0; budget < 32 && Serial.available(); ++budget) {
        const int byte = Serial.read();
        // stop when the receive FIFO empties.
        if (byte < 0) break;
        rtcusb::Command command;
        const auto input = rtcInput.push(static_cast<uint8_t>(byte), nowMs, command);
        // schedule diagnostics when the host sends a status query.
        if (input == rtcusb::Input::Status) requested = true;
        // handoff a parsed RTC command under the shared-state lock.
        if (input == rtcusb::Input::Command) {
            taskENTER_CRITICAL();
            // accept only one outstanding RTC request.
            if (rtcJob.phase == RtcJobState::Idle) {
                rtcJob.command = command;
                rtcJob.generation = usbGeneration;
                rtcJob.receivedMs = nowMs;
                rtcJob.phase = RtcJobState::Pending;
            }
            taskEXIT_CRITICAL();
        }
    }
    // refresh connected-host diagnostics every 30 seconds.
    if (static_cast<uint32_t>(nowMs - lastReportMs) >= 30000) requested = true;
    // send an RTC reply only after the previous serial output finishes.
    if (sent == length) {
        RtcJob reply;
        taskENTER_CRITICAL();
        // claim the watch task reply and release the command slot.
        if (rtcJob.phase == RtcJobState::Reply) {
            reply = rtcJob;
            rtcJob.phase = RtcJobState::Idle;
        }
        taskEXIT_CRITICAL();
        // format the claimed reply outside the critical section.
        if (reply.phase == RtcJobState::Reply) {
            length = rtcusb::formatReply(reply.command, reply.ok, reply.utc, output, sizeof(output));
            sent = 0;
        }
    }
    // build requested diagnostics only when the output buffer is free.
    if (requested && sent == length) {
        // take a consistent cached snapshot without accessing live hardware.
        UsbStatus current;
        taskENTER_CRITICAL();
        current = usbStatus;
        taskEXIT_CRITICAL();
        // map startup and UI enums to stable diagnostic names.
        static const char* const stages[] = {
            "starting", "board_init", "rtc_read", "battery_read", "orientation_init", "bluetooth_init",
            "display_init", "ui_init", "ui_ready", "running", "task_create_failed"
        };
        static const char* const views[] = {"face", "launcher", "clock", "weather", "calendar"};
        char address[18] = "unknown";
        // format the BLE address only after successful discovery.
        if (current.addressKnown) {
            // Nordic stores addresses least-significant byte first.
            snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X",
                     static_cast<unsigned>(current.address[5]), static_cast<unsigned>(current.address[4]),
                     static_cast<unsigned>(current.address[3]), static_cast<unsigned>(current.address[2]),
                     static_cast<unsigned>(current.address[1]), static_cast<unsigned>(current.address[0]));
        }
        // format the cached report into the bounded USB output buffer.
        const int count = snprintf(output, sizeof(output),
            "WATCH uptime_ms=%lu stage=%s board=%s loops=%lu progress_age_ms=%lu snapshot_age_ms=%lu\n"
            "UI phase=%s view=%s flushes=%lu touch_errors=%lu recoveries=%lu\n"
            "RTC present=%u trusted=%u fault=%u time_valid=%u utc=%lu\n"
            "RTC USB protocol=1\n"
            "BATTERY measured_mv=%ld estimated_percent=%d\n"
            "BLE ready=%u connected=%u address=%s type=%u error=%u\n"
            "MANUAL ready=%u accepted=%lu result=%s\n"
            "WEATHER embedded=%u hours=%u days=%u fetched_utc=%lu\n"
            "RELAY revision=%lu topic=%s seq=%lu received_ms=%lu generic_bytes=%u polls_queued=%lu interval_s=900 binary_v=1 generic_format=%s\n"
            "ORIENTATION ready=%u healthy=%u raw_z_mg=%d face_up_mg=%d filtered_mg=%d errors=%lu dim=%u pwm_ready=%u backlight=%u\n"
            "IMU last_failure=%s reg=0x%02X observed=0x%02X detail=0x%08lX\n"
            "DISPLAY max_flush_us=%lu max_render_ms=%lu errors=%lu d6_high_permille=%d\n"
            "WATCH stack_free_bytes=%lu (USB '?' repeats; display visibility unverified)\n",
            static_cast<unsigned long>(nowMs), stages[static_cast<unsigned>(current.stage)],
            board::startupStage(),
            static_cast<unsigned long>(current.loops),
            static_cast<unsigned long>(nowMs - current.progressMs),
            static_cast<unsigned long>(nowMs - current.capturedMs),
            workPhase.load(std::memory_order_relaxed),
            static_cast<unsigned>(current.currentView) < sizeof(views) / sizeof(views[0]) ?
                views[static_cast<unsigned>(current.currentView)] : "unknown",
            static_cast<unsigned long>(displayFlushes.load(std::memory_order_relaxed)),
            static_cast<unsigned long>(current.touchErrors),
            static_cast<unsigned long>(current.touchRecoveries),
            static_cast<unsigned>(current.rtcPresent), static_cast<unsigned>(current.rtcTrusted),
            static_cast<unsigned>(current.rtcFault), static_cast<unsigned>(current.timeValid),
            static_cast<unsigned long>(current.utc), static_cast<long>(current.batteryMv),
            static_cast<int>(current.batteryPercent), static_cast<unsigned>(current.bleReady),
            static_cast<unsigned>(current.bleConnected), address,
            static_cast<unsigned>(current.addressType), static_cast<unsigned>(current.bleError),
            static_cast<unsigned>(current.manualReady),
            static_cast<unsigned long>(current.manualApplied), current.manualResult,
            static_cast<unsigned>(current.weatherEmbedded),
            static_cast<unsigned>(current.weatherHours), static_cast<unsigned>(current.weatherDays),
            static_cast<unsigned long>(current.weatherFetchedAt),
            static_cast<unsigned long>(current.relayRevision), current.relayTopic,
            static_cast<unsigned long>(current.relaySequence),
            static_cast<unsigned long>(current.relayReceivedAtMs),
            static_cast<unsigned>(current.genericPayloadBytes),
            static_cast<unsigned long>(current.relayPollsQueued),
            current.genericBinary ? "binary" : "json",
            static_cast<unsigned>(current.orientationReady), static_cast<unsigned>(current.orientationHealthy),
            static_cast<int>(current.rawZMilliG), static_cast<int>(current.faceUpMilliG),
            static_cast<int>(current.filteredZMilliG), static_cast<unsigned long>(current.orientationErrors),
            static_cast<unsigned>(current.displayDimmed), static_cast<unsigned>(current.backlightReady),
            static_cast<unsigned>(current.backlight),
            current.orientationFailureStage, static_cast<unsigned>(current.orientationFailureRegister),
            static_cast<unsigned>(current.orientationFailureObserved),
            static_cast<unsigned long>(current.orientationFailureDetail),
            static_cast<unsigned long>(current.maxFlushUs), static_cast<unsigned long>(current.maxRenderMs),
            static_cast<unsigned long>(current.displayErrors), static_cast<int>(current.backlightHighPermille),
            static_cast<unsigned long>(current.stackFree));
        length = count > 0 ? static_cast<size_t>(count) : 0;
        // clamp truncated output to the available buffer.
        if (length >= sizeof(output)) length = sizeof(output) - 1;
        sent = 0;
        requested = false;
        lastReportMs = nowMs;
    }
    // one CDC writer; never wait for the host to read.
    const int available = Serial.availableForWrite();
    // send only the bytes the USB driver can currently accept.
    if (sent < length && available > 0) {
        size_t chunk = length - sent;
        // limit each write to one 64-byte USB chunk.
        if (chunk > 64) chunk = 64;
        // respect the remaining transmit FIFO capacity.
        if (chunk > static_cast<size_t>(available)) chunk = static_cast<size_t>(available);
        sent += Serial.write(reinterpret_cast<const uint8_t*>(output + sent), chunk);
    }
}

// BLE callbacks mark session changes; the watch task parses and applies data.
void onConnect(uint16_t) {
    connectionStartOverflow = overflowGeneration;
    ++connectionGeneration;
}
// discard disconnected BLE bytes and mark the next session boundary.
void onDisconnect(uint16_t, uint8_t) {
    // discard old bytes before the next connection.
    phoneUart.flush();
    ++connectionGeneration;
}
// mark dropped BLE bytes for watch-task resynchronization.
void onOverflow(uint16_t, uint16_t) { ++overflowGeneration; }
// supply LVGL with the core monotonic millisecond clock.
uint32_t tick() { return millis(); }

// translate guarded touch samples into LVGL pointer events.
void touchRead(lv_indev_t* input, lv_indev_data_t* data) {
    static lv_point_t lastPoint = {120, 120};
    static bool suppressUntilReleased = false;
    data->point = lastPoint;
    // suppress touches while orientation keeps the screen dim.
    if (state.displayDimmed) {
        // only orientation wakes; require finger release to prevent click-through.
        lv_indev_reset(input, nullptr);
        suppressUntilReleased = true;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    workPhase.store("touch_read", std::memory_order_relaxed);
    const board::TouchSample sample = board::readTouch();
    workPhase.store("render", std::memory_order_relaxed);
    data->point = lastPoint;
    // reject failed touch samples and cancel the current gesture.
    if (!sample.valid) {
        // cancel the gesture to prevent phantom taps after I2C errors.
        lv_indev_reset(input, nullptr);
        suppressUntilReleased = true;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    // ignore held fingers until a valid release arrives.
    if (suppressUntilReleased) {
        // rearm touch only after the finger lifts.
        if (!sample.pressed) suppressUntilReleased = false;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    data->state = sample.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    // update cached coordinates only during a valid press.
    if (sample.pressed) {
        lastPoint.x = sample.x;
        lastPoint.y = sample.y;
        data->point = lastPoint;
    }
}

// initialize browser relay and manual phone services.
void startBluetooth() {
    // keep the BLE stack available independently of the display.
    Bluefruit.autoConnLed(false);
    bluetoothReady = Bluefruit.begin(1, 0);
    // stop service setup if the BLE stack fails.
    if (!bluetoothReady) {
        diagnosticBleError = 1;
        return;
    }
    // configure BLE identity, connection callbacks and requested connection interval.
    diagnosticAddress = Bluefruit.getAddr();
    diagnosticAddressKnown = true;
    Bluefruit.setTxPower(0);
    Bluefruit.setName("XIAO Watch");
    Bluefruit.Periph.setConnectCallback(onConnect);
    Bluefruit.Periph.setDisconnectCallback(onDisconnect);
    Bluefruit.Periph.setConnInterval(24, 48);  // 30..60ms; central negotiates.
    // report Nordic UART initialization failure.
    if (phoneUart.begin() != ERROR_NONE) {
        bluetoothReady = false;
        diagnosticBleError = 2;
        return;
    }
    phoneUart.setRxOverflowCallback(onOverflow);
    // report manual GATT service initialization failure.
    if (!manualBle::begin()) {
        bluetoothReady = false;
        diagnosticBleError = 4;
        return;
    }
    // advertise NUS for browser discovery; keep the name in scan response.
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addTxPower();
    Bluefruit.Advertising.addService(phoneUart);
    Bluefruit.ScanResponse.addName();
    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(160, 800);  // 100ms fast, 500ms slow.
    Bluefruit.Advertising.setFastTimeout(30);
    bluetoothReady = Bluefruit.Advertising.start(0);
    // retain advertising startup failure for USB diagnostics.
    if (!bluetoothReady) diagnosticBleError = 3;
}

// disconnect the current BLE session and discard its outgoing frame.
void closeSession() {
    sessionClosing = true;
    closingGeneration = connectionGeneration;
    Bluefruit.disconnect(Bluefruit.connHandle());
    ackLength = ackSent = 0;
}

// queue a bounded JSON failure ACK or close a partially written stream.
void queueError(const char* reason, uint32_t nowMs) {
    // prevent an error ACK from corrupting an unfinished NUS frame.
    if (ackSent != 0 && ackSent < ackLength) {
        // close rather than append a JSON ACK to a partial NUS frame.
        closeSession();
        return;
    }
    ackLength = snprintf(pendingAck, sizeof(pendingAck),
        "{\"v\":1,\"type\":\"ack\",\"seq\":null,\"ok\":false,\"status\":\"%s\"}\n", reason);
    ackSent = 0;
    ackStarted = nowMs;
}

// process BLE frames and ACKs while preserving session boundaries.
void receivePhone(uint32_t nowMs) {
    // skip phone traffic until BLE initialization succeeds.
    if (!bluetoothReady) return;
    state.bleConnected = Bluefruit.connected() != 0;
    // reset framing and sequencing when the BLE connection changes.
    if (handledGeneration != connectionGeneration) {
        handledGeneration = connectionGeneration;
        receiver.reset();
        protocol.resetSession();
        relaySchedule.reset();
        handledOverflow = connectionStartOverflow;
        ackLength = ackSent = 0;
        // reopen processing only for a genuinely new connected session.
        if (state.bleConnected && connectionGeneration != closingGeneration) sessionClosing = false;
    }
    // wait for a connected phone before reading or writing.
    if (!state.bleConnected) return;
    // repeat disconnect requests until the closing session ends.
    if (sessionClosing) {
        Bluefruit.disconnect(Bluefruit.connHandle());
        return;
    }
    // discard an overflowed stream through its next newline and report the loss.
    if (handledOverflow != overflowGeneration) {
        handledOverflow = overflowGeneration;
        phoneUart.flush();
        receiver.discardToNewline();
        queueError("receive_overflow", nowMs);
    }
    // reject incomplete frames that exceed their receive deadline.
    if (receiver.expire(nowMs)) queueError("frame_timeout", nowMs);
    // stop if timeout reporting forced a disconnect.
    if (sessionClosing) return;
    // stream ACKs/polls in <=20-byte chunks for MTU 23; retry a full queue.
    if (ackSent < ackLength) {
        // write fragments only when the phone subscribes to notifications.
        if (phoneUart.notifyEnabled()) {
            const size_t count = (ackLength - ackSent < 20) ? ackLength - ackSent : 20;
            const uint32_t sendingGeneration = handledGeneration;
            // avoid writing after a new connection supersedes this ACK.
            if (sendingGeneration != connectionGeneration) return;
            const uint16_t sendingHandle = Bluefruit.connHandle();
            const size_t sent = phoneUart.write(sendingHandle,
                reinterpret_cast<const uint8_t*>(pendingAck + ackSent), count);
            /* a blocking write can outlive its connection and reuse its handle;
             * close the replacement session if it may have received an old ACK. */
            if (sendingGeneration != connectionGeneration) {
                closeSession();
                return;
            }
            ackSent += sent;
        }
        // defer input until the full outgoing frame has been accepted.
        if (ackSent < ackLength) {
            // close a stalled ACK stream after five seconds.
            if (static_cast<uint32_t>(nowMs - ackStarted) >= 5000) {
                closeSession();
            }
            return;
        }
        ackLength = ackSent = 0;
    }
    // bound receive work to keep touch/rendering responsive.
    for (unsigned budget = 0; budget < 256 && phoneUart.available(); ++budget) {
        // stop consuming bytes if disconnect or overflow invalidates the stream.
        if (handledGeneration != connectionGeneration || handledOverflow != overflowGeneration) return;
        const int value = phoneUart.read();
        // stop when the BLE receive FIFO empties.
        if (value < 0) break;
        const watch::FrameEvent event = receiver.push(static_cast<uint8_t>(value), nowMs);
        // report invalid bytes or oversized frames before accepting another message.
        if (event == watch::FrameEvent::TooLong || event == watch::FrameEvent::InvalidByte) {
            queueError(event == watch::FrameEvent::TooLong ? "frame_too_long" : "invalid_byte", nowMs);
            break;
        }
        // wait for a complete frame before parsing JSON.
        if (event != watch::FrameEvent::Ready) continue;
        /* parse state/sequence changes outside the lock; commit only if the BLE
         * session and FIFO stayed intact during parsing. */
        watch::WatchState nextState = state;
        watch::ProtocolSession nextProtocol = protocol;
        const watch::ProtocolResult result = nextProtocol.apply(receiver.data(), receiver.length(), nextState, nowMs);
        taskENTER_CRITICAL();
        const bool intact = handledGeneration == connectionGeneration && handledOverflow == overflowGeneration;
        // commit state and sequence together only for the original intact stream.
        if (intact) { state = nextState; protocol = nextProtocol; }
        taskEXIT_CRITICAL();
        // drop parsing results from an invalidated connection.
        if (!intact) return;
        // queue the protocol result and wait for its full ACK before further input.
        ackLength = watch::formatAcknowledgement(result, pendingAck, sizeof(pendingAck));
        ackSent = 0;
        ackStarted = nowMs;
        break;  // phone waits for the full ACK before sending again.
    }
    // notify only subscribed peers, after pending ACKs and input have drained.
    if (ackLength == 0 && !phoneUart.available() && phoneUart.notifyEnabled() && relaySchedule.due(nowMs)) {
        // keep each poll intact on the same bounded notification queue as ACKs.
        static const char poll[] = "{\"v\":1,\"type\":\"poll\",\"interval\":900}\n";
        memcpy(pendingAck, poll, sizeof(poll));
        ackLength = sizeof(poll) - 1;
        ackSent = 0;
        ackStarted = nowMs;
        relaySchedule.queued(nowMs);
        ++relayPollsQueued;
    }
}

// measure and smooth the watch battery voltage before estimating percentage.
void sampleBattery() {
    const float measured = board::readBatteryVolts();
    // preserve unknown battery state for unsupported voltage readings.
    if (watch::batteryPercentFromVoltage(measured) < 0) {
        state.batteryVoltage = measured;
        state.batteryPercent = -1;
        return;
    }
    // seed the first valid reading, then apply a 25% smoothing step.
    state.batteryVoltage = state.batteryPercent < 0 ? measured :
                           state.batteryVoltage * 0.75f + measured * 0.25f;
    state.batteryPercent = watch::batteryPercentFromVoltage(state.batteryVoltage);
}

// read hardware RTC time and update clock validity/interpolation.
void refreshRtc() {
    uint32_t utc = 0;
    const bool valid = board::readRtc(utc);
    state.rtcAvailable = board::rtcPresent();
    watch::applyRtcSample(state, valid, utc, millis());
}

// execute queued USB RTC operations in the task that owns Wire.
void processRtcUsb() {
    RtcJob job;
    // lock the shared job while checking and claiming its pending state.
    taskENTER_CRITICAL();
    // claim one pending RTC job atomically.
    if (rtcJob.phase == RtcJobState::Pending) {
        rtcJob.phase = RtcJobState::Working;
        job = rtcJob;
    }
    taskEXIT_CRITICAL();
    // return when no job was claimed.
    if (job.phase != RtcJobState::Working) return;
    workPhase.store("rtc_usb", std::memory_order_relaxed);
    // reject delayed requests to avoid setting stale time.
    const bool timely = static_cast<uint32_t>(millis() - job.receivedMs) < 2000;
    // read job state and USB generation under the same lock.
    taskENTER_CRITICAL();
    const bool sameSession = job.generation == usbGeneration && rtcJob.phase == RtcJobState::Working;
    taskEXIT_CRITICAL();
    const bool active = timely && sameSession && static_cast<bool>(Serial);
    // set or read the physical RTC only for a timely, active USB session.
    const bool written = active &&
        (job.command.operation == rtcusb::Operation::Get || board::writeRtc(job.command.utc));
    job.ok = written && board::readRtc(job.utc);
    // accept RTCSET only when readback is within one second of the requested UTC.
    if (job.ok && job.command.operation == rtcusb::Operation::Set)
        job.ok = job.utc >= job.command.utc && job.utc - job.command.utc <= 1;
    // update the watch clock only for the active request.
    if (active) {
        state.rtcAvailable = board::rtcPresent();
        watch::applyRtcSample(state, job.ok, job.utc, millis());
    }
    publishStatus(BootStage::Running);
    // publish the completed job atomically with its session check.
    taskENTER_CRITICAL();
    // reply only to the originating USB session.
    if (rtcJob.phase == RtcJobState::Working && job.generation == usbGeneration) {
        rtcJob.utc = job.utc;
        rtcJob.ok = job.ok;
        rtcJob.phase = RtcJobState::Reply;
    }
    taskEXIT_CRITICAL();
}

// initialize hardware and run the sole model/UI owner task.
void watchTask(void*) {
    // initialize the board and restore time from the hardware RTC.
    publishStatus(BootStage::BoardInit);
    board::begin();
    state.rtcAvailable = board::rtcPresent();
    publishStatus(BootStage::RtcRead);
    refreshRtc();
    // load weather and timezone without changing RTC time; never replay build timestamps.
    if (uploadweather::available) {
        uploadweather::load(state, millis());
        state.offsetSeconds = uploadweather::offsetSeconds;
        state.offsetKnown = true;
        state.nextTransitionUtc = uploadweather::nextTransitionUtc;
        state.nextOffsetSeconds = uploadweather::nextOffsetSeconds;
    }
    // sample battery, initialize orientation and start BLE.
    publishStatus(BootStage::BatteryRead);
    sampleBattery();
    publishStatus(BootStage::OrientationInit);
    board::beginOrientation();
    lastOrientationRetryMs = millis();
    publishStatus(BootStage::BluetoothInit);
    startBluetooth();
    publishStatus(BootStage::DisplayInit);
    // initialize LVGL, replace vendor flush/touch callbacks and reserve PWM.
    lv_init();
    lv_tick_set_cb(tick);
    screen_rotation = 0;
    lv_xiao_disp_init();
    backlightReady = board::beginBacklight();
    lv_display_set_flush_cb(lv_display_get_default(), displayFlush);
    lv_indev_t* input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, touchRead);
    // construct the initial view after display and input callbacks are ready.
    publishStatus(BootStage::UiInit);
    ui::begin(state);
    publishStatus(BootStage::UiReady);
    // retain independent polling timestamps for battery, RTC, UI and diagnostics.
    uint32_t lastBattery = millis();
    uint32_t lastRtc = lastBattery;
    uint32_t lastUi = lastBattery;
    uint32_t lastStatus = lastBattery;
    // service hardware and the UI while yielding regularly to other RTOS tasks.
    for (;;) {
        const uint32_t nowMs = millis();
        watch::advanceClock(state, nowMs);
        workPhase.store("orientation", std::memory_order_relaxed);
        // retry missing or stale orientation hardware every five seconds.
        if ((!board::orientationReady() || !orientationWake.healthy()) &&
            static_cast<uint32_t>(nowMs - lastOrientationRetryMs) >= 5000) {
            lastOrientationRetryMs = nowMs;
            board::beginOrientation();
        }
        sampleOrientation();
        // apply posture and service queued USB/BLE work before periodic refreshes.
        applyOrientation();
        processRtcUsb();
        workPhase.store("phone", std::memory_order_relaxed);
        receivePhone(nowMs);
        manualBle::poll(state, nowMs);
        // sample battery voltage every ten seconds.
        if (static_cast<uint32_t>(nowMs - lastBattery) >= 10000) {
            lastBattery = nowMs;
            workPhase.store("battery", std::memory_order_relaxed);
            sampleBattery();
        }
        // refresh the model from the hardware RTC every second.
        if (static_cast<uint32_t>(nowMs - lastRtc) >= 1000) {
            lastRtc = nowMs;
            workPhase.store("rtc", std::memory_order_relaxed);
            refreshRtc();
        }
        // refresh UI labels and hands every 200 ms.
        if (static_cast<uint32_t>(nowMs - lastUi) >= 200) {
            lastUi = nowMs;
            workPhase.store("ui_update", std::memory_order_relaxed);
            ui::update(state);
        }
        // run LVGL drawing and measure its longest handler duration.
        workPhase.store("render", std::memory_order_relaxed);
        const uint32_t renderStartedMs = millis();
        lv_timer_handler();
        const uint32_t renderMs = millis() - renderStartedMs;
        // retain the worst observed render duration.
        if (renderMs > maxRenderMs) maxRenderMs = renderMs;
        // apply posture sampled during drawing before queued navigation.
        applyOrientation();
        workPhase.store("navigation", std::memory_order_relaxed);
        ui::processNavigation();  // outside input-event dispatch.
        ++watchLoops;
        // publish loop progress atomically so USB can detect driver stalls.
        watchProgressMs = millis();
        taskENTER_CRITICAL();
        usbStatus.loops = watchLoops;
        usbStatus.progressMs = watchProgressMs;
        taskEXIT_CRITICAL();
        // publish full diagnostics on the first loop and once per second.
        if (watchLoops == 1 || static_cast<uint32_t>(watchProgressMs - lastStatus) >= 1000) {
            lastStatus = watchProgressMs;
            publishStatus(BootStage::Running);
        }
        workPhase.store("waiting", std::memory_order_relaxed);
        delay(5);                // yield to BLE/USB and other RTOS tasks.
    }
}
}  // namespace

// start independent USB diagnostics and the larger watch application task.
void setup() {
    Serial.begin(115200);  // boot on battery without waiting for USB.
    /* the core disables time slicing; a higher-priority USB loop can report
     * cached state even when a driver stalls the watch task. */
    vTaskPrioritySet(nullptr, TASK_PRIO_NORMAL);
    // give LVGL/JSON a 12 KiB task stack instead of the core's fixed 4 KiB loop stack.
    if (xTaskCreate(watchTask, "watch", 3072, nullptr, 1, nullptr) != pdPASS) {
        taskENTER_CRITICAL();
        usbStatus.stage = BootStage::TaskCreateFailed;
        usbStatus.capturedMs = millis();
        taskEXIT_CRITICAL();
    }
}

// keep USB responsive while yielding between diagnostic iterations.
void loop() {
    usbDiagnostics();
    delay(10);
}
