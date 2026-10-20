#pragma once

#include <stddef.h>

// bounded JSON syntax checks shared by relay message families.
namespace watch {

// reject malformed ASCII JSON, decoded duplicate keys and excess nesting.
bool strictJsonSyntax(const char* text, size_t length,
                      size_t* payloadStart = nullptr, size_t* payloadLength = nullptr);

}  // namespace watch
