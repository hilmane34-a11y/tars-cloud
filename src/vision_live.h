#pragma once
#include <Arduino.h>

struct VisionResult {
    bool valid;
    bool human;
    bool animal;
    uint32_t timestamp;
};

void visionLiveBegin();
bool visionLivePause();
void visionLiveResume();
String visionLiveAsk(const String &question);

// Hasil Vision terakhir
VisionResult visionLiveGetLastResult();
bool visionLiveHasValidResult(uint32_t maxAgeMs);
void visionLiveClearLastResult();
