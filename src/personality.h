#pragma once
#include <Arduino.h>

enum TarsEmotion:uint8_t{
  EMOTION_NEUTRAL,
  EMOTION_HAPPY,
  EMOTION_SAD,
  EMOTION_ANGRY,
  EMOTION_SURPRISED
};

struct PersonalityState{
  float energy;
  float boredom;
  float curiosity;
  float fatigue;
  int8_t mood;
  TarsEmotion emotion;
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

void personalitySetEmotion(TarsEmotion emotion);
TarsEmotion personalityGetEmotion();
