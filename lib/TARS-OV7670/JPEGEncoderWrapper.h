#pragma once
#include <Arduino.h>

class JPEGEncoderWrapper {
public:
  static bool encode(const uint8_t* rgb565, int xres, int yres, int quality, uint8_t* outBuffer, size_t* outLen);
  static bool available();
};
