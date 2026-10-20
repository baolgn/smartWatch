#include "orientation_sensor.h"

#include <Arduino.h>
#include <nrf_gpio.h>

// private internal IMU registers, bus state and transfers
namespace {
// LSM6DS3TR-C address and registers used for Z-only sampling
constexpr uint8_t kAddress = 0x6a;
constexpr uint8_t kWhoAmI = 0x0f;
constexpr uint8_t kCtrl1Xl = 0x10;
constexpr uint8_t kCtrl2G = 0x11;
constexpr uint8_t kCtrl3C = 0x12;
constexpr uint8_t kCtrl4C = 0x13;
constexpr uint8_t kCtrl6C = 0x15;
constexpr uint8_t kStatus = 0x1e;
constexpr uint8_t kOutZLow = 0x2c;
// bound waits by elapsed time and poll count
constexpr uint32_t kTransferTimeoutMs = 5;
constexpr unsigned kTransferPollLimit = 5000;
constexpr uint32_t kStopTimeoutMs = 2;
constexpr unsigned kStopPollLimit = 2000;
/* XIAO components face away from the screen; ST +Z exits the IMU top,
   so screen-up reads about -1 g before mounting correction */
constexpr int32_t kScreenUpSign = -1;

// watch-task readiness, last Z reading and retained failure diagnostics
bool ready = false;
uint32_t errorCount = 0;
int16_t lastRawZ = 0;
const char* failureStage = "none";
uint8_t failureRegister = 0;
uint8_t failureObserved = 0;
uint32_t failureDetail = 0;
// static EasyDMA storage survives timeout/STOP failure; disable DMA before reuse
alignas(4) uint8_t txBytes[2] = {};
alignas(4) uint8_t rxBytes[2] = {};

// clear TWIM1 shortcuts, transfer events and write-one-to-clear errors
void clearEvents() {
  NRF_TWIM1->SHORTS = 0;
  NRF_TWIM1->EVENTS_RXSTARTED = 0;
  NRF_TWIM1->EVENTS_TXSTARTED = 0;
  NRF_TWIM1->EVENTS_LASTRX = 0;
  NRF_TWIM1->EVENTS_LASTTX = 0;
  NRF_TWIM1->EVENTS_STOPPED = 0;
  NRF_TWIM1->EVENTS_SUSPENDED = 0;
  NRF_TWIM1->EVENTS_ERROR = 0;
  NRF_TWIM1->ERRORSRC = NRF_TWIM1->ERRORSRC;  // write one to clear
}

// retain failure evidence, disable DMA and require sensor reinitialization
bool fail(const char* stage) {
  // snapshot stage, register, observed byte, events, counts and bus levels
  failureStage = stage;
  failureRegister = txBytes[0];
  failureObserved = rxBytes[0];
  failureDetail = (NRF_TWIM1->EVENTS_STOPPED ? 1UL : 0) |
      (NRF_TWIM1->EVENTS_TXSTARTED ? 2UL : 0) |
      (NRF_TWIM1->EVENTS_LASTTX ? 4UL : 0) |
      (NRF_TWIM1->EVENTS_RXSTARTED ? 8UL : 0) |
      (NRF_TWIM1->EVENTS_LASTRX ? 16UL : 0) |
      (NRF_TWIM1->EVENTS_ERROR ? 32UL : 0) |
      ((NRF_TWIM1->ERRORSRC & 7UL) << 8) |
      ((NRF_TWIM1->TXD.AMOUNT & 15UL) << 12) |
      ((NRF_TWIM1->RXD.AMOUNT & 15UL) << 16) |
      ((NRF_TWIM1->TXD.MAXCNT & 15UL) << 20) |
      ((NRF_TWIM1->RXD.MAXCNT & 15UL) << 24) |
      (digitalRead(PIN_WIRE1_SDA) == HIGH ? (1UL << 28) : 0) |
      (digitalRead(PIN_WIRE1_SCL) == HIGH ? (1UL << 29) : 0);
  // disable DMA before clearing events or reusing static buffers
  NRF_TWIM1->ENABLE = TWIM_ENABLE_ENABLE_Disabled;
  __DMB();
  clearEvents();
  ready = false;
  ++errorCount;
  return false;
}

// require both internal I2C lines high before starting a transfer
bool busIdle() {
  return digitalRead(PIN_WIRE1_SDA) == HIGH &&
         digitalRead(PIN_WIRE1_SCL) == HIGH;
}

// connect the dedicated IMU bus to TWIM1 at 100 kHz
void configureBus() {
  // map internal SDA/SCL with open-drain behavior and pull-ups
  const uint32_t sda = g_ADigitalPinMap[PIN_WIRE1_SDA];
  const uint32_t scl = g_ADigitalPinMap[PIN_WIRE1_SCL];
  nrf_gpio_cfg(sda, NRF_GPIO_PIN_DIR_INPUT, NRF_GPIO_PIN_INPUT_CONNECT,
               NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_S0D1, NRF_GPIO_PIN_NOSENSE);
  nrf_gpio_cfg(scl, NRF_GPIO_PIN_DIR_INPUT, NRF_GPIO_PIN_INPUT_CONNECT,
               NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_S0D1, NRF_GPIO_PIN_NOSENSE);
  NRF_TWIM1->PSEL.SDA = sda;
  NRF_TWIM1->PSEL.SCL = scl;
  NRF_TWIM1->FREQUENCY = TWIM_FREQUENCY_FREQUENCY_K100;
  // poll TWIM1 events with its interrupts disabled; global interrupts stay enabled
  NRF_TWIM1->INTENCLR = 0xffffffffUL;
  clearEvents();
  NRF_TWIM1->ENABLE = TWIM_ENABLE_ENABLE_Enabled;
}

// bounded TWIM1 transfer; avoid unbounded Wire waits and preserve TWIM0 touch/RTC state
bool transfer(uint8_t txCount, uint8_t rxCount) {
  // fail immediately if the internal bus is held low
  if (!busIdle()) return fail("bus_low");
  // arm static DMA buffers and hardware TX/RX/STOP shortcuts
  clearEvents();
  NRF_TWIM1->ADDRESS = kAddress;
  NRF_TWIM1->TXD.PTR = reinterpret_cast<uintptr_t>(txBytes);
  NRF_TWIM1->TXD.MAXCNT = txCount;
  NRF_TWIM1->RXD.PTR = reinterpret_cast<uintptr_t>(rxBytes);
  NRF_TWIM1->RXD.MAXCNT = rxCount;
  NRF_TWIM1->SHORTS = rxCount ? TWIM_SHORTS_LASTTX_STARTRX_Msk |
                                   TWIM_SHORTS_LASTRX_STOP_Msk
                             : TWIM_SHORTS_LASTTX_STOP_Msk;
  __DMB();
  NRF_TWIM1->TASKS_RESUME = 1;
  NRF_TWIM1->TASKS_STARTTX = 1;
  const uint32_t started = millis();
  unsigned polls = 0;
  // poll completion; the count limit also bounds a stalled scheduler tick
  while (!NRF_TWIM1->EVENTS_STOPPED && !NRF_TWIM1->EVENTS_ERROR &&
         static_cast<uint32_t>(millis() - started) < kTransferTimeoutMs &&
         polls++ < kTransferPollLimit) {
    delayMicroseconds(1);
  }
  // request a manual STOP after error or timeout
  if (!NRF_TWIM1->EVENTS_STOPPED) {
    NRF_TWIM1->SHORTS = 0;
    NRF_TWIM1->TASKS_RESUME = 1;
    NRF_TWIM1->TASKS_STOP = 1;
    const uint32_t stopping = millis();
    polls = 0;
    // bound STOP completion by time and poll count
    while (!NRF_TWIM1->EVENTS_STOPPED &&
           static_cast<uint32_t>(millis() - stopping) < kStopTimeoutMs &&
           polls++ < kStopPollLimit) {
      delayMicroseconds(1);
    }
  }
  // require complete TX and any requested RX without hardware errors
  const bool success = NRF_TWIM1->EVENTS_STOPPED &&
                       NRF_TWIM1->EVENTS_TXSTARTED &&
                       NRF_TWIM1->EVENTS_LASTTX &&
                       !NRF_TWIM1->EVENTS_ERROR && !NRF_TWIM1->ERRORSRC &&
                       NRF_TWIM1->TXD.AMOUNT == txCount &&
                       (!rxCount || (NRF_TWIM1->EVENTS_RXSTARTED &&
                                     NRF_TWIM1->EVENTS_LASTRX &&
                                     NRF_TWIM1->RXD.AMOUNT == rxCount));
  // clear transfer shortcuts before the next operation
  NRF_TWIM1->SHORTS = 0;
  __DMB();
  // disable failed transfers before buffer reuse
  if (!success) return fail("transfer");
  clearEvents();
  return true;
}

// send one IMU register/value pair
bool writeRegister(uint8_t reg, uint8_t value) {
  txBytes[0] = reg;
  txBytes[1] = value;
  return transfer(2, 0);
}

// select an IMU register and read consecutive bytes
bool readRegisters(uint8_t reg, uint8_t count) {
  txBytes[0] = reg;
  return transfer(1, count);
}

// verify IMU identity or configuration by readback
bool verifyRegister(uint8_t reg, uint8_t expected) {
  // propagate transport failures without comparing stale data
  if (!readRegisters(reg, 1)) return false;
  return rxBytes[0] == expected || fail("verify");
}
}  // namespace

// watch-task orientation sensor interface
namespace board {
// power-cycle, identify and configure the IMU for low-power Z sampling
bool beginOrientation() {
  // disable the dedicated peripheral before pin or power changes
  ready = false;
  NRF_TWIM1->ENABLE = TWIM_ENABLE_ENABLE_Disabled;
  __DMB();
  // power-cycle only the IMU to clear its bus; high-impedance I/O prevents parasitic power
  pinMode(PIN_WIRE1_SDA, INPUT);
  pinMode(PIN_WIRE1_SCL, INPUT);
  pinMode(PIN_LSM6DS3TR_C_INT1, INPUT);
  digitalWrite(PIN_LSM6DS3TR_C_POWER, LOW);
  pinMode(PIN_LSM6DS3TR_C_POWER, OUTPUT);
  /* P1.08 powers the IMU directly; use H0H1, as in Seeed's beginCore:
     https://github.com/Seeed-Studio/Seeed_Arduino_LSM6DS3/blob/master/LSM6DS3.cpp */
  nrf_gpio_cfg(g_ADigitalPinMap[PIN_LSM6DS3TR_C_POWER],
               NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_DISCONNECT,
               NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_H0H1, NRF_GPIO_PIN_NOSENSE);
  delay(2);  // ST AN5130: at least 100 us off
  digitalWrite(PIN_LSM6DS3TR_C_POWER, HIGH);
  delay(20);  // ST AN5130: 15 ms power-on boot
  configureBus();
  // reject an unexpected or unresponsive IMU
  if (!verifyRegister(kWhoAmI, 0x6a)) return false;
  // reset modes left by previous firmware
  if (!writeRegister(kCtrl3C, 0x01)) return false;
  bool resetComplete = false;
  // wait at most 100 one-millisecond polls for soft reset
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    // stop immediately if reset-status reads fail
    if (!readRegisters(kCtrl3C, 1)) return false;
    // a cleared reset bit confirms completion
    if (!(rxBytes[0] & 0x01)) {
      resetComplete = true;
      break;
    }
    delay(1);
  }
  // keep initialization failed if reset never completes
  if (!resetComplete) return fail("reset_timeout");
  /* BDU locks the selected LSB/MSB pair; OUTZ_H clears XLDA;
     ST AN5130 sections 4.3/4.4 permit coherent single-axis reads */
  if (!writeRegister(kCtrl3C, 0x44) ||  // BDU + auto-increment; little endian
      !writeRegister(kCtrl2G, 0x00) ||  // gyro off
      !writeRegister(kCtrl6C, 0x10) ||  // XL_HM_MODE=1: low power at 26 Hz
      !writeRegister(kCtrl4C, 0x08) ||  // mask data-ready until filters settle
      !writeRegister(kCtrl1Xl, 0x20) || // 26 Hz, +/-2 g; retain gravity, no high-pass
      !verifyRegister(kCtrl3C, 0x44) || !verifyRegister(kCtrl2G, 0x00) ||
      !verifyRegister(kCtrl6C, 0x10) || !verifyRegister(kCtrl4C, 0x08) ||
      !verifyRegister(kCtrl1Xl, 0x20)) {
    return false;
  }
  // publish readiness only after all settings pass readback
  ready = true;
  return true;
}

// read fresh Z acceleration and normalize screen-up to positive mg
OrientationRead readFaceUpMilliG(int16_t& milliG) {
  // reject reads until initialization succeeds
  if (!ready) return OrientationRead::Error;
  // propagate status-read failures
  if (!readRegisters(kStatus, 1)) return OrientationRead::Error;
  // distinguish an unchanged sample from an error
  if (!(rxBytes[0] & 0x01)) return OrientationRead::NotReady;
  // read only Z; no X/Y, gyro or temperature outputs
  if (!readRegisters(kOutZLow, 2)) return OrientationRead::Error;
  // assemble the little-endian Z register pair
  const uint16_t bits = static_cast<uint16_t>(rxBytes[0]) |
                        (static_cast<uint16_t>(rxBytes[1]) << 8);
  // decode two's complement without an implementation-defined cast
  const int32_t raw = bits & 0x8000 ? static_cast<int32_t>(bits) - 65536
                                  : static_cast<int32_t>(bits);
  // scale and round using ST's 0.061 mg/LSB at +/-2 g, then apply mounting sign
  const int32_t scaled = raw * 61;
  lastRawZ = static_cast<int16_t>((scaled + (scaled < 0 ? -500 : 500)) / 1000);
  milliG = static_cast<int16_t>(kScreenUpSign * lastRawZ);
  return OrientationRead::Fresh;
}

// report whether IMU initialization remains valid
bool orientationReady() { return ready; }
// return accumulated initialization and transfer failures
uint32_t orientationErrors() { return errorCount; }
// return the last signed Z reading before mounting correction
int16_t orientationRawZMilliG() { return lastRawZ; }
// return the retained failure stage
const char* orientationFailureStage() { return failureStage; }
// return the register targeted by the failed transfer
uint8_t orientationFailureRegister() { return failureRegister; }
// return the received byte retained at failure
uint8_t orientationFailureObserved() { return failureObserved; }
// return packed transfer events, counts and pin levels at failure
uint32_t orientationFailureDetail() { return failureDetail; }
}  // namespace board
