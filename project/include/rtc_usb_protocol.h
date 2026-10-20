#pragma once

#include <stddef.h>
#include <stdint.h>

// USB provisioning frames, independent of RTC hardware access.
namespace rtcusb {

// select physical RTC read or write.
enum class Operation : uint8_t { Get, Set };
// correlate replies by nonce; writes include UTC.
struct Command {
  Operation operation = Operation::Get;
  uint32_t nonce = 0;
  uint32_t utc = 0;
};
// distinguish diagnostics from complete RTC commands.
enum class Input : uint8_t { None, Status, Command };

/* USB task only; discard invalid/overlong/timed-out frames through LF.
 * pass complete commands to the watch task; parsing never accesses I2C. */
class Framer {
 public:
  // consume one byte; return a command only after complete validation.
  Input push(uint8_t byte, uint32_t nowMs, Command& command);
  // clear partial/discard state on USB session changes.
  void reset();

 private:
  // bound command storage and track the inter-byte timeout.
  char line_[40]{};
  size_t length_ = 0;
  uint32_t lastByteMs_ = 0;
  bool discarding_ = false;
};

// format a nonce-matched reply; return 0 if the buffer is too small.
size_t formatReply(const Command& command, bool ok, uint32_t utc,
                   char* output, size_t capacity);
}  // namespace rtcusb
