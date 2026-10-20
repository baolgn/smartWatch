#pragma once

#include <stdint.h>

// watch-task-only hardware access; USB diagnostics use cached values
namespace board {
// distinguish a new sample, no new sample and a sensor fault
enum class OrientationRead : uint8_t { Fresh, NotReady, Error };

// original XIAO Sense LSM6DS3TR-C uses separate I2C; enable accelerometer, keep gyro off
bool beginOrientation();
// read fresh signed screen-normal acceleration; screen-up is positive
OrientationRead readFaceUpMilliG(int16_t& milliG);
// report successful initialization without a subsequent failure
bool orientationReady();
// return accumulated sensor failures
uint32_t orientationErrors();
// last valid signed Z measurement before mounting correction
int16_t orientationRawZMilliG();
// last failure stage survives retries; read in watch task, cache for USB
const char* orientationFailureStage();
// return the last failed register address
uint8_t orientationFailureRegister();
// return the byte observed at the last failure
uint8_t orientationFailureObserved();
/* retained failure: events[5:0], ERRORSRC[10:8], TX count[15:12], RX count[19:16],
   requested TX[23:20], requested RX[27:24], SDA high[28], SCL high[29] */
uint32_t orientationFailureDetail();
}  // namespace board
