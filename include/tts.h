#pragma once

#include <Arduino.h>

extern bool playing;
extern bool dacOK;

bool initDAC();
bool audioStart();
void audioStop();

bool playLocalMP3(
  const uint8_t* start,
  const uint8_t* end,
  const String& name,
  bool alarm = false
);

bool playLocalAlarm();

bool streamAudio(
  const String& url,
  const String& text
);
