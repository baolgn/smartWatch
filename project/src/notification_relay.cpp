#include "notification_relay.h"

#include <Arduino.h>
#include <bluefruit.h>
#include <string.h>

// isolate bounded ANCS work from the watch task and the core's unsafe discovery waits.
namespace notificationRelay {
// own persistent ATT state; no callback retains a stack/heap request buffer.
namespace {
// retain UUIDs, response storage and diagnostics for the worker lifetime.
BLEClientService solicitation(BLEANCS_UUID_SERVICE);  // UUID only; never registered as a client.
BLEUuid controlUuid(BLEANCS_UUID_CHR_CONTROL);
BLEUuid notificationUuid(BLEANCS_UUID_CHR_NOTIFICATION);
BLEUuid dataUuid(BLEANCS_UUID_CHR_DATA);
BLEUuid gattUuid(static_cast<uint16_t>(0x1801));
BLEUuid changedUuid(static_cast<uint16_t>(0x2A05));
AttributeFrame response;
Diagnostics status;

// cap request time, discovery pagination and retained notification identities.
constexpr uint32_t kRequestTimeoutMs = 3000;
constexpr uint32_t kBodyTimeoutMs = 15000;
constexpr size_t kQueueSize = 8;
constexpr size_t kDiscoveryItems = 64;
// share connection generations, queued identities and one pending frame under locks.
uint16_t connection = BLE_CONN_HANDLE_INVALID;
uint32_t epoch = 0;
bool restart = false;
bool requestActive = false;
uint32_t requestEpoch = 0;
uint32_t uids[kQueueSize]{};
uint8_t queued = 0;
char pending[kMaxNotificationBytes + 1]{};
size_t pendingLength = 0;
uint32_t pendingEpoch = 0;

// retain discovered handles only for the current direct GAP generation.
struct Characteristic {
  uint16_t declaration = 0;
  uint16_t value = 0;
  uint16_t end = 0;
  uint8_t properties = 0;
};
// replace all discovered values and subscription state at connection boundaries.
Characteristic control, notifications, dataSource, changed;
bool serviceChangeSubscribed = false;

// one ATT operation at a time; callbacks copy only into these persistent fields.
enum class Operation : uint8_t { None, Service, Characteristics, Descriptors, Write };
// persist reply storage until the worker consumes the matching operation.
struct AttReply {
  Operation operation = Operation::None;
  uint32_t generation = 0;
  bool complete = false;
  uint16_t status = BLE_GATT_STATUS_SUCCESS;
  uint16_t count = 0;
  uint16_t handle = 0;
  ble_uuid_t uuid{};
  ble_gattc_handle_range_t range{};
  ble_gattc_char_t characteristics[kDiscoveryItems]{};
  ble_gattc_desc_t descriptors[kDiscoveryItems]{};
} att;

// compare identity without retaining a deletable BLEConnection pointer.
bool current(uint32_t expected, uint16_t handle) {
  taskENTER_CRITICAL();
  const bool valid = epoch == expected && connection == handle && !restart;
  taskEXIT_CRITICAL();
  return valid;
}

// caller excludes BLE/watch tasks; invalidate requests before handle reuse.
void clearTransport() {
  ++epoch;
  requestActive = false;
  att.operation = Operation::None;
  queued = 0;
  pendingLength = 0;
  status.subscribed = false;
  serviceChangeSubscribed = false;
  control = notifications = dataSource = changed = Characteristic{};
}

// never retry a timed-out ATT operation on the same connection.
void failSession(uint32_t expected) {
  taskENTER_CRITICAL();
  // ignore failures superseded by an actual disconnect/reconnect.
  if (epoch == expected) {
    ++status.errors;
    clearTransport();
    restart = true;
  }
  taskEXIT_CRITICAL();
}

// BLE callback holds scheduling; queue identities without fetching unrelated bodies.
void onNotification(const uint8_t* bytes, uint16_t length) {
  // ANCS event packets have exactly eight bytes.
  if (!bytes || length != 8 || bytes[0] > ANCS_EVT_NOTIFICATION_REMOVED) return;
  // decode the notification identity without unaligned access.
  const uint32_t uid = static_cast<uint32_t>(bytes[4]) |
      (static_cast<uint32_t>(bytes[5]) << 8) |
      (static_cast<uint32_t>(bytes[6]) << 16) |
      (static_cast<uint32_t>(bytes[7]) << 24);
  // removed notifications need no lookup.
  if (bytes[0] == ANCS_EVT_NOTIFICATION_REMOVED) {
    // compact at most eight queued identifiers.
    for (uint8_t i = 0; i < queued; ++i) {
      // discard the matching queued lookup.
      if (uids[i] == uid) {
        memmove(uids + i, uids + i + 1, (queued - i - 1) * sizeof(uids[0]));
        --queued;
        break;
      }
    }
    return;
  }
  // coalesce modifications still awaiting their first lookup.
  for (uint8_t i = 0; i < queued; ++i) {
    // the pending lookup will retrieve the latest attributes.
    if (uids[i] == uid) return;
  }
  // preserve the newest arrivals when the fixed queue fills.
  if (queued == kQueueSize) {
    memmove(uids, uids + 1, (kQueueSize - 1) * sizeof(uids[0]));
    --queued;
    ++status.dropped;
  }
  uids[queued++] = uid;
}

// process raw events after the core, before the BLE task yields.
void onBleEvent(ble_evt_t* event) {
  const uint16_t handle = event->evt.common_evt.conn_handle;
  const uint16_t id = event->header.evt_id;
  // exclude other tasks without masking the SoftDevice's priority-4 SVC.
  vTaskSuspendAll();
  // direct GAP events invalidate frames before deferred application callbacks.
  if (id == BLE_GAP_EVT_CONNECTED) {
    clearTransport();
    connection = handle;
    restart = false;
    status.secured = false;
  }
  // clear handles at the physical disconnect boundary.
  else if (id == BLE_GAP_EVT_DISCONNECTED && handle == connection) {
    clearTransport();
    connection = BLE_CONN_HANDLE_INVALID;
    restart = false;
    status.secured = false;
  }
  // track encryption changes on the current connection.
  else if (id == BLE_GAP_EVT_CONN_SEC_UPDATE && handle == connection) {
    // security state has already been updated by the core's event handler.
    BLEConnection* peer = Bluefruit.Connection(handle);
    const bool encrypted = peer && peer->secured();
    // revoke content if encryption is lost within an active connection.
    if (status.secured && !encrypted) { clearTransport(); restart = true; }
    status.secured = encrypted;
  }
  // route only secured values from this connection.
  else if (id == BLE_GATTC_EVT_HVX && handle == connection && !restart && status.secured) {
    // handle only our discovered values; no framework client callbacks are registered.
    const ble_gattc_evt_hvx_t& hvx = event->evt.gattc_evt.params.hvx;
    // confirm Service Changed exactly once, then reconnect with fresh handles.
    if (hvx.type == BLE_GATT_HVX_INDICATION) {
      // a bonded peer may restore indications before discovery; acknowledge then rediscover.
      sd_ble_gattc_hv_confirm(handle, hvx.handle);
      clearTransport();
      restart = true;
    }
    // metadata may arrive immediately after subscription.
    else if (hvx.type == BLE_GATT_HVX_NOTIFICATION && notifications.value != 0 && hvx.handle == notifications.value) {
      onNotification(hvx.data, hvx.len);
    }
    // route fragments only to their active request.
    else if (hvx.type == BLE_GATT_HVX_NOTIFICATION && dataSource.value != 0 && hvx.handle == dataSource.value &&
               requestActive && requestEpoch == epoch) {
      // the parser tolerates split headers and rejects mismatched request identities.
      response.push(hvx.data, hvx.len);
    }
  }
  // accept replies only for this generation's active ATT operation.
  else if (handle == connection && !restart && att.generation == epoch &&
             att.operation != Operation::None && !att.complete) {
    // match the outstanding operation before copying any reply.
    const ble_gattc_evt_t& reply = event->evt.gattc_evt;
    bool matched = false;
    // service queries filter one UUID and expect exactly one matching service.
    if (id == BLE_GATTC_EVT_PRIM_SRVC_DISC_RSP && att.operation == Operation::Service) {
      matched = true;
      const auto& result = reply.params.prim_srvc_disc_rsp;
      att.count = reply.gatt_status == BLE_GATT_STATUS_SUCCESS ? result.count : 0;
      // copy no variable-length data unless its count and identity are valid.
      if (reply.gatt_status == BLE_GATT_STATUS_SUCCESS && result.count == 1 &&
          result.services[0].uuid.uuid == att.uuid.uuid && result.services[0].uuid.type == att.uuid.type) {
        att.range = result.services[0].handle_range;
      }
      // reject successful replies with an unexpected service.
      else if (reply.gatt_status == BLE_GATT_STATUS_SUCCESS) {
        att.count = 0;
      }
    }
    // copy one bounded characteristic page.
    else if (id == BLE_GATTC_EVT_CHAR_DISC_RSP && att.operation == Operation::Characteristics) {
      matched = true;
      const auto& result = reply.params.char_disc_rsp;
      att.count = reply.gatt_status == BLE_GATT_STATUS_SUCCESS && result.count <= kDiscoveryItems ? result.count : 0;
      // failure replies carry no trusted elements.
      if (reply.gatt_status == BLE_GATT_STATUS_SUCCESS && att.count)
        memcpy(att.characteristics, result.chars, att.count * sizeof(ble_gattc_char_t));
    }
    // copy one bounded descriptor page.
    else if (id == BLE_GATTC_EVT_DESC_DISC_RSP && att.operation == Operation::Descriptors) {
      matched = true;
      const auto& result = reply.params.desc_disc_rsp;
      att.count = reply.gatt_status == BLE_GATT_STATUS_SUCCESS && result.count <= kDiscoveryItems ? result.count : 0;
      // failure replies carry no trusted elements.
      if (reply.gatt_status == BLE_GATT_STATUS_SUCCESS && att.count)
        memcpy(att.descriptors, result.descs, att.count * sizeof(ble_gattc_desc_t));
    }
    // match the write's handle and opcode, including ATT errors.
    else if (id == BLE_GATTC_EVT_WRITE_RSP && att.operation == Operation::Write &&
               ((reply.gatt_status == BLE_GATT_STATUS_SUCCESS && reply.params.write_rsp.handle == att.handle &&
                 reply.params.write_rsp.write_op == BLE_GATT_OP_WRITE_REQ) ||
                (reply.gatt_status != BLE_GATT_STATUS_SUCCESS && reply.error_handle == att.handle))) {
      matched = true;
    }
    // late unrelated responses remain ignored without touching caller memory.
    if (matched) { att.status = reply.gatt_status; att.complete = true; }
  }
  xTaskResumeAll();
}

// arm before issuing the SoftDevice call so immediate replies cannot be missed.
bool arm(Operation operation, uint32_t expected, uint16_t handle) {
  taskENTER_CRITICAL();
  const bool valid = epoch == expected && connection == handle && !restart;
  // refuse operations belonging to a superseded connection.
  if (valid) {
    att.operation = operation;
    att.generation = expected;
    att.complete = false;
    att.count = 0;
    att.status = BLE_GATT_STATUS_SUCCESS;
  }
  taskEXIT_CRITICAL();
  return valid;
}

// wait only in the worker; timeout invalidates the connection and all late replies.
bool awaitAtt(uint16_t handle, uint32_t expected) {
  const uint32_t started = millis();
  // fixed deadlines bound loss, missing permissions and unexpected responses.
  while (current(expected, handle)) {
    taskENTER_CRITICAL();
    const bool complete = att.complete;
    // persistent reply data remains stable after disarming.
    if (complete) att.operation = Operation::None;
    taskEXIT_CRITICAL();
    // explicit ATT failures still complete; the caller interprets status.
    if (complete) return true;
    // do not reuse the transaction slot after a timeout on the same link.
    if (static_cast<uint32_t>(millis() - started) >= kRequestTimeoutMs) {
      failSession(expected);
      return false;
    }
    delay(5);
  }
  return false;
}

// discover one filtered service without the core's transient AdaMsg buffers.
bool service(uint16_t handle, uint32_t expected, BLEUuid& uuid, ble_gattc_handle_range_t& range) {
  // suspend scheduling, not interrupts: BASEPRI critical sections block SVC.
  vTaskSuspendAll();
  att.uuid = uuid._uuid;
  const bool armed = arm(Operation::Service, expected, handle);
  // submission and generation check must exclude disconnect/handle reuse.
  const uint32_t submitted = armed ? sd_ble_gattc_primary_services_discover(handle, 1, &att.uuid) : 1;
  xTaskResumeAll();
  // an already replaced connection must not receive this request.
  if (!armed) return false;
  // submission errors cannot leave an active request slot behind.
  if (submitted != NRF_SUCCESS) {
    failSession(expected);
    return false;
  }
  // absence is retryable; malformed successes and timeouts require reconnection.
  if (!awaitAtt(handle, expected)) return false;
  // iOS may publish ANCS later; Service Changed and periodic retry handle that case.
  if (att.status == BLE_GATT_STATUS_ATTERR_ATTRIBUTE_NOT_FOUND) return false;
  // require a complete, ordered service handle range.
  if (att.status != BLE_GATT_STATUS_SUCCESS || att.count != 1 || att.range.start_handle == 0 ||
      att.range.start_handle > att.range.end_handle) { failSession(expected); return false; }
  range = att.range;
  return current(expected, handle);
}

// discover all characteristic declarations, retaining precise descriptor boundaries.
bool characteristics(uint16_t handle, uint32_t expected, ble_gattc_handle_range_t range,
                     bool isAncs) {
  const uint16_t serviceEnd = range.end_handle;
  // bound pagination even if an unexpected peer exposes an excessive table.
  for (unsigned page = 0; page < 32 && current(expected, handle); ++page) {
    // keep lifecycle tasks out while leaving SoftDevice interrupts and SVC enabled.
    vTaskSuspendAll();
    const bool armed = arm(Operation::Characteristics, expected, handle);
    // guard submission against a new connection reusing this handle.
    const uint32_t submitted = armed ? sd_ble_gattc_characteristics_discover(handle, &range) : 1;
    xTaskResumeAll();
    // do not access framework Discovery or submit for an invalid generation.
    if (!armed) return false;
    // queue one discovery page and await its matching event.
    if (submitted != NRF_SUCCESS) {
      failSession(expected); return false;
    }
    // transport failure already invalidates the session.
    if (!awaitAtt(handle, expected)) return false;
    // ATTRIBUTE_NOT_FOUND marks the end of a paginated service.
    if (att.status == BLE_GATT_STATUS_ATTERR_ATTRIBUTE_NOT_FOUND) return true;
    // reject malformed pages instead of advancing through untrusted handles.
    if (att.status != BLE_GATT_STATUS_SUCCESS || att.count == 0) { failSession(expected); return false; }
    uint16_t last = 0;
    // finalize previous characteristics at every following declaration.
    for (uint16_t i = 0; i < att.count; ++i) {
      const ble_gattc_char_t& item = att.characteristics[i];
      // values must follow declarations and stay in the requested service.
      if (item.handle_decl < range.start_handle || item.handle_decl <= last ||
          item.handle_value <= item.handle_decl || item.handle_value > serviceEnd) {
        failSession(expected); return false;
      }
      Characteristic* values[] = {&control, &notifications, &dataSource, &changed};
      taskENTER_CRITICAL();
      // a direct disconnect may have reset these handles during discovery.
      if (epoch != expected || restart) { taskEXIT_CRITICAL(); return false; }
      // close only values belonging to this service, including across pages.
      for (auto* previous : values) {
        // the next declaration owns every following descriptor.
        if (previous->value && previous->value < item.handle_decl && previous->end == serviceEnd &&
            (isAncs ? previous != &changed : previous == &changed)) previous->end = item.handle_decl - 1;
      }
      Characteristic* found = nullptr;
      // retain the ANCS Control Point.
      if (isAncs && controlUuid == item.uuid) found = &control;
      // retain notification metadata routing.
      else if (isAncs && notificationUuid == item.uuid) found = &notifications;
      // retain requested attribute routing.
      else if (isAncs && dataUuid == item.uuid) found = &dataSource;
      // retain standard GATT Service Changed indications.
      else if (!isAncs && changedUuid == item.uuid) found = &changed;
      // duplicate relevant characteristics would make routing ambiguous.
      if (found) {
        // reject duplicates before changing an established value handle.
        if (found->value) { taskEXIT_CRITICAL(); failSession(expected); return false; }
        found->declaration = item.handle_decl;
        found->value = item.handle_value;
        found->end = serviceEnd;
        memcpy(&found->properties, &item.char_props, sizeof(found->properties));
      }
      taskEXIT_CRITICAL();
      last = item.handle_value;
    }
    // stop at the final service value without wrapping the 16-bit handle.
    if (last >= serviceEnd) return true;
    range.start_handle = last + 1;
  }
  failSession(expected);
  return false;
}

// find a fresh CCCD strictly inside one characteristic's descriptor range.
uint16_t freshCccd(uint16_t handle, uint32_t expected, const Characteristic& value) {
  // prevent overflow when the characteristic has no descriptor space.
  if (value.value == 0 || value.value >= value.end) return 0;
  ble_gattc_handle_range_t range{static_cast<uint16_t>(value.value + 1), value.end};
  // cap descriptor pagination independently of characteristic discovery.
  for (unsigned page = 0; page < 8 && current(expected, handle); ++page) {
    // exclude lifecycle task switches without masking SVC.
    vTaskSuspendAll();
    const bool armed = arm(Operation::Descriptors, expected, handle);
    // persist the request and submit before any lifecycle callback can intervene.
    const uint32_t submitted = armed ? sd_ble_gattc_descriptors_discover(handle, &range) : 1;
    xTaskResumeAll();
    // superseded operations never reach the SoftDevice.
    if (!armed) return 0;
    // submission failure invalidates the operation immediately.
    if (submitted != NRF_SUCCESS) { failSession(expected); return 0; }
    // require an acknowledged successful discovery page.
    if (!awaitAtt(handle, expected) || att.status != BLE_GATT_STATUS_SUCCESS || att.count == 0) return 0;
    uint16_t last = 0;
    // choose only a SIG CCCD within this exact characteristic.
    for (uint16_t i = 0; i < att.count; ++i) {
      const ble_gattc_desc_t& item = att.descriptors[i];
      // reject unordered or out-of-bounds descriptor replies.
      if (item.handle < range.start_handle || item.handle > range.end_handle || item.handle <= last) return 0;
      last = item.handle;
      // unrelated vendor descriptors cannot satisfy a subscription.
      if (item.uuid.type == BLE_UUID_TYPE_BLE && item.uuid.uuid == BLE_UUID_DESCRIPTOR_CLIENT_CHAR_CONFIG)
        return item.handle;
    }
    // stop without wrapping the final handle.
    if (last >= range.end_handle) return 0;
    range.start_handle = last + 1;
  }
  return 0;
}

// use raw WRITE_REQ so ATT errors and immediate responses are observed safely.
bool write(uint16_t handle, uint32_t expected, uint16_t valueHandle,
           const uint8_t* bytes, uint16_t length) {
  // a zero handle is never a writable value or descriptor.
  if (!valueHandle) return false;
  // prepare a bounded write request; the SoftDevice copies its bytes immediately.
  ble_gattc_write_params_t params{};
  params.write_op = BLE_GATT_OP_WRITE_REQ;
  params.handle = valueHandle;
  params.len = length;
  params.p_value = bytes;
  // preserve generation atomicity while allowing the SoftDevice SVC to run.
  vTaskSuspendAll();
  att.handle = valueHandle;
  const bool armed = arm(Operation::Write, expected, handle);
  // couple the generation check and immediate copy into the SoftDevice.
  const uint32_t submitted = armed ? sd_ble_gattc_write(handle, &params) : 1;
  xTaskResumeAll();
  // a replaced connection must never receive an old-handle write.
  if (!armed) return false;
  // the SoftDevice copies this bounded command before returning.
  if (submitted != NRF_SUCCESS) { failSession(expected); return false; }
  return awaitAtt(handle, expected) && att.status == BLE_GATT_STATUS_SUCCESS && current(expected, handle);
}

// subscribe only after confirming the fresh descriptor and its ATT write.
bool subscribe(uint16_t handle, uint32_t expected, const Characteristic& value, bool indicate) {
  const uint8_t property = indicate ? 0x20 : 0x10;
  // refuse a characteristic with the wrong operation properties.
  if (!(value.properties & property)) return false;
  const uint16_t descriptor = freshCccd(handle, expected, value);
  const uint8_t setting[2] = {static_cast<uint8_t>(indicate ? 2 : 1), 0};
  return descriptor != 0 && write(handle, expected, descriptor, setting, sizeof(setting));
}

// retrieve one exact attribute with a total deadline across all fragments.
bool attribute(uint16_t handle, uint32_t expected, uint32_t uid, uint8_t id,
               uint16_t limit, char* output, size_t capacity, size_t& length) {
  // caller buffers reserve an extra local terminator.
  if (!output || capacity <= limit || !current(expected, handle)) return false;
  // encode the UID and optional length, with one total response deadline.
  uint8_t command[8] = {0, static_cast<uint8_t>(uid), static_cast<uint8_t>(uid >> 8),
      static_cast<uint8_t>(uid >> 16), static_cast<uint8_t>(uid >> 24), id,
      static_cast<uint8_t>(limit), static_cast<uint8_t>(limit >> 8)};
  const uint16_t commandLength = id == ANCS_ATTR_TITLE || id == ANCS_ATTR_SUBTITLE || id == ANCS_ATTR_MESSAGE ? 8 : 6;
  const uint32_t started = millis();
  const uint32_t timeout = id == ANCS_ATTR_MESSAGE ? kBodyTimeoutMs : kRequestTimeoutMs;
  taskENTER_CRITICAL();
  // arm before writing: Data Source can arrive before the ATT write response.
  if (epoch != expected || restart) { taskEXIT_CRITICAL(); return false; }
  response.start(uid, id, limit);
  requestActive = true;
  requestEpoch = expected;
  taskEXIT_CRITICAL();
  // failed requests need a fresh link to discard any late fragments.
  if (!write(handle, expected, control.value, command, commandLength)) { failSession(expected); return false; }
  // wait here, never in the watch task or BLE callback.
  while (current(expected, handle)) {
    taskENTER_CRITICAL();
    const bool failed = response.failed();
    // copy only a complete response belonging to this active request.
    if (!failed && response.complete() && epoch == expected && requestActive) {
      length = response.length();
      memcpy(output, response.data(), length + 1);
      requestActive = false;
      taskEXIT_CRITICAL();
      return true;
    }
    taskEXIT_CRITICAL();
    // an absolute deadline rejects endless partial streams.
    if (failed || static_cast<uint32_t>(millis() - started) >= timeout) { failSession(expected); return false; }
    delay(5);
  }
  return false;
}

// fetch metadata first; only the dedicated ntfy title may expose a body.
void fetch(uint16_t handle, uint32_t expected, uint32_t uid) {
  // bound metadata independently of the larger message buffer.
  char app[129]{}, title[65]{}, sizeText[17]{};
  size_t appLength = 0, titleLength = 0, sizeLength = 0;
  // app identifiers have no request-length parameter, so the decoder enforces it.
  if (!attribute(handle, expected, uid, ANCS_ATTR_APP_IDENTIFIER, 128, app, sizeof(app), appLength)) return;
  // retain only a fingerprint and byte count for unrelated app identifiers.
  const uint32_t appHash = metadataFingerprint(app, appLength);
  taskENTER_CRITICAL();
  // reset title diagnostics before examining each new app identifier.
  status.lastAppHash = appHash;
  status.lastAppLength = static_cast<uint16_t>(appLength);
  status.lastTitleHash = 0;
  status.lastTitleLength = 0;
  status.lastMetadataFlags = appLength && app[appLength - 1] == '\0' ? 1 : 0;
  taskEXIT_CRITICAL();
  // skip other applications without reading their titles or bodies.
  if (appLength != strlen(kSourceApp) || memcmp(app, kSourceApp, appLength) != 0) {
    taskENTER_CRITICAL(); ++status.filtered; ++status.filteredApp; taskEXIT_CRITICAL(); return;
  }
  // title filtering separates relay data from unrelated ntfy topics.
  if (!attribute(handle, expected, uid, ANCS_ATTR_TITLE, 64, title, sizeof(title), titleLength)) return;
  // diagnose exact ntfy title bytes without exposing their contents.
  const uint32_t titleHash = metadataFingerprint(title, titleLength);
  const bool pairedBidi = titleLength >= 6 && memcmp(title, "\xE2\x80\xAD", 3) == 0 &&
      memcmp(title + titleLength - 3, "\xE2\x80\xAC", 3) == 0;
  taskENTER_CRITICAL();
  // only the exact ntfy app may populate title diagnostics; preserve strict filtering.
  status.lastTitleHash = titleHash;
  status.lastTitleLength = static_cast<uint16_t>(titleLength);
  status.lastMetadataFlags |= (titleLength && title[titleLength - 1] == '\0' ? 2 : 0) |
      (pairedBidi ? 4 : 0);
  taskEXIT_CRITICAL();
  // accept only the exact title and optional paired iOS bidi markers.
  if (!metadataMatches(app, appLength, title, titleLength)) {
    taskENTER_CRITICAL(); ++status.filtered; ++status.filteredTitle; taskEXIT_CRITICAL(); return;
  }
  // retrieve declared size first to detect truncation and oversize bodies.
  if (!attribute(handle, expected, uid, ANCS_ATTR_MESSAGE_SIZE, 16, sizeText, sizeof(sizeText), sizeLength)) return;
  uint16_t messageSize = 0;
  // invalid lengths preserve previously accepted watch data.
  if (!parseMessageSize(sizeText, sizeLength, messageSize)) {
    taskENTER_CRITICAL(); ++status.dropped; taskEXIT_CRITICAL(); return;
  }
  // reuse one worker-owned body buffer across notification lookups.
  static char body[kMaxNotificationBytes + 1];
  size_t bodyLength = 0;
  // request the declared value within the transport's fixed capacity.
  if (!attribute(handle, expected, uid, ANCS_ATTR_MESSAGE, messageSize, body, sizeof(body), bodyLength)) return;
  const uint32_t bodyHash = metadataFingerprint(body, bodyLength);
  taskENTER_CRITICAL();
  // retain transfer sizes without accepting mismatched content.
  if (epoch == expected && connection == handle && !restart && status.secured) {
    status.lastReportedBodyLength = messageSize;
    status.lastBodyLength = static_cast<uint16_t>(bodyLength);
    status.lastBodyHash = bodyHash;
  }
  taskEXIT_CRITICAL();
  // reject truncation and raw binary before decoding the text transport.
  if (bodyLength != messageSize || !asciiRelayBody(body, bodyLength)) {
    taskENTER_CRITICAL(); ++status.dropped; taskEXIT_CRITICAL(); return;
  }
  taskENTER_CRITICAL();
  // publish only while the same secure physical connection remains active.
  if (epoch == expected && connection == handle && !restart && status.secured) {
    memcpy(pending, body, bodyLength + 1);
    pendingLength = bodyLength;
    pendingEpoch = expected;
    ++status.received;
  }
  taskEXIT_CRITICAL();
}

// require Service Changed before accepting dynamically published ANCS handles.
bool discover(uint16_t handle, uint32_t expected) {
  ble_gattc_handle_range_t range{};
  // establish standard GATT indications once per connection.
  if (!serviceChangeSubscribed) {
    // absence can be retried; malformed or incomplete discovery fails closed.
    if (!service(handle, expected, gattUuid, range)) return false;
    // all discovery and subscriptions use persistent project-owned ATT state.
    if (!characteristics(handle, expected, range, false) ||
        !subscribe(handle, expected, changed, true)) { failSession(expected); return false; }
    taskENTER_CRITICAL();
    // a disconnect cannot leave a subscription flag on its replacement session.
    if (epoch == expected && !restart) serviceChangeSubscribed = true;
    taskEXIT_CRITICAL();
  }
  // iOS may expose ANCS only after permission or an availability change.
  if (!service(handle, expected, solicitation.uuid, range)) return false;
  // require all three ANCS values and the Control Point write property.
  if (!characteristics(handle, expected, range, true) || !(control.properties & 0x08) ||
      !subscribe(handle, expected, dataSource, false) ||
      !subscribe(handle, expected, notifications, false)) { failSession(expected); return false; }
  taskENTER_CRITICAL();
  // acknowledged CCCDs establish subscriptions, not proof of physical delivery.
  if (epoch == expected && !restart) status.subscribed = true;
  taskEXIT_CRITICAL();
  return current(expected, handle);
}

// run bounded BLE waits independently of rendering, touch and the hardware RTC.
void worker(void*) {
  // reset discovery and pairing attempts at each observed generation.
  uint32_t observedEpoch = 0, lastDiscovery = 0;
  bool pairingRequested = false;
  // fixed queues and static buffers avoid per-message allocations.
  for (;;) {
    // snapshot connection state and stack headroom without retaining peer pointers.
    const uint32_t freeStack = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
    taskENTER_CRITICAL();
    const uint32_t expected = epoch;
    const uint16_t handle = connection;
    const bool reconnect = restart, encrypted = status.secured, subscribed = status.subscribed;
    status.stackFree = freeStack;
    taskEXIT_CRITICAL();
    // refresh worker-local scheduling at each physical/service boundary.
    if (observedEpoch != expected) {
      observedEpoch = expected;
      pairingRequested = false;
      lastDiscovery = millis() - 5000;
    }
    // wait without accessing an absent connection.
    if (handle == BLE_CONN_HANDLE_INVALID) { delay(20); continue; }
    // stale ATT state cannot survive a reconnect.
    if (reconnect) {
      // suspend lifecycle tasks without masking the disconnect SVC.
      vTaskSuspendAll();
      // a raw handle call avoids retaining a deletable connection pointer.
      if (epoch == expected && connection == handle && restart)
        sd_ble_gap_disconnect(handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
      xTaskResumeAll();
      delay(20); continue;
    }
    // pairing is queued once; denial waits for a user reconnect.
    if (!encrypted) {
      // keep the pointer valid only across this core's immediate SoftDevice request.
      if (!pairingRequested && current(expected, handle)) {
        pairingRequested = true;
        // the BLE task cannot delete this pointer while scheduling is suspended.
        vTaskSuspendAll();
        BLEConnection* peer = Bluefruit.Connection(handle);
        // requestPairing performs no waits or allocations in the installed core.
        if (epoch == expected && !restart && peer) peer->requestPairing();
        xTaskResumeAll();
      }
      delay(20); continue;
    }
    // retry an absent service every five seconds.
    if (!subscribed) {
      // avoid repeatedly walking iOS's GATT table on each loop.
      if (static_cast<uint32_t>(millis() - lastDiscovery) >= 5000) {
        lastDiscovery = millis();
        discover(handle, expected);
      }
      delay(20); continue;
    }
    // reserve at most one lookup while keeping a completed frame available to the watch.
    uint32_t uid = 0;
    bool available = false;
    taskENTER_CRITICAL();
    // preserve a completed frame until the watch consumes it.
    if (epoch == expected && !restart && pendingLength == 0 && queued) {
      uid = uids[0];
      --queued;
      memmove(uids, uids + 1, queued * sizeof(uids[0]));
      available = true;
    }
    taskEXIT_CRITICAL();
    // fetch only the next bounded queued notification.
    if (available) fetch(handle, expected, uid);
    delay(10);
  }
}
}  // namespace

// register UUIDs only; core Discovery and client-characteristic buffers stay unused.
bool begin() {
  // avoid duplicate worker creation.
  if (status.ready) return true;
  // report UUID table exhaustion without starting a partially configured client.
  if (!solicitation.uuid.begin() || !controlUuid.begin() || !notificationUuid.begin() ||
      !dataUuid.begin() || !gattUuid.begin() || !changedUuid.begin()) return false;
  // register the direct callback before starting the independent bounded worker.
  Bluefruit.setEventCallback(onBleEvent);
  const bool ready = xTaskCreate(worker, "ancs", 1024, nullptr, 1, nullptr) == pdPASS;
  taskENTER_CRITICAL(); status.ready = ready; taskEXIT_CRITICAL();
  return ready;
}

// a client service UUID encodes an ANCS solicitation, not a server advertisement.
bool addSolicitation() { return Bluefruit.Advertising.addService(solicitation); }

// provide a stable copied frame; the watch must also check the generation at commit.
bool takeFrame(char* buffer, size_t capacity, size_t& length, uint32_t& ticket) {
  taskENTER_CRITICAL();
  // an undersized destination does not consume the pending frame.
  if (!buffer || pendingLength == 0 || capacity <= pendingLength ||
      pendingEpoch != epoch || restart || !status.secured) { taskEXIT_CRITICAL(); return false; }
  length = pendingLength;
  ticket = pendingEpoch;
  memcpy(buffer, pending, length + 1);
  pendingLength = 0;
  taskEXIT_CRITICAL();
  return true;
}

// support the watch task's existing atomic model-commit guard.
uint32_t generation() {
  taskENTER_CRITICAL(); const uint32_t result = epoch; taskEXIT_CRITICAL();
  return result;
}

// expose cached counters only, never unrelated notification text.
Diagnostics diagnostics() {
  taskENTER_CRITICAL(); const Diagnostics result = status; taskEXIT_CRITICAL();
  return result;
}
}  // namespace notificationRelay
