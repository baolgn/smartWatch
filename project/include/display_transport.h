#pragma once

#include <cstdint>

// blocking Round Display transport
namespace board {

/* flush a tightly packed, native-endian RGB565 strip from the 4,800-byte LVGL buffer;
   caller supplies width * height * 2 bytes; restore before returning to LVGL;
   false means invalid input; blocking core SPI reports no transfer errors */
bool writeDisplayRect(int32_t x, int32_t y, int32_t width, int32_t height,
                      uint8_t* pixels);

}  // namespace board
