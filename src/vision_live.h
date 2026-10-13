#pragma once
#include <Arduino.h>

void visionLiveBegin();

bool visionLivePause();
void visionLiveResume();
String visionLiveAsk(const String &question);

// Giliran khusus DETR
bool visionLiveTrackPause();
bool visionLiveTrackCapture(size_t &length);
uint8_t *visionLiveSharedJpeg();
void visionLiveTrackResume();
