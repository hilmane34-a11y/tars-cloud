#pragma once
#include <Arduino.h>

struct VisionResult {
    bool valid;
    bool human;
    bool animal;
    uint32_t timestamp;
};

struct VisionTrackResult {
    bool valid;
    bool found;
    int targetX;
    float centerX;
    uint32_t timestamp;
};

enum VisionAim {
    AIM_CENTER,
    AIM_LEFT,
    AIM_RIGHT,
    AIM_RETURN_CENTER,
    AIM_STOP
};

void visionLiveBegin();
bool visionLivePause();
void visionLiveResume();
String visionLiveAsk(const String &question);

VisionResult visionLiveGetLastResult();
bool visionLiveHasValidResult(uint32_t maxAgeMs);
void visionLiveClearLastResult();

VisionTrackResult visionLiveTrack();
VisionAim visionLiveGetAim();
void visionLiveClearTrack();
