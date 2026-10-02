#pragma once
#include <Arduino.h>

struct PersonalityState {
  float energy;
  float boredom;
  float curiosity;
  float fatigue;
  int8_t mood; // 0=normal, 1=bosan, 2=lelah, 3=penasaran
};

void personalityBegin();
void personalityUpdate(bool busy, bool explored, bool spoke);
PersonalityState personalityGet();
bool personalityCanExplore();
bool personalityWantsSpeak();
void personalitySpeechDone();
