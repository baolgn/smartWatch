#include "backlight.h"

#include <Arduino.h>
#include <HardwarePWM.h>
#include <nrf_gpio.h>

// private PWM ownership and pad sampling
namespace {
// dedicated owner tag and cached backlight state
constexpr uint32_t kBacklightOwner = 0x57424c54;  // WBLT; separate from analogWrite
HardwarePWM* backlightPwm = nullptr;
uint8_t currentLevel = 255;
int16_t measuredHighPermille = -1;

// cache the electrical D6 duty fraction without changing PWM routing
void sampleBacklightOutput() {
  measuredHighPermille = -1;
  // sample only our active PWM output
  if (!backlightPwm || !backlightPwm->isOwner(kBacklightOwner) ||
      !backlightPwm->enabled() || !backlightPwm->checkPin(D6)) return;

  // allow one 255 us PWM period for the new duty cycle
  delayMicroseconds(260);
  const uint32_t physicalPin = g_ADigitalPinMap[D6];
  const auto previousInput = nrf_gpio_pin_input_get(physicalPin);
  // Nordic's watcher enables pad reads without changing output, drive, pull or PWM
  nrf_gpio_cfg_watcher(physicalPin);
  unsigned highSamples = 0;
  constexpr unsigned kSamples = 512;
  // 7 us varies phase across the 255 us PWM period; no event wait
  for (unsigned i = 0; i < kSamples; ++i) {
    delayMicroseconds(7);
    highSamples += nrf_gpio_pin_read(physicalPin) != 0;
  }
  // restore the original pad-input connection
  if (previousInput == NRF_GPIO_PIN_INPUT_DISCONNECT)
    nrf_gpio_input_disconnect(physicalPin);
  // round the sampled high fraction to permille
  measuredHighPermille = static_cast<int16_t>(
      (highSamples * 1000U + kSamples / 2) / kSamples);
}
}  // namespace

// watch-task backlight controls
namespace board {
// reserve an unused core PWM and start at full brightness
bool beginBacklight() {
  // preserve existing initialization without claiming another owner's PWM
  if (backlightPwm) {
    // reuse our existing reservation
    if (backlightPwm->isOwner(kBacklightOwner)) return true;
    measuredHighPermille = -1;
    return false;
  }
  // prevent two PWM peripherals from driving D6
  for (unsigned i = 0; i < HWPWM_MODULE_NUM; ++i) {
    // reject any existing D6 assignment
    if (HwPWMx[i]->checkPin(D6)) return false;
  }
  // try each free PWM until D6 initialization succeeds
  for (unsigned i = 0; i < HWPWM_MODULE_NUM; ++i) {
    HardwarePWM* candidate = HwPWMx[i];
    // skip peripherals reserved by other owners
    if (!candidate->takeOwnership(kBacklightOwner)) continue;
    candidate->setResolution(8);
    // 16 MHz / 16 / 255 = 3.92 kHz; avoids visible flicker
    candidate->setClockDiv(PWM_PRESCALER_PRESCALER_DIV_16);
    // release candidates without a free D6 channel
    if (!candidate->addPin(D6)) {
      candidate->releaseOwnership(kBacklightOwner);
      continue;
    }
    // undo failed startup and leave the backlight high
    if (!candidate->writePin(D6, 255)) {
      candidate->stop();
      candidate->removePin(D6);
      candidate->releaseOwnership(kBacklightOwner);
      digitalWrite(D6, HIGH);
      continue;
    }
    // retain the reservation and cache its electrical output
    backlightPwm = candidate;
    currentLevel = 255;
    sampleBacklightOutput();
    return true;
  }
  return false;  // vendor initialization leaves the backlight on
}

// update D6 duty and fail bright if the owned PWM fails
bool setBacklight(uint8_t level) {
  // reject updates without ownership
  if (!backlightPwm || !backlightPwm->isOwner(kBacklightOwner)) {
    measuredHighPermille = -1;
    return false;
  }
  // avoid resampling an unchanged duty
  if (currentLevel == level) return true;
  // release our PWM and restore full brightness on duty-change failure
  if (!backlightPwm->writePin(D6, level)) {
    backlightPwm->stop();
    // disconnect D6 before handing control back to GPIO
    if (backlightPwm->checkPin(D6)) backlightPwm->removePin(D6);
    backlightPwm->releaseOwnership(kBacklightOwner);
    backlightPwm = nullptr;
    digitalWrite(D6, HIGH);
    pinMode(D6, OUTPUT);
    currentLevel = 255;
    measuredHighPermille = -1;
    return false;
  }
  // cache the successful duty and measured pad fraction
  currentLevel = level;
  sampleBacklightOutput();
  return true;
}

// return the last applied duty
uint8_t backlightLevel() { return currentLevel; }
// return cached D6 high time, or -1 when unavailable
int16_t backlightHighPermille() { return measuredHighPermille; }
}  // namespace board
