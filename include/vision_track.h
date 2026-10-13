#pragma once

#include <Arduino.h>

struct VisionTrackResult {
    bool found;
    float score;
    float centerX;
    int targetX;
};

void visionTrackBegin();

bool visionTrackPerson(
    VisionTrackResult &result
);
