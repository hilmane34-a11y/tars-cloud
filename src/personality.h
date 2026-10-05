#pragma once
#include <Arduino.h>

struct PersonalityState {
  float energy;
  float boredom;
  float curiosity;
  float fatigue;
  int8_t mood;
};

void personalityBegin();
void personalityUpdate(bool busy,bool explored,bool spoke);
PersonalityState personalityGet();
bool personalityCanExplore();
bool personalityWantsSpeak();
void personalitySpeechDone();
bool personalityNeedsRest();
bool personalityIsResting();
void personalityStartRest();
void personalityStopRest();
