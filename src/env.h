#pragma once
#include <Arduino.h>

enum EnvEvent:uint8_t{
  ENV_NONE,
  ENV_MOTION_LEFT,
  ENV_MOTION_CENTER,
  ENV_MOTION_RIGHT,
  ENV_SCENE_CHANGED
};

struct EnvState{
  bool valid;
  bool obstacle;
  bool leftClear;
  bool centerClear;
  bool rightClear;
  uint8_t confidence;
  uint8_t leftBright;
  uint8_t centerBright;
  uint8_t rightBright;
  bool motion;
  EnvEvent event;
  uint8_t motionLevel;
};

void envBegin();
void envResetMotion();
bool envAnalyze(const uint8_t*image,EnvState&result);
EnvState envGet();
