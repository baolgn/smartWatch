#pragma once

#include "phone_protocol.h"

// accept only fresh generic relay messages from the notification transport.
namespace watch {

// bound parser input; the PC publisher separately enforces its 256-byte path limit.
constexpr size_t kMaxNotificationJsonBytes = 3000;

// accept XW1 text or legacy JSON; stale input preserves all state and history.
ProtocolResult applyNotificationBody(const char* body, size_t length,
                                     ProtocolSession& session, WatchState& state,
                                     uint32_t nowMs);
// retain the legacy entry point while callers migrate to the format-neutral name.
ProtocolResult applyNotificationJson(const char* json, size_t length,
                                     ProtocolSession& session, WatchState& state,
                                     uint32_t nowMs);

}  // namespace watch
