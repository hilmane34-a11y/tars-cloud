#pragma once
#include <Arduino.h>

enum EnvEvent:uint8_t{
  ENV_NONE,
  ENV_MOTION_LEFT,
  ENV_MOTION_CENTER,
  ENV_MOTION_RIGHT,
  ENV_SCENE_CHANGED
};

enum EnvColor:uint8_t{
  ENV_COLOR_UNKNOWN,
  ENV_COLOR_BLACK,
  ENV_COLOR_WHITE,
  ENV_COLOR_GRAY,
  ENV_COLOR_RED,
  ENV_COLOR_ORANGE,
  ENV_COLOR_YELLOW,
  ENV_COLOR_GREEN,
  ENV_COLOR_CYAN,
  ENV_COLOR_BLUE,
  ENV_COLOR_PURPLE
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

  EnvColor dominantColor;
  uint8_t colorConfidence;

  EnvColor leftColor;
  EnvColor centerColor;
  EnvColor rightColor;

  uint8_t leftColorConfidence;
  uint8_t centerColorConfidence;
  uint8_t rightColorConfidence;

  // DETEKSI REFLEKS TERKEJUT
  bool surprise;
  uint8_t surpriseConfidence;
};

void envBegin();
void envResetMotion();

bool envAnalyze(
  const uint16_t *image,
  EnvState &result
);

bool envAnalyze(
  const uint16_t *image,
  uint8_t dominantColor,
  uint8_t colorConfidence,
  EnvState &result
);

EnvState envGet();
