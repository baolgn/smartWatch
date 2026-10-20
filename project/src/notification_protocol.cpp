#include "notification_protocol.h"

// keep notification-specific freshness separate from the UART protocol.
namespace watch {

// validate a complete body and commit only a fresh generic envelope.
ProtocolResult applyNotificationBody(const char* json, size_t length,
                                     ProtocolSession& session, WatchState& state,
                                     uint32_t nowMs) {
    // default to rejection without changing caller state.
    ProtocolResult rejected;
    // an unset/untrusted RTC cannot distinguish current from queued notifications.
    if (!state.timeValid || state.rtcFault || json == nullptr || length == 0 ||
        length > kMaxNotificationJsonBytes) return rejected;
    // notification bodies carry ASCII XW1 text or escaped legacy JSON.
    for (size_t i = 0; i < length; ++i) {
        const unsigned char byte = static_cast<unsigned char>(json[i]);
        // Unicode stays inside Base64 or legacy JSON escapes.
        if ((byte < 32 && byte != '\t' && byte != '\r' && byte != '\n') || byte > 126) return rejected;
    }
    // stage model and sequence changes until notification freshness passes.
    WatchState candidate = state;
    ProtocolSession nextSession = session;
    ProtocolResult result = nextSession.apply(json, length, candidate, nowMs);
    // retain parser failures before requiring a recognized envelope family.
    if (result.status == ProtocolStatus::InvalidJson || result.status == ProtocolStatus::InvalidBinary ||
        result.status == ProtocolStatus::NoMemory)
        return result;
    // legacy settings/weather messages are unavailable through notification bodies.
    if (result.kind != MessageKind::Data || !result.hasSequence) return rejected;
    // compare publisher UTC against the trusted RTC without arithmetic overflow.
    const uint64_t sent = result.sequence;
    const uint64_t now = state.utc;
    // allow five minutes ahead and fifteen minutes behind the RTC.
    if (sent > now + 300 || sent + 900 < now) {
        result.status = ProtocolStatus::InvalidMessage;
        return result;
    }
    // copy only accepted data; rejected input must not consume topic sequences.
    if (result.status == ProtocolStatus::Applied || result.status == ProtocolStatus::Duplicate) {
        state = candidate;
        session = nextSession;
    }
    return result;
}

// retain the previous entry point without maintaining a second validation path.
ProtocolResult applyNotificationJson(const char* json, size_t length,
                                     ProtocolSession& session, WatchState& state,
                                     uint32_t nowMs) {
    return applyNotificationBody(json, length, session, state, nowMs);
}

}  // namespace watch
