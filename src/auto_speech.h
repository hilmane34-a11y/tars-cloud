#pragma once
#include <Arduino.h>
#include "env.h"

typedef bool (*AutoSpeechCallback)(const String &prompt);

void autoSpeechBegin(AutoSpeechCallback callback);

void autoSpeechUpdate(
  bool enabled,
  bool listening,
  bool speaking
);

void autoSpeechNotifyVisionEvent(EnvEvent event);
void autoSpeechNotifyVision(const String &description);

void autoSpeechDone();
