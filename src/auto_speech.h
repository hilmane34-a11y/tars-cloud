
#pragma once
#include <Arduino.h>

typedef bool (*AutoSpeechCallback)(const String &prompt);

void autoSpeechBegin(AutoSpeechCallback callback);

void autoSpeechUpdate(
  bool enabled,
  bool listening,
  bool speaking
);

void autoSpeechNotifyVisionEvent(EnvEvent event);

void autoSpeechDone();
