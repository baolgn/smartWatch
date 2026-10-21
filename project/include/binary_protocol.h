#pragma once

#include <stddef.h>
#include <stdint.h>

// bounded XW1 framing and allocation-free typed payload views.
namespace watch {

// bounded relay text includes the prefix and canonical padded Base64.
constexpr size_t kMaxBinaryTextBytes = 256;
constexpr size_t kMaxBinaryFrameBytes = 189;
constexpr size_t kBinaryTopicBytes = 32;
constexpr uint8_t kBinaryDepthLimit = 8;

// wire tags preserve integers, floating-point numbers and arbitrary bytes.
enum class BinaryType : uint8_t {
  Null = 0, False = 1, True = 2, Integer = 3, Real = 4,
  String = 5, Array = 6, Map = 7, Bytes = 8, Invalid = 255
};

// forward declaration for sequential container traversal.
class BinaryIterator;

// validated views borrow their original payload storage.
class BinaryValue {
 public:
  // an empty view has no value until parsing or iteration succeeds.
  BinaryValue() = default;
  // identify the exact wire type without coercion.
  BinaryType type() const;
  // read a boolean only from its two dedicated tags.
  bool boolean(bool& value) const;
  // read a signed 64-bit integer without floating-point conversion.
  bool integer(int64_t& value) const;
  // read a finite IEEE-754 binary64 value.
  bool real(double& value) const;
  // expose UTF-8 string or arbitrary byte contents, including embedded NUL.
  bool bytes(const uint8_t*& data, size_t& length) const;
  // count array elements or map entries; scalars return zero.
  size_t count() const;
  // traverse immediate children in wire order without allocation.
  BinaryIterator iterator() const;
  // expose the encoded value for forwarding or storage.
  const uint8_t* encoded() const { return data_; }
  // include the tag and all nested content in the encoded length.
  size_t encodedBytes() const { return length_; }

 private:
  // only validated parsing and traversal may create nonempty views.
  friend bool parseBinaryValue(const uint8_t*, size_t, BinaryValue&);
  friend class BinaryIterator;
  const uint8_t* data_ = nullptr;
  size_t length_ = 0;
};

// iterate arrays or maps while retaining the caller-owned backing storage.
class BinaryIterator {
 public:
  // an empty iterator has no children.
  BinaryIterator() = default;
  // return the next child and optional UTF-8 map key; arrays have no key.
  bool next(BinaryValue& value, const uint8_t** key = nullptr,
            size_t* keyBytes = nullptr);

 private:
  // only container views construct populated iterators.
  friend class BinaryValue;
  const uint8_t* cursor_ = nullptr;
  size_t remainingBytes_ = 0;
  uint8_t remaining_ = 0;
  bool map_ = false;
};

// parsed envelope payloads borrow the raw frame or text decode buffer.
struct BinaryEnvelope {
  uint32_t sequence = 0;
  uint8_t codec = 0;
  char topic[kBinaryTopicBytes + 1]{};
  const uint8_t* payload = nullptr;
  size_t payloadBytes = 0;
};

// validate one complete typed value, including UTF-8 and unique map keys.
bool parseBinaryValue(const uint8_t* data, size_t length, BinaryValue& value);
// compute CRC-32/ISO-HDLC over the bytes preceding the trailing checksum.
uint32_t binaryCrc32(const uint8_t* data, size_t length);
// validate a complete raw frame, topic, codec, length and checksum.
bool parseBinaryFrame(const uint8_t* data, size_t length, BinaryEnvelope& envelope);
// decode canonical XW1 text into caller storage and validate its raw frame.
bool parseBinaryText(const char* text, size_t length, uint8_t* storage,
                     size_t capacity, BinaryEnvelope& envelope);

}  // namespace watch
