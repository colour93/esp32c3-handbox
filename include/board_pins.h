#pragma once

#include <Arduino.h>

namespace BoardPins {

constexpr uint8_t kOledClock = 4;
constexpr uint8_t kOledData = 6;
constexpr uint8_t kOledChipSelect = 7;
constexpr uint8_t kOledDataCommand = 10;
constexpr uint8_t kOledReset = 5;

constexpr uint8_t kEncoderS1 = 0;
constexpr uint8_t kEncoderS2 = 1;
constexpr uint8_t kEncoderKey = 3;

}  // namespace BoardPins
