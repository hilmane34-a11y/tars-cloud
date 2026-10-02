#pragma once
#include <Arduino.h>

struct EnvState {
  bool valid;
  bool obstacle;
  bool leftClear;
  bool centerClear;
  bool rightClear;
  uint8_t confidence;
};

void envBegin();
bool envAnalyze(const uint8_t* frame, uint16_t width,
                uint16_t height, EnvState &result);
EnvState envGet();
