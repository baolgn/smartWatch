#pragma once

#include "watch_model.h"
#include "binary_protocol.h"

// bounded phone protocols and watch-state validation.
namespace watch {

// cap frame length, idle time and JSON heap use.
constexpr size_t kMaxFrameBytes = 8192;  // excludes terminating LF.
constexpr uint32_t kFrameTimeoutMs = 15000;
constexpr size_t kJsonHeapLimit = 16384;
constexpr unsigned kJsonDepthLimit = 8;
constexpr size_t kJsonObjectKeys = 32;
constexpr size_t kRelayTopicsPerSession = 8;

// classify frame completion and rejected bytes.
enum class FrameEvent : uint8_t { None, Ready, TooLong, InvalidByte };

// accumulate one LF-terminated ASCII frame.
class LineFramer {
 public:
  // consume one byte and report frame completion or rejection.
  FrameEvent push(uint8_t byte, uint32_t nowMs);
  // discard incomplete frames past the idle timeout.
  bool expire(uint32_t nowMs);
  // clear buffered and discard state.
  void reset();
  // ignore bytes through the next LF.
  void discardToNewline();
  // expose the buffered frame without copying.
  const char* data() const { return data_; }
  // report buffered bytes, excluding LF/NUL.
  size_t length() const { return length_; }

 private:
  // retain one bounded frame and its receive state.
  char data_[kMaxFrameBytes + 1]{};
  size_t length_ = 0;
  uint32_t lastByteMs_ = 0;
  bool discarding_ = false;
  bool ready_ = false;
};

// accepted message families; time synchronization is excluded.
enum class MessageKind : uint8_t { Unknown, Weather, Settings, Data };
// distinguish applied, repeated and rejected messages.
enum class ProtocolStatus : uint8_t {
  Applied, Duplicate, OutOfOrder, InvalidJson, InvalidMessage, NoMemory,
  TopicLimit, PayloadTooLarge, InvalidBinary
};

// retain validation outcome and optional acknowledgement sequence.
struct ProtocolResult {
  ProtocolStatus status = ProtocolStatus::InvalidMessage;
  MessageKind kind = MessageKind::Unknown;
  bool hasSequence = false;
  uint32_t sequence = 0;
  char topic[kRelayTopicBytes + 1]{};
};

// validate messages and track sequence order per message family.
class ProtocolSession {
 public:
  /* application loop only; rejected packets preserve state/sequence tracking.
   * Bluetooth messages cannot set time. */
  ProtocolResult apply(const char* text, size_t length, WatchState& state,
                       uint32_t nowMs);
  // accept a complete raw XW1 frame without its ASCII text wrapper.
  ProtocolResult applyBinary(const uint8_t* bytes, size_t length, WatchState& state,
                             uint32_t nowMs);
  // clear sequence history on a new BLE session.
  void resetSession();

 private:
  // isolate legacy staging so binary dispatch does not reserve its stack frame.
  ProtocolResult applyJson(const char* json, size_t length, WatchState& state,
                           uint32_t nowMs);
  // dispatch a fully validated frame while retaining borrowed payload bytes.
  ProtocolResult applyBinaryEnvelope(const BinaryEnvelope& envelope, WatchState& state,
                                     uint32_t nowMs);
  // commit shared relay metadata and sequence history after payload validation.
  ProtocolResult commitData(ProtocolResult result, WatchState& state,
                            WatchState& next, uint32_t nowMs);
  // keep independent weather/settings sequence histories.
  uint32_t lastSequence_[2]{};
  bool haveSequence_[2]{};
  // bound independent generic topic histories within one BLE connection.
  struct TopicSequence {
    char topic[kRelayTopicBytes + 1]{};
    uint32_t sequence = 0;
  };
  // retain only the occupied topic slots for this connection.
  TopicSequence topics_[kRelayTopicsPerSession]{};
  uint8_t topicCount_ = 0;
};

// map message kinds to JSON names.
const char* messageKindName(MessageKind kind);
// map results to JSON acknowledgement status.
const char* protocolStatusName(ProtocolStatus status);
// bytes include LF, exclude NUL; 0 means insufficient space.
size_t formatAcknowledgement(const ProtocolResult& result, char* buffer,
                             size_t capacity);

}  // namespace watch
