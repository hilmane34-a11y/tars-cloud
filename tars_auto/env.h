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
bool envAnalyze(const uint8_t* image, EnvState &result);
EnvState envGet();
