#include "rtc_usb_protocol.h"
#include "watch_model.h"

#include <cstdio>
#include <cstring>

// parse USB RTC commands without accessing hardware.
namespace rtcusb {
// private decimal and command parsers.
namespace {
// consume unsigned decimal digits with overflow checks.
bool number(const char*& input, uint32_t& value) {
  // require an initial digit.
  if (*input < '0' || *input > '9') return false;
  value = 0;
  // accumulate consecutive digits.
  do {
    const uint32_t digit = static_cast<uint32_t>(*input++ - '0');
    // reject overflow before multiplying.
    if (value > (UINT32_MAX - digit) / 10) return false;
    value = value * 10 + digit;
  } while (*input >= '0' && *input <= '9');
  return true;
}

// validate the complete command before returning its nonce and UTC.
bool parse(const char* line, Command& output) {
  Command next;
  // select writes by their command prefix.
  if (std::strncmp(line, "RTCSET,", 7) == 0) next.operation = Operation::Set;
  // otherwise require the read prefix.
  else if (std::strncmp(line, "RTCGET,", 7) != 0) return false;
  const char* input = line + 7;
  // require the reply-correlation nonce.
  if (!number(input, next.nonce)) return false;
  // writes also require a valid UTC field.
  if (next.operation == Operation::Set) {
    // reject missing delimiters, malformed numbers and unsupported dates.
    if (*input++ != ',' || !number(input, next.utc) || !watch::validUtc(next.utc)) return false;
  }
  // reject trailing fields or bytes.
  if (*input != '\0') return false;
  output = next;
  return true;
}
}  // namespace

// discard partial frames and reset receive timing.
void Framer::reset() {
  length_ = 0;
  lastByteMs_ = 0;
  discarding_ = false;
}

// consume USB bytes; return only complete commands or standalone diagnostics.
Input Framer::push(uint8_t byte, uint32_t nowMs, Command& command) {
  // discard incomplete commands after two seconds between bytes.
  if (length_ && static_cast<uint32_t>(nowMs - lastByteMs_) >= 2000) {
    discarding_ = true;
    length_ = 0;
  }
  lastByteMs_ = nowMs;
  // finish at LF and validate a non-discarded frame.
  if (byte == '\n') {
    const bool complete = !discarding_ && length_ != 0;
    // accept optional CR before LF.
    if (length_ && line_[length_ - 1] == '\r') --length_;
    line_[length_] = '\0';
    reset();
    return complete && parse(line_, command) ? Input::Command : Input::None;
  }
  // ignore the rest of a rejected frame through LF.
  if (discarding_) return Input::None;
  // allow diagnostics only outside a command.
  if (byte == '?' && length_ == 0) return Input::Status;
  // reject non-ASCII/control bytes and preserve space for NUL.
  if ((byte < 32 && byte != '\r') || byte > 126 || length_ == sizeof(line_) - 1) {
    length_ = 0;
    discarding_ = true;
    return Input::None;
  }
  line_[length_++] = static_cast<char>(byte);
  return Input::None;
}

// format a nonce-matched success/error line; never report truncated replies.
size_t formatReply(const Command& command, bool ok, uint32_t utc,
                   char* output, size_t capacity) {
  // require writable output space.
  if (!output || capacity == 0) return 0;
  // include UTC only when the hardware operation succeeded.
  const char* name = command.operation == Operation::Set ? "RTCSET" : "RTCGET";
  const int count = ok ?
      std::snprintf(output, capacity, "%s,%lu,OK,%lu\n", name,
                    static_cast<unsigned long>(command.nonce), static_cast<unsigned long>(utc)) :
      std::snprintf(output, capacity, "%s,%lu,ERR\n", name,
                    static_cast<unsigned long>(command.nonce));
  return count > 0 && static_cast<size_t>(count) < capacity ? static_cast<size_t>(count) : 0;
}
}  // namespace rtcusb
