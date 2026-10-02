#pragma once
#include <Arduino.h>

void visionLiveBegin();
bool visionLivePause(bool captureVision);
void visionLiveResume();
String visionLiveAsk(const String &question);
