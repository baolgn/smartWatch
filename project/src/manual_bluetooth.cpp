#include "manual_bluetooth.h"

#include <Arduino.h>
#include <bluefruit.h>
#include <cstring>
#include "manual_protocol.h"

// LightBlue GATT service and watch-task command processing.
namespace manualBle {
// keep transport state private to this service.
namespace {
// identify the service and command/result characteristics.
constexpr char kServiceUuid[] = "A7E40001-6B18-4F8D-A91F-3E32C2D4B620";
constexpr char kCommandUuid[] = "A7E40002-6B18-4F8D-A91F-3E32C2D4B620";
constexpr char kResultUuid[] = "A7E40003-6B18-4F8D-A91F-3E32C2D4B620";
constexpr unsigned kQueueSize = 4;
// store one bounded GATT write.
struct Command {
    char data[watch::kMaxManualCommandBytes];
    uint16_t length;
};
// share queue/session counters under critical sections.
Command queue[kQueueSize]{};
unsigned head = 0, count = 0;
uint16_t connection = BLE_CONN_HANDLE_INVALID;
uint32_t generation = 0;
uint32_t overflow = 0;
uint32_t sessionStartOverflow = 0;
uint32_t handledGeneration = 0;
uint32_t handledOverflow = 0;
// retain readiness, applied count and latest watch-task result.
bool initialized = false;
uint32_t applied = 0;
char resultText[21] = "READY";
watch::ManualSession session;

// atomically replace the session and discard queued commands.
void changeConnection(uint16_t handle) {
    // guard shared queue/session state against concurrent BLE writes and polling.
    taskENTER_CRITICAL();
    connection = handle;
    ++generation;
    sessionStartOverflow = overflow;
    head = count = 0;
    taskEXIT_CRITICAL();
}

/* service hooks preserve BLE event order; deferred global connection callbacks
 * can arrive after the first GATT write. */
class ManualService : public BLEService {
 public:
    // bind the manual service UUID.
    ManualService() : BLEService(kServiceUuid) {}
 protected:
    // replace queue ownership before this connection can write.
    void svc_connect_hdl(uint16_t handle) override { changeConnection(handle); }
    // invalidate the session and queued data on disconnect.
    void svc_disconnect_hdl(uint16_t) override {
        changeConnection(BLE_CONN_HANDLE_INVALID);
    }
};
// retain GATT objects for the service lifetime.
ManualService service;
BLECharacteristic commandCharacteristic(kCommandUuid);
BLECharacteristic resultCharacteristic(kResultUuid);

// direct BLE callback: bounded queue copies only; no parsing or UI work.
void onWrite(uint16_t handle, BLECharacteristic*, uint8_t* data, uint16_t length) {
    // serialize writes with queue consumption and connection changes.
    taskENTER_CRITICAL();
    // accept only the current connection.
    if (handle == connection) {
        // invalidate queued data on overflow or oversized writes.
        if (count == kQueueSize || length > watch::kMaxManualCommandBytes) {
            ++overflow;
            head = count = 0;
        }
        // enqueue the bounded payload.
        else {
            Command& entry = queue[(head + count) % kQueueSize];
            entry.length = length;
            // copy only supplied bytes; empty commands are rejected during parsing.
            if (length) std::memcpy(entry.data, data, length);
            ++count;
        }
    }
    taskEXIT_CRITICAL();
}

// publish a bounded result for READ; no notification subscription needed.
void publish(const char* text) {
    const size_t length = std::strlen(text);
    // cache only a fully written characteristic value.
    if (length <= 20 && resultCharacteristic.write(text) == length) {
        std::memcpy(resultText, text, length + 1);
    }
}

// check session continuity under the caller's critical section.
bool intact(uint32_t expectedGeneration, uint32_t expectedOverflow) {
    return generation == expectedGeneration && overflow == expectedOverflow &&
           connection != BLE_CONN_HANDLE_INVALID;
}
}  // namespace

// register command/result characteristics before advertising.
bool begin() {
    // stop if service registration fails.
    if (service.begin() != ERROR_NONE) return false;
    /* the core copies read permission into the user descriptor.
     * READ exposes the last command; Result holds its outcome. */
    commandCharacteristic.setProperties(CHR_PROPS_READ | CHR_PROPS_WRITE);
    commandCharacteristic.setPermission(SECMODE_OPEN, SECMODE_OPEN);
    commandCharacteristic.setMaxLen(watch::kMaxManualCommandBytes);
    commandCharacteristic.setWriteCallback(onWrite, false);
    commandCharacteristic.setUserDescriptor("Command (ASCII)");
    // stop if command registration fails.
    if (commandCharacteristic.begin() != ERROR_NONE) return false;
    // expose results as read-only ASCII.
    resultCharacteristic.setProperties(CHR_PROPS_READ);
    resultCharacteristic.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
    resultCharacteristic.setMaxLen(20);
    resultCharacteristic.setUserDescriptor("Result (ASCII)");
    // stop if result registration fails.
    if (resultCharacteristic.begin() != ERROR_NONE) return false;
    initialized = resultCharacteristic.write("READY") == 5;
    return initialized;
}

// consume at most one command in the watch task and publish its result.
void poll(watch::WatchState& state, uint32_t nowMs) {
    // skip an unavailable service.
    if (!initialized) return;
    // snapshot connection and overflow counters atomically.
    uint32_t currentGeneration, currentOverflow, startOverflow;
    bool connected;
    taskENTER_CRITICAL();
    currentGeneration = generation;
    currentOverflow = overflow;
    startOverflow = sessionStartOverflow;
    connected = connection != BLE_CONN_HANDLE_INVALID;
    taskEXIT_CRITICAL();
    // reset staged data when the BLE session changes.
    if (handledGeneration != currentGeneration) {
        session.reset();
        handledGeneration = currentGeneration;
        handledOverflow = startOverflow;
        publish(connected ? "READY" : "DISCONNECTED");
    }
    // disconnected sessions cannot consume commands.
    if (!connected) return;
    // discard queued/staged data after command loss to prevent partial forecasts.
    if (handledOverflow != currentOverflow) {
        taskENTER_CRITICAL();
        const bool sameSession = generation == currentGeneration;
        // clear only the session whose overflow was observed.
        if (sameSession) { head = count = 0; handledOverflow = overflow; }
        taskEXIT_CRITICAL();
        // invalidate staging and report command loss outside the lock.
        if (sameSession) { session.reset(); publish("ERR QUEUE FULL"); }
        return;
    }
    // report expired forecast staging.
    if (session.expire(nowMs)) publish("ERR STAGE EXPIRED");
    // dequeue only while session and stream remain intact.
    Command next{};
    bool haveCommand = false;
    taskENTER_CRITICAL();
    // remove one command atomically.
    if (intact(currentGeneration, currentOverflow) && count) {
        next = queue[head];
        head = (head + 1) % kQueueSize;
        --count;
        haveCommand = true;
    }
    taskEXIT_CRITICAL();
    // leave when the queue has no usable command.
    if (!haveCommand) return;
    // parse into copies outside the critical section.
    watch::WatchState nextState = state;
    watch::ManualSession nextSession = session;
    const watch::ManualResult reply = nextSession.apply(next.data, next.length,
                                                       nextState, nowMs);
    // commit only if the session and queue stayed intact during parsing.
    taskENTER_CRITICAL();
    const bool canCommit = intact(currentGeneration, currentOverflow);
    // apply both live and staged changes atomically.
    if (canCommit) { state = nextState; session = nextSession; }
    taskEXIT_CRITICAL();
    // discard parsing results after a session change or overflow.
    if (!canCommit) return;
    // count accepted commands.
    if (reply.ok) ++applied;
    // recheck session ownership before publishing.
    taskENTER_CRITICAL();
    const bool sameSession = intact(currentGeneration, currentOverflow);
    taskEXIT_CRITICAL();
    /* publish to the same session; later changes publish READY next poll.
     * applied data stays cached through disconnects. */
    if (sameSession) publish(reply.reply);
}

// report successful GATT initialization.
bool ready() { return initialized; }
// report accepted commands.
uint32_t appliedCount() { return applied; }
// expose the watch-task result; USB must copy it into its snapshot.
const char* lastResult() { return resultText; }
}  // namespace manualBle
