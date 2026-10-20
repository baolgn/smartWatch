#include "strict_json.h"
#include "phone_protocol.h"

#include <stdint.h>
#include <string.h>

// strict JSON validation before ArduinoJson allocation.
namespace watch {
// keep parser state and decoded-key comparison private.
namespace {

// validate ASCII wire JSON, including escaped Unicode scalar values.
class SyntaxCheck {
 public:
  // bind the bounded input without copying it.
  SyntaxCheck(const char* text, size_t length) : text_(text), length_(length) {}

  // accept exactly one complete object.
  bool valid() {
    skip();
    // protocol envelopes must be objects.
    if (position_ == length_ || text_[position_] != '{' || !value(0)) return false;
    skip();
    return position_ == length_;
  }

  // expose the root payload's original JSON without numeric reserialization.
  size_t payloadStart() const { return payloadStart_; }
  // report the payload slice length, excluding surrounding whitespace.
  size_t payloadLength() const { return payloadLength_; }

 private:
  // retain key slices; input frames fit in 16-bit offsets.
  struct Key { uint16_t start; uint16_t count; };

  // recognize the four JSON whitespace bytes.
  static bool whitespace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

  // advance over JSON whitespace only.
  void skip() {
    // stop at the next token or frame end.
    while (position_ < length_ && whitespace(text_[position_])) ++position_;
  }

  // consume an expected delimiter after whitespace.
  bool take(char c) {
    skip();
    // preserve the cursor when the delimiter is absent.
    if (position_ == length_ || text_[position_] != c) return false;
    ++position_;
    return true;
  }

  // decode one four-digit Unicode escape after its u.
  bool hex4(size_t& cursor, uint32_t& result) const {
    // reject a truncated escape.
    if (length_ - cursor < 4) return false;
    result = 0;
    // accumulate exactly four hexadecimal digits.
    for (unsigned i = 0; i < 4; ++i) {
      const char c = text_[cursor++];
      unsigned digit;
      // decode decimal digits.
      if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
      // decode lowercase hexadecimal letters.
      else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
      // decode uppercase hexadecimal letters.
      else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
      // reject all other characters.
      else return false;
      result = result * 16 + digit;
    }
    return true;
  }

  // decode one string scalar for validation and exact key comparison.
  bool scalar(size_t& cursor, uint32_t& result) const {
    // reject a truncated string.
    if (cursor == length_) return false;
    const unsigned char c = static_cast<unsigned char>(text_[cursor++]);
    // raw controls/non-ASCII and delimiters cannot form string contents.
    if (c < 32 || c >= 127 || c == '"') return false;
    // retain unescaped printable ASCII.
    if (c != '\\') { result = c; return true; }
    // require the escape selector.
    if (cursor == length_) return false;
    const char escape = text_[cursor++];
    // decode the standard JSON escapes.
    switch (escape) {
      // preserve escaped punctuation.
      case '"': case '\\': case '/': result = static_cast<unsigned char>(escape); return true;
      // decode backspace.
      case 'b': result = 8; return true;
      // decode form feed.
      case 'f': result = 12; return true;
      // decode newline.
      case 'n': result = 10; return true;
      // decode carriage return.
      case 'r': result = 13; return true;
      // decode tab.
      case 't': result = 9; return true;
      // decode a Unicode scalar or a valid surrogate pair.
      case 'u': {
        // require the first code unit and reject isolated low surrogates.
        if (!hex4(cursor, result) || (result >= 0xDC00 && result <= 0xDFFF)) return false;
        // combine a high surrogate with its required low surrogate.
        if (result >= 0xD800 && result <= 0xDBFF) {
          // reject missing or incorrectly marked second code units.
          if (length_ - cursor < 6 || text_[cursor] != '\\' || text_[cursor + 1] != 'u') return false;
          cursor += 2;
          uint32_t low;
          // only low surrogates may complete the pair.
          if (!hex4(cursor, low) || low < 0xDC00 || low > 0xDFFF) return false;
          result = 0x10000 + (result - 0xD800) * 1024 + low - 0xDC00;
        }
        return true;
      }
      // reject nonstandard escapes.
      default: return false;
    }
  }

  // validate a quoted string and retain its original byte slice.
  bool string(Key& key) {
    // require an opening quote.
    if (!take('"')) return false;
    const size_t start = position_;
    // decode every scalar through the closing quote.
    while (position_ < length_ && text_[position_] != '"') {
      uint32_t code;
      // reject malformed escape sequences and invalid raw bytes.
      if (!scalar(position_, code)) return false;
    }
    // require a closing quote.
    if (position_ == length_) return false;
    key = {static_cast<uint16_t>(start), static_cast<uint16_t>(position_ - start)};
    ++position_;
    return true;
  }

  // compare decoded keys without hashing, allocation or escape aliases.
  bool equal(Key left, Key right) const {
    size_t a = left.start, b = right.start;
    const size_t endA = a + left.count, endB = b + right.count;
    // compare Unicode scalars from already validated strings.
    while (a < endA && b < endB) {
      uint32_t x, y;
      // different decoded scalars identify distinct keys.
      if (!scalar(a, x) || !scalar(b, y) || x != y) return false;
    }
    return a == endA && b == endB;
  }

  // recognize the envelope's payload member, including escaped aliases.
  bool payloadKey(Key key) const {
    static const char expected[] = "payload";
    size_t cursor = key.start, index = 0;
    const size_t end = cursor + key.count;
    // compare decoded scalars against the complete ASCII field name.
    while (cursor < end && index < sizeof(expected) - 1) {
      uint32_t code;
      // reject differing names.
      if (!scalar(cursor, code) || code != static_cast<unsigned char>(expected[index++])) return false;
    }
    return cursor == end && index == sizeof(expected) - 1;
  }

  // validate object members with bounded exact duplicate detection.
  bool object(unsigned depth) {
    // consume the object and accept an empty object.
    if (!take('{')) return false;
    // no keys need tracking in an empty object.
    if (take('}')) return true;
    Key keys[kJsonObjectKeys];
    size_t count = 0;
    // consume comma-separated members.
    do {
      Key key;
      // reject excessive key counts or malformed names.
      if (count == kJsonObjectKeys || !string(key)) return false;
      // compare this name with earlier members of this object only.
      for (size_t i = 0; i < count; ++i) {
        // aliases such as a and \u0061 are duplicates.
        if (equal(keys[i], key)) return false;
      }
      keys[count++] = key;
      // require the separator before recording the value slice.
      if (!take(':')) return false;
      skip();
      const size_t start = position_;
      // reject malformed values.
      if (!value(depth)) return false;
      // retain only the outer envelope's payload bytes.
      if (depth == 1 && payloadKey(key)) {
        payloadStart_ = start;
        payloadLength_ = position_ - start;
      }
      // close the object after its final member.
      if (take('}')) return true;
    } while (take(','));
    return false;
  }

  // validate an array within the shared nesting limit.
  bool array(unsigned depth) {
    // require the opener.
    if (!take('[')) return false;
    // empty arrays are valid generic values.
    if (take(']')) return true;
    // parse comma-separated values.
    do {
      // reject malformed nested values.
      if (!value(depth)) return false;
      // finish at the closing bracket.
      if (take(']')) return true;
    } while (take(','));
    return false;
  }

  // match a complete JSON literal without reading past the frame.
  bool literal(const char* text) {
    const size_t count = strlen(text);
    // reject mismatched or truncated literals.
    if (count > length_ - position_ || memcmp(text_ + position_, text, count)) return false;
    position_ += count;
    return true;
  }

  // test the current byte for a decimal digit.
  bool digit() const { return position_ < length_ && text_[position_] >= '0' && text_[position_] <= '9'; }

  // validate JSON numeric spelling before conversion.
  bool number() {
    // consume an optional minus sign.
    if (position_ < length_ && text_[position_] == '-') ++position_;
    // require an integer part.
    if (!digit()) return false;
    // prohibit a leading zero followed by another integer digit.
    if (text_[position_] == '0') ++position_;
    // consume a nonzero integer.
    else {
        // stop at the fraction, exponent or delimiter.
        while (digit()) ++position_;
    }
    // parse an optional fractional part.
    if (position_ < length_ && text_[position_] == '.') {
      ++position_;
      // require digits after the decimal point.
      if (!digit()) return false;
      // consume every fractional digit.
      while (digit()) ++position_;
    }
    // parse an optional exponent.
    if (position_ < length_ && (text_[position_] == 'e' || text_[position_] == 'E')) {
      ++position_;
      // consume an optional exponent sign.
      if (position_ < length_ && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
      // require exponent digits.
      if (!digit()) return false;
      // consume all exponent digits.
      while (digit()) ++position_;
    }
    return true;
  }

  // dispatch one JSON value while bounding compound nesting.
  bool value(unsigned depth) {
    skip();
    // reject missing values.
    if (position_ == length_) return false;
    // choose the syntax from its first byte.
    switch (text_[position_]) {
      // descend into an object only within the nesting limit.
      case '{': return depth < kJsonDepthLimit && object(depth + 1);
      // descend into an array only within the nesting limit.
      case '[': return depth < kJsonDepthLimit && array(depth + 1);
      // validate a string value.
      case '"': { Key key; return string(key); }
      // match boolean true.
      case 't': return literal("true");
      // match boolean false.
      case 'f': return literal("false");
      // match JSON null.
      case 'n': return literal("null");
      // other values must follow number syntax.
      default: return number();
    }
  }

  // track the immutable input and current parser cursor.
  const char* text_;
  size_t length_;
  size_t position_ = 0;
  size_t payloadStart_ = 0;
  size_t payloadLength_ = 0;
};

}  // namespace

// enforce frame bounds before recursive syntax validation.
bool strictJsonSyntax(const char* text, size_t length, size_t* payloadStart, size_t* payloadLength) {
  // reject absent or oversized frames before constructing the parser.
  if (!text || !length || length > kMaxFrameBytes) return false;
  SyntaxCheck check(text, length);
  // publish slices only after the entire JSON object passes.
  if (!check.valid()) return false;
  // return optional root payload bounds.
  if (payloadStart) *payloadStart = check.payloadStart();
  // leave callers free to request either bound independently.
  if (payloadLength) *payloadLength = check.payloadLength();
  return true;
}

}  // namespace watch
