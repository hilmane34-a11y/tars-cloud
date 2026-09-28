#pragma once
#include <Arduino.h>

class JPEGEncoderWrapper {
public:
  static bool begin(
    uint8_t* outBuffer,
    size_t outCapacity,
    int xres,
    int yres,
    int quality
  );

  static bool addBlock(
    const uint8_t* rgb565,
    int width,
    int height
  );

  static bool finish(
    size_t* outLen
  );

  static bool available();
};
