#pragma once
#include <Arduino.h>

void visionLiveBegin();
bool visionLivePause();
void visionLiveResume();
String visionLiveAsk(const String &question);
