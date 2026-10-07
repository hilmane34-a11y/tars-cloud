#pragma once
#include <Arduino.h>

enum TarsEmotionEvent : uint8_t {
  EMOTION_NONE=0,
  EMOTION_ANGRY,
  EMOTION_HURT,
  EMOTION_SLEEPY,
  EMOTION_SURPRISED,
  EMOTION_TIRED,
  EMOTION_ARROGANT,
  EMOTION_SAD,
  EMOTION_HAPPY,
  EMOTION_JEALOUS
};

void tarsEmotionBegin();
void tarsEmotionUpdate();
void tarsEmotionResetPending();

void tarsEmotionQuestion(const String &text);
void tarsEmotionSpeechPeak(uint16_t peak,bool sttSpeech);
void tarsEmotionPeople(uint8_t count);
void tarsEmotionVisionInteresting(bool interesting);
void tarsEmotionExhausted();
void tarsEmotionMoodRise();

bool tarsEmotionHasEvent();
TarsEmotionEvent tarsEmotionTakeEvent();

String tarsEmotionPrompt(TarsEmotionEvent event);
const char* tarsEmotionName(TarsEmotionEvent event);

void tarsEmotionSpeechDone();
void tarsEmotionResetActivity();
