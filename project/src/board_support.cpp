#include "board_support.h"

#include <Arduino.h>
#include <RTClib.h>
#include <Wire.h>
#include <atomic>
#include <nrf_gpio.h>

// private RTC, touch and battery support
namespace {
// external I2C addresses, calendar range and battery scaling
constexpr uint8_t kRtcAddress = 0x51;
constexpr uint8_t kTouchAddress = 0x2e;
constexpr uint32_t kFirstSupportedUtc = 946684800UL;   // 2000-01-01
constexpr uint32_t kLastSupportedUtc = 4102444799UL;  // 2099-12-31
constexpr float kAdcRangeVolts = 3.0f;
constexpr float kDividerRatio = 2.0f;  // Round Display: 470 kOhm / 470 kOhm
// calibrate D0 voltage against a calibrated meter
constexpr float kBatteryCalibration = 1.0f;
constexpr unsigned kBatterySamples = 16;
// bound direct touch reception and STOP completion
constexpr uint32_t kTouchReceiveTimeoutMs = 10;
constexpr uint32_t kTouchStopTimeoutMs = 5;

// watch-task hardware state and atomic startup diagnostics
RTC_PCF8563 rtc;
bool rtcFound = false;
std::atomic<const char*> startupLabel{"not_started"};
uint32_t touchErrorCount = 0;
uint32_t touchRecoveryCount = 0;
// static DMA storage survives a missing STOPPED event
alignas(4) uint8_t touchBytes[5] = {};

// clear TWIM0 shortcuts, events and write-one-to-clear errors
void clearTwimEvents() {
  NRF_TWIM0->SHORTS = 0;
  NRF_TWIM0->EVENTS_RXSTARTED = 0;
  NRF_TWIM0->EVENTS_TXSTARTED = 0;
  NRF_TWIM0->EVENTS_LASTRX = 0;
  NRF_TWIM0->EVENTS_LASTTX = 0;
  NRF_TWIM0->EVENTS_STOPPED = 0;
  NRF_TWIM0->EVENTS_SUSPENDED = 0;
  NRF_TWIM0->EVENTS_ERROR = 0;
  NRF_TWIM0->ERRORSRC = NRF_TWIM0->ERRORSRC;  // write one to clear
}

// wait briefly for a slave to release SCL
bool waitForSclHigh(uint32_t scl) {
  // bound the wait even if the software clock stops
  for (unsigned i = 0; i < 100; ++i) {
    // stop polling as soon as SCL is released
    if (nrf_gpio_pin_read(scl)) return true;
    delayMicroseconds(1);
  }
  return nrf_gpio_pin_read(scl) != 0;
}

// require both external I2C lines high
bool busIdle() {
  return digitalRead(D4) == HIGH && digitalRead(D5) == HIGH;
}

// release stuck external I2C slaves and restore Wire at 100 kHz
bool recoverBus() {
  // watch task owns the bus; disable DMA before pin/buffer reuse, even after STOP timeout
  Wire.end();
  __DMB();
  clearTwimEvents();
  // S0D1 releases HIGH; never drive against a slave
  const uint32_t sda = g_ADigitalPinMap[D4];
  const uint32_t scl = g_ADigitalPinMap[D5];
  nrf_gpio_pin_set(sda);
  nrf_gpio_pin_set(scl);
  nrf_gpio_cfg(sda, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_CONNECT,
               NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_S0D1, NRF_GPIO_PIN_NOSENSE);
  nrf_gpio_cfg(scl, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_CONNECT,
               NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_S0D1, NRF_GPIO_PIN_NOSENSE);
  bool clockReleased = waitForSclHigh(scl);
  // pulse SCL up to nine times while a slave holds SDA low
  for (unsigned i = 0; clockReleased && !nrf_gpio_pin_read(sda) && i < 9; ++i) {
    nrf_gpio_pin_clear(scl);
    delayMicroseconds(5);
    nrf_gpio_pin_set(scl);
    clockReleased = waitForSclHigh(scl);
    delayMicroseconds(5);
  }
  // lower SDA with SCL low; release SCL then SDA to generate STOP
  if (clockReleased) {
    nrf_gpio_pin_clear(scl);
    nrf_gpio_pin_clear(sda);
    delayMicroseconds(5);
    nrf_gpio_pin_set(scl);
    clockReleased = waitForSclHigh(scl);
    delayMicroseconds(5);
    nrf_gpio_pin_set(sda);
    delayMicroseconds(5);
  }
  // restore the shared peripheral and discard stale events
  Wire.begin();
  Wire.setClock(100000);
  clearTwimEvents();
  return clockReleased && busIdle();
}

// reject stuck buses before unbounded Wire calls; in-progress RTC transfers lack timeouts
bool prepareBus() {
  return busIdle() || recoverBus();
}

// receive one five-byte touch report with bounded DMA and STOP waits
bool receiveTouch() {
  // recover a busy bus and cancel the interrupted gesture
  if (!busIdle()) {
    ++touchErrorCount;
    // count only successful bus recovery
    if (recoverBus()) ++touchRecoveryCount;
    return false;  // cancel the gesture even after recovery
  }
  // arm direct reception with an automatic LASTRX-to-STOP shortcut
  clearTwimEvents();
  NRF_TWIM0->ADDRESS = kTouchAddress;
  NRF_TWIM0->RXD.PTR = reinterpret_cast<uintptr_t>(touchBytes);
  NRF_TWIM0->RXD.MAXCNT = sizeof(touchBytes);
  NRF_TWIM0->SHORTS = TWIM_SHORTS_LASTRX_STOP_Msk;
  __DMB();
  NRF_TWIM0->TASKS_RESUME = 1;
  NRF_TWIM0->TASKS_STARTRX = 1;
  const uint32_t started = millis();
  // interrupts stay enabled for BLE, USB and the RTOS tick during the 10 ms wait
  while (!NRF_TWIM0->EVENTS_LASTRX && !NRF_TWIM0->EVENTS_ERROR &&
         !NRF_TWIM0->EVENTS_STOPPED &&
         static_cast<uint32_t>(millis() - started) < kTouchReceiveTimeoutMs) {
  }
  // require an actual completed reception without a hardware error
  const bool received = NRF_TWIM0->EVENTS_RXSTARTED &&
                        NRF_TWIM0->EVENTS_LASTRX &&
                        !NRF_TWIM0->EVENTS_ERROR;
  // wait for STOP before inspecting or reusing DMA data
  if (!NRF_TWIM0->EVENTS_STOPPED) {
    // LASTRX's shortcut finishes the final byte before STOP; request STOP only without LASTRX
    if (!NRF_TWIM0->EVENTS_LASTRX) {
      NRF_TWIM0->TASKS_RESUME = 1;
      NRF_TWIM0->TASKS_STOP = 1;
    }
    const uint32_t stopping = millis();
    // allow at most 5 ms for STOP completion
    while (!NRF_TWIM0->EVENTS_STOPPED &&
           static_cast<uint32_t>(millis() - stopping) < kTouchStopTimeoutMs) {}
  }
  // validate the entire report and restore Wire's software STOP/SUSPEND policy
  const bool success = received && NRF_TWIM0->EVENTS_STOPPED &&
                       !NRF_TWIM0->EVENTS_ERROR && !NRF_TWIM0->ERRORSRC &&
                       NRF_TWIM0->RXD.AMOUNT == sizeof(touchBytes);
  NRF_TWIM0->SHORTS = 0;  // Wire RTC transfers use software STOP/SUSPEND
  __DMB();
  // discard incomplete transfers and recover the bus
  if (!success) {
    ++touchErrorCount;
    // count only successful bus recovery
    if (recoverBus()) ++touchRecoveryCount;
    return false;
  }
  clearTwimEvents();
  return true;
}

// discard any unread bytes after an incomplete RTC response
void drainWire() {
  // empty the core's receive queue
  while (Wire.available() > 0) {
    Wire.read();
  }
}

// read consecutive I2C registers using a repeated START
bool readRegisters(uint8_t address, uint8_t reg, uint8_t* bytes,
                   uint8_t count) {
  // avoid entering Wire while the bus remains stuck
  if (!prepareBus()) return false;
  Wire.beginTransmission(address);
  // terminate a failed register-address enqueue
  if (Wire.write(reg) != 1) {
    Wire.endTransmission();
    return false;
  }
  // abort if the slave rejects the register address
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  // discard short responses
  if (Wire.requestFrom(address, static_cast<size_t>(count)) != count) {
    drainWire();
    return false;
  }
  // copy the complete response into caller storage
  for (uint8_t i = 0; i < count; ++i) {
    const int value = Wire.read();
    // reject unexpected receive-queue exhaustion
    if (value < 0) {
      drainWire();
      return false;
    }
    bytes[i] = static_cast<uint8_t>(value);
  }
  return true;
}

// write one I2C register and check queueing plus bus acknowledgement
bool writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  // avoid entering Wire while the bus remains stuck
  if (!prepareBus()) return false;
  // send register and value in one transaction
  Wire.beginTransmission(address);
  const bool queued = Wire.write(reg) == 1 && Wire.write(value) == 1;
  const uint8_t result = Wire.endTransmission();
  return queued && result == 0;
}

// decode one BCD field within its allowed calendar range
bool decodeBcd(uint8_t raw, uint8_t minimum, uint8_t maximum,
               uint8_t& decoded) {
  const uint8_t ones = raw & 0x0f;
  const uint8_t tens = raw >> 4;
  // reject nondecimal nibbles before conversion
  if (ones > 9 || tens > 9) {
    return false;
  }
  decoded = tens * 10 + ones;
  return decoded >= minimum && decoded <= maximum;
}

// validate PCF8563 status and calendar fields, then produce UTC
bool decodeRtc(const uint8_t* bytes, uint32_t& utc) {
  /* reject STOP, oscillator tests, voltage loss and century=1;
     2000..2099 uses century=0, as written by adjust() */
  if ((bytes[0] & 0xa8) != 0 || (bytes[2] & 0x80) != 0 ||
      (bytes[7] & 0x80) != 0) {
    return false;
  }
  uint8_t second, minute, hour, day, month, year;
  // reject malformed BCD, out-of-range fields and invalid weekdays
  if (!decodeBcd(bytes[2] & 0x7f, 0, 59, second) ||
      !decodeBcd(bytes[3] & 0x7f, 0, 59, minute) ||
      !decodeBcd(bytes[4] & 0x3f, 0, 23, hour) ||
      !decodeBcd(bytes[5] & 0x3f, 1, 31, day) ||
      (bytes[6] & 0x07) > 6 ||
      !decodeBcd(bytes[7] & 0x1f, 1, 12, month) ||
      !decodeBcd(bytes[8], 0, 99, year)) {
    return false;
  }
  // month lengths before the leap-day adjustment
  constexpr uint8_t kMonthDays[] = {31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31};
  uint8_t days = kMonthDays[month - 1];
  // leap-year rule valid throughout 2000..2099
  if (month == 2 && year % 4 == 0) {
    ++days;
  }
  // reject dates that exceed the selected month's length
  if (day > days) {
    return false;
  }
  // convert the validated UTC calendar to epoch seconds
  const DateTime value(static_cast<uint16_t>(2000 + year), month, day,
                       hour, minute, second);
  utc = value.unixtime();
  return true;
}
}  // namespace

// watch-task external hardware interface
namespace board {
// initialize shared pins, battery ADC and RTC without changing calendar time
void begin() {
  startupLabel.store("pins", std::memory_order_relaxed);
  // deselect SD on the shared display SPI bus
  digitalWrite(D2, HIGH);
  pinMode(D2, OUTPUT);
  pinMode(D7, INPUT_PULLUP);  // CHSC6X active-low interrupt, polled below
  pinMode(D0, INPUT);        // no pull on the battery divider

  // configure the battery-divider ADC and calibrate its offset
  startupLabel.store("adc_setup", std::memory_order_relaxed);
  analogReadResolution(12);
  analogReference(AR_INTERNAL_3_0);
  // 470k || 470k = 235k source impedance; default 3 us acquisition is insufficient
  analogSampleTime(40);
  startupLabel.store("adc_calibrate", std::memory_order_relaxed);
  analogCalibrateOffset();

  // probe PCF8563 on the shared touch/RTC bus at 100 kHz
  startupLabel.store("wire_begin", std::memory_order_relaxed);
  Wire.begin();
  startupLabel.store("rtc_probe", std::memory_order_relaxed);
  rtcFound = prepareBus() && rtc.begin(&Wire);
  startupLabel.store("i2c_clock", std::memory_order_relaxed);
  Wire.setClock(100000);
  // disable unconnected Round Display CLKOUT to save power
  if (rtcFound) {
    startupLabel.store("rtc_clkout", std::memory_order_relaxed);
    rtcFound = writeRegister(kRtcAddress, 0x0d, 0x00);
  }
  startupLabel.store("ready", std::memory_order_relaxed);
}

// expose atomic startup progress without peripheral access
const char* startupStage() {
  return startupLabel.load(std::memory_order_relaxed);
}

// report whether the last RTC transport operation succeeded
bool rtcPresent() {
  return rtcFound;
}

// fetch and validate one coherent RTC calendar
bool readRtc(uint32_t& utc) {
  uint8_t bytes[9] = {};
  // read controls and seconds through year in one coherent transfer
  rtcFound = readRegisters(kRtcAddress, 0x00, bytes, sizeof(bytes));
  // reject transport failure before decoding
  if (!rtcFound) {
    return false;
  }
  return decodeRtc(bytes, utc);
}

// set UTC, restart the oscillator and verify readback within one second
bool writeRtc(uint32_t utc) {
  // enforce the supported century
  if (utc < kFirstSupportedUtc || utc > kLastSupportedUtc) {
    return false;
  }
  // probe before RTClib's unchecked adjust(); verify its I2C write by readback
  uint8_t control = 0;
  rtcFound = readRegisters(kRtcAddress, 0x00, &control, 1);
  // avoid writes when the RTC does not respond
  if (!rtcFound) {
    return false;
  }
  // reject a bus fault before the unchecked RTClib write
  if (!prepareBus()) return false;
  rtc.adjust(DateTime(utc));
  // checked writes clear STOP/test/reserved bits; avoid RTClib start()'s unchecked read
  if (!writeRegister(kRtcAddress, 0x00, 0x00) ||
      !writeRegister(kRtcAddress, 0x0d, 0x00)) {
    return false;
  }
  uint32_t readback = 0;
  // reject an unreadable or invalid calendar after writing
  if (!readRtc(readback)) {
    return false;
  }
  return readback >= utc && readback - utc <= 1;
}

// average Round Display battery-divider samples and return calibrated volts
float readBatteryVolts() {
  // restore shared Arduino ADC settings; preserve XIAO VBAT/current controls
  analogReadResolution(12);
  analogReference(AR_INTERNAL_3_0);
  analogSampleTime(40);
  (void)analogRead(D0);
  delayMicroseconds(100);
  uint32_t total = 0;
  // average 16 fresh conversions after the discarded settling sample
  for (unsigned i = 0; i < kBatterySamples; ++i) {
    total += analogRead(D0);
  }
  // Nordic SAADC uses 2^resolution and the selected 3.0 V range
  return static_cast<float>(total) / kBatterySamples * kAdcRangeVolts /
         4096.0f * kDividerRatio * kBatteryCalibration;
}

// poll CHSC6X interrupt and validate its native display coordinates
TouchSample readTouch() {
  TouchSample sample = {true, false, 0, 0};
  // an inactive interrupt means no pressed contact
  if (digitalRead(D7) != LOW) {
    return sample;
  }
  /* CHSC6X returns five bytes without a register write;
     bounded TWIM0 reception avoids Wire.requestFrom()'s unbounded waits */
  if (!receiveTouch()) {
    sample.valid = false;
    return sample;
  }
  // accept the controller's explicit release report
  if (touchBytes[0] == 0) {
    return sample;
  }
  // reject unsupported contact counts and off-panel coordinates
  if (touchBytes[0] != 1 || touchBytes[2] >= 240 || touchBytes[4] >= 240) {
    ++touchErrorCount;
    sample.valid = false;
    return sample;
  }
  // publish the validated single-contact position
  sample.pressed = true;
  sample.x = touchBytes[2];
  sample.y = touchBytes[4];
  return sample;
}

// return rejected touch reports and transport failures
uint32_t touchErrors() { return touchErrorCount; }
// return successful touch-bus recoveries
uint32_t touchRecoveries() { return touchRecoveryCount; }
}  // namespace board
