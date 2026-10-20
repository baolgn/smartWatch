#pragma once

#include <stdint.h>

// watch-task backlight controls
namespace board {
// watch task only, after display initialization; reserve an unowned core PWM
bool beginBacklight();
// set active-high D6 duty: 0 off, 255 full
bool setBacklight(uint8_t level);
// return the last applied duty
uint8_t backlightLevel();
/* cached D6 high fraction after duty changes: 0..1000, -1 if PWM unavailable;
   measures neither LED brightness nor KE switch state */
int16_t backlightHighPermille();
}  // namespace board
