#include "binary_protocol.h"

#include <float.h>
#include <string.h>

// validate bounded binary packets before exposing borrowed typed views.
namespace watch {
// keep byte-order, UTF-8 and recursive scanning helpers private.
namespace {

// require the wire floating-point representation on every target.
static_assert(sizeof(double) == 8 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
              "XW1 requires IEEE-754 binary64");

// load little-endian words without alignment or aliasing assumptions.
uint64_t littleEndian(const uint8_t* bytes, size_t length) {
  uint64_t value = 0;
  // assemble only fixed-width fields already checked by the caller.
  for (size_t i = 0; i < length; ++i) value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
  return value;
}

// reject malformed, overlong, surrogate and out-of-range UTF-8 sequences.
bool validUtf8(const uint8_t* bytes, size_t length) {
  size_t offset = 0;
  // consume complete Unicode scalar values, including ASCII NUL.
  while (offset < length) {
    const uint8_t first = bytes[offset++];
    // ASCII needs no continuation bytes.
    if (first < 0x80) continue;
    uint32_t value = 0, minimum = 0;
    size_t count = 0;
    // classify valid leading bytes without accepting overlong prefixes.
    if (first >= 0xC2 && first <= 0xDF) {
      value = first & 0x1F; count = 1; minimum = 0x80;
    // three-byte sequences exclude surrogates after reconstruction.
    } else if (first >= 0xE0 && first <= 0xEF) {
      value = first & 0x0F; count = 2; minimum = 0x800;
    // Unicode ends at U+10FFFF.
    } else if (first >= 0xF0 && first <= 0xF4) {
      value = first & 0x07; count = 3; minimum = 0x10000;
    // stray continuation bytes and obsolete prefixes are invalid.
    } else return false;
    // require every continuation before inspecting its byte.
    if (count > length - offset) return false;
    // assemble one scalar value from its continuation bytes.
    for (size_t i = 0; i < count; ++i) {
      const uint8_t next = bytes[offset++];
      // continuations always have the binary prefix 10.
      if ((next & 0xC0) != 0x80) return false;
      value = (value << 6) | (next & 0x3F);
    }
    // reject aliases and code points excluded from Unicode scalar values.
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) return false;
  }
  return true;
}

// inspect a complete value or skip a previously validated child.
bool scanValue(const uint8_t* bytes, size_t length, size_t& used,
               uint8_t depth, bool validate) {
  // every value begins with one tag.
  if (!length) return false;
  const uint8_t tag = bytes[0];
  used = 1;
  // null and boolean tags have no content.
  if (tag <= 2) return true;
  // integers and finite floating-point values occupy eight bytes.
  if (tag == 3 || tag == 4) {
    // reject incomplete scalar fields.
    if (length < 9) return false;
    used = 9;
    // all-ones binary64 exponents encode infinity or NaN.
    return tag == 3 || !validate || (littleEndian(bytes + 1, 8) & UINT64_C(0x7FF0000000000000)) != UINT64_C(0x7FF0000000000000);
  }
  // strings and byte arrays carry an unsigned two-byte length.
  if (tag == 5 || tag == 8) {
    // read lengths only after the entire prefix is present.
    if (length < 3) return false;
    const size_t count = static_cast<size_t>(littleEndian(bytes + 1, 2));
    // prevent truncated content and validate strings without coercion.
    if (count > length - 3 || (validate && tag == 5 && !validUtf8(bytes + 3, count))) return false;
    used = count + 3;
    return true;
  }
  // only arrays and maps remain; container depth starts at one.
  if ((tag != 6 && tag != 7) || length < 2 || depth >= kBinaryDepthLimit) return false;
  const uint8_t count = bytes[1];
  // maps retain a bounded number of unique keys.
  if (tag == 7 && count > 32) return false;
  used = 2;
  // scan children once; duplicate key checks rescan bounded prior entries.
  for (uint8_t i = 0; i < count; ++i) {
    // map entries prefix each value with a UTF-8 key.
    if (tag == 7) {
      // each key needs its length byte before its contents.
      if (used >= length) return false;
      const size_t keyLength = bytes[used++];
      const uint8_t* key = bytes + used;
      // reject truncated or malformed keys.
      if (keyLength > length - used || (validate && !validUtf8(key, keyLength))) return false;
      // compare exact decoded UTF-8 bytes, including embedded NUL.
      if (validate) {
        size_t prior = 2;
        // walk earlier entries without allocating a per-depth key table.
        for (uint8_t j = 0; j < i; ++j) {
          const size_t priorLength = bytes[prior++];
          // canonical UTF-8 gives one encoding for each scalar sequence.
          if (priorLength == keyLength && memcmp(bytes + prior, key, keyLength) == 0) return false;
          prior += priorLength;
          size_t skipped = 0;
          // previous values were already validated at this depth.
          if (!scanValue(bytes + prior, length - prior, skipped, depth + 1, false)) return false;
          prior += skipped;
        }
      }
      used += keyLength;
    }
    size_t child = 0;
    // recursively validate each child before advancing the cursor.
    if (!scanValue(bytes + used, length - used, child, depth + 1, validate)) return false;
    used += child;
  }
  return true;
}

// accept only the standard Base64 alphabet.
int base64Value(char byte) {
  // contiguous ranges avoid locale-dependent character conversion.
  if (byte >= 'A' && byte <= 'Z') return byte - 'A';
  // lowercase letters follow uppercase letters.
  if (byte >= 'a' && byte <= 'z') return byte - 'a' + 26;
  // decimal digits occupy the next ten values.
  if (byte >= '0' && byte <= '9') return byte - '0' + 52;
  // retain the standard alphabet rather than URL-safe aliases.
  if (byte == '+') return 62;
  // slash is the final standard alphabet character.
  if (byte == '/') return 63;
  return -1;
}
}  // namespace

// validate exactly one bounded value and leave failed output empty.
bool parseBinaryValue(const uint8_t* data, size_t length, BinaryValue& value) {
  value = BinaryValue{};
  size_t used = 0;
  // frames bound total work even for deeply nested or malformed containers.
  if (!data || length > kMaxBinaryFrameBytes || !scanValue(data, length, used, 0, true) || used != length) return false;
  value.data_ = data;
  value.length_ = length;
  return true;
}

// expose a validated tag or the empty-view marker.
BinaryType BinaryValue::type() const {
  return data_ ? static_cast<BinaryType>(data_[0]) : BinaryType::Invalid;
}

// read booleans without converting numeric or null values.
bool BinaryValue::boolean(bool& value) const {
  // reject all other wire tags.
  if (type() != BinaryType::False && type() != BinaryType::True) return false;
  value = type() == BinaryType::True;
  return true;
}

// preserve signed 64-bit values, including the minimum integer.
bool BinaryValue::integer(int64_t& value) const {
  // require the dedicated integer tag.
  if (type() != BinaryType::Integer) return false;
  const uint64_t raw = littleEndian(data_ + 1, 8);
  value = raw & (UINT64_C(1) << 63) ? -1 - static_cast<int64_t>(~raw) : static_cast<int64_t>(raw);
  return true;
}

// reconstruct binary64 bits without aliasing an unaligned wire buffer.
bool BinaryValue::real(double& value) const {
  // integers are intentionally not converted.
  if (type() != BinaryType::Real) return false;
  const uint64_t bits = littleEndian(data_ + 1, 8);
  memcpy(&value, &bits, sizeof(value));
  return true;
}

// borrow string or byte contents without requiring a NUL terminator.
bool BinaryValue::bytes(const uint8_t*& data, size_t& length) const {
  // string and arbitrary bytes remain distinguishable through type().
  if (type() != BinaryType::String && type() != BinaryType::Bytes) return false;
  data = data_ + 3;
  length = static_cast<size_t>(littleEndian(data_ + 1, 2));
  return true;
}

// report immediate container children.
size_t BinaryValue::count() const {
  return type() == BinaryType::Array || type() == BinaryType::Map ? data_[1] : 0;
}

// initialize a sequential cursor over this validated container.
BinaryIterator BinaryValue::iterator() const {
  BinaryIterator result;
  // scalars and empty views yield an empty iterator.
  if (type() == BinaryType::Array || type() == BinaryType::Map) {
    result.cursor_ = data_ + 2;
    result.remainingBytes_ = length_ - 2;
    result.remaining_ = data_[1];
    result.map_ = type() == BinaryType::Map;
  }
  return result;
}

// advance one validated child, optionally returning its map key.
bool BinaryIterator::next(BinaryValue& value, const uint8_t** key, size_t* keyBytes) {
  value = BinaryValue{};
  // arrays and exhausted iterators have no key.
  if (key) *key = nullptr;
  // clear optional lengths before any early return.
  if (keyBytes) *keyBytes = 0;
  // stop after the declared child count.
  if (!remaining_) return false;
  // skip a map key before exposing its value.
  if (map_) {
    const size_t length = *cursor_++;
    // expose borrowed UTF-8 key bytes only when requested.
    if (key) *key = cursor_;
    // callers must use this length rather than strlen().
    if (keyBytes) *keyBytes = length;
    cursor_ += length;
    remainingBytes_ -= length + 1;
  }
  size_t length = 0;
  // immutable validated storage makes this scan infallible.
  if (!scanValue(cursor_, remainingBytes_, length, 0, false)) {
    remaining_ = 0;
    return false;
  }
  value.data_ = cursor_;
  value.length_ = length;
  cursor_ += length;
  remainingBytes_ -= length;
  --remaining_;
  return true;
}

// compute the reflected IEEE CRC with initial/final inversion.
uint32_t binaryCrc32(const uint8_t* data, size_t length) {
  uint32_t crc = UINT32_MAX;
  // cover every header, topic and payload byte in wire order.
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    // the reflected polynomial matches CRC-32/ISO-HDLC.
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1)));
  }
  return ~crc;
}

// validate the envelope and typed payload before publishing any borrowed view.
bool parseBinaryFrame(const uint8_t* data, size_t length, BinaryEnvelope& envelope) {
  envelope = BinaryEnvelope{};
  // reject incomplete headers, unsupported versions and oversized packets.
  if (!data || length < 16 || length > kMaxBinaryFrameBytes || data[0] != 'X' ||
      data[1] != 'W' || data[2] != 1 || data[3] > 1) return false;
  const size_t topicBytes = data[8];
  const size_t payloadBytes = static_cast<size_t>(littleEndian(data + 9, 2));
  // require one exact frame with no suffix or truncated fields.
  if (!topicBytes || topicBytes > kBinaryTopicBytes || length != 15 + topicBytes + payloadBytes ||
      binaryCrc32(data, length - 4) != littleEndian(data + length - 4, 4)) return false;
  // validate topic ASCII before copying it into diagnostic strings.
  for (size_t i = 0; i < topicBytes; ++i) {
    const uint8_t c = data[11 + i];
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    // punctuation is permitted only after an initial letter or digit.
    if (!alnum && (!i || (c != '.' && c != '_' && c != '/' && c != '-'))) return false;
  }
  const bool weather = topicBytes == 7 && memcmp(data + 11, "weather", 7) == 0;
  const uint8_t* payload = data + 11 + topicBytes;
  // the weather adapter exclusively owns its fixed codec and topic.
  if (data[3] == 1) {
    // remaining weather semantics belong to the existing weather validator.
    if (!weather || payloadBytes != 118) return false;
  // generic payloads contain exactly one typed value.
  } else {
    BinaryValue value;
    // reject a generic alias for the weather adapter.
    if (weather || !parseBinaryValue(payload, payloadBytes, value)) return false;
  }
  envelope.sequence = static_cast<uint32_t>(littleEndian(data + 4, 4));
  envelope.codec = data[3];
  memcpy(envelope.topic, data + 11, topicBytes);
  envelope.topic[topicBytes] = '\0';
  envelope.payload = payload;
  envelope.payloadBytes = payloadBytes;
  return true;
}

// decode the ASCII text wrapper with canonical Base64 padding.
bool parseBinaryText(const char* text, size_t length, uint8_t* storage,
                     size_t capacity, BinaryEnvelope& envelope) {
  envelope = BinaryEnvelope{};
  // the prefix and complete quartets are mandatory; whitespace is not accepted.
  if (!text || !storage || length <= 4 || length > kMaxBinaryTextBytes ||
      memcmp(text, "XW1:", 4) || (length - 4) % 4) return false;
  size_t written = 0;
  // decode one quartet at a time without writing past caller storage.
  for (size_t i = 4; i < length; i += 4) {
    const int a = base64Value(text[i]), b = base64Value(text[i + 1]);
    const bool last = i + 4 == length;
    // the first two positions never contain padding.
    if (a < 0 || b < 0 || written >= capacity) return false;
    storage[written++] = static_cast<uint8_t>((a << 2) | (b >> 4));
    // one-byte groups require two padding symbols and zero unused bits.
    if (text[i + 2] == '=') {
      // padding may occur only at the end of the complete wrapper.
      if (!last || text[i + 3] != '=' || (b & 15)) return false;
      continue;
    }
    const int c = base64Value(text[i + 2]);
    // require the third alphabet character and its output capacity.
    if (c < 0 || written >= capacity) return false;
    storage[written++] = static_cast<uint8_t>((b << 4) | (c >> 2));
    // two-byte groups require one padding symbol and zero unused bits.
    if (text[i + 3] == '=') {
      // reject noncanonical alternate encodings of the same bytes.
      if (!last || (c & 3)) return false;
      continue;
    }
    const int d = base64Value(text[i + 3]);
    // complete groups always emit a third byte.
    if (d < 0 || written >= capacity) return false;
    storage[written++] = static_cast<uint8_t>((c << 6) | d);
  }
  return parseBinaryFrame(storage, written, envelope);
}
}  // namespace watch
