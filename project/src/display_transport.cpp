#include "display_transport.h"

#include <Arduino.h>
#include <SPI.h>

// vendor initialization sets GC9A01 orientation/RGB565; all rotations have zero offsets
static_assert(TFT_WIDTH == 240 && TFT_HEIGHT == 240, "Round Display geometry required");

// blocking Round Display transport
namespace board {
// private GC9A01 commands and pixel conversion
namespace {
// panel bounds and maximum strip size
constexpr int32_t kWidth = 240;
constexpr int32_t kHeight = 240;
constexpr uint32_t kMaxPixels = 2400;  // 4,800-byte partial draw buffer

// send one command, then return D/C to data mode
void command(uint8_t value) {
    digitalWrite(TFT_DC, LOW);
    SPI.transfer(&value, nullptr, 1);
    digitalWrite(TFT_DC, HIGH);
}

// exchange native and GC9A01 RGB565 byte order in place
void swapPixelBytes(uint8_t* pixels, uint32_t bytes) {
    // swap each two-byte pixel
    for (uint32_t i = 0; i < bytes; i += 2) {
        const uint8_t low = pixels[i];
        pixels[i] = pixels[i + 1];
        pixels[i + 1] = low;
    }
}
}  // namespace

// send one validated RGB565 strip and restore its original bytes
bool writeDisplayRect(int32_t x, int32_t y, int32_t width, int32_t height,
                      uint8_t* pixels) {
    // validate LVGL rectangles before buffer/bus access; subtract to avoid overflow
    if (!pixels || width <= 0 || height <= 0 || width > kWidth || height > kHeight ||
        x < 0 || y < 0 || x > kWidth - width || y > kHeight - height) return false;
    const uint32_t count = static_cast<uint32_t>(width) * static_cast<uint32_t>(height);
    // reject areas larger than the LVGL draw buffer
    if (count > kMaxPixels) return false;

    // convert the area into inclusive controller endpoints
    const uint16_t xEnd = static_cast<uint16_t>(x + width - 1);
    const uint16_t yEnd = static_cast<uint16_t>(y + height - 1);
    // writable local buffers satisfy EasyDMA's data RAM requirement
    uint8_t columns[] = {static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(x),
                         static_cast<uint8_t>(xEnd >> 8), static_cast<uint8_t>(xEnd)};
    uint8_t rows[] = {static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y),
                      static_cast<uint8_t>(yEnd >> 8), static_cast<uint8_t>(yEnd)};
    // convert RGB565 pixels to controller byte order
    const uint32_t bytes = count * 2;
    swapPixelBytes(pixels, bytes);

    /* send each strip in one blocking EasyDMA transfer on SPIM3 at 8 MHz;
       avoid the vendor's per-pixel transfers and incorrect SPIM0 dmaWait()/endWrite() */
    SPI.beginTransaction(SPISettings(SPI_FREQUENCY, MSBFIRST, SPI_MODE0));
    digitalWrite(TFT_CS, LOW);
    command(0x2A);  // column window; inclusive endpoints
    SPI.transfer(columns, nullptr, sizeof(columns));
    command(0x2B);  // row window; inclusive endpoints
    SPI.transfer(rows, nullptr, sizeof(rows));
    command(0x2C);  // write RAM
    SPI.transfer(pixels, nullptr, bytes);  // TX only; MISO cannot overwrite pixels
    digitalWrite(TFT_CS, HIGH);
    SPI.endTransaction();

    // restore native byte order before LVGL reuses the buffer
    swapPixelBytes(pixels, bytes);
    return true;
}

}  // namespace board
