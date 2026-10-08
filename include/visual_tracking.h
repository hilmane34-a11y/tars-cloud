#pragma once
#include <Arduino.h>

enum VisualTrackState:uint8_t{
  TRACK_IDLE=0,
  TRACKING,
  TRACK_LOST,
  TRACK_RETURNING
};

void visualTrackingBegin();
void visualTrackingUpdate(bool active,bool busy);
void visualTrackingTarget(int16_t x,bool valid);
void visualTrackingLost();
VisualTrackState visualTrackingState();
bool visualTrackingIsMoving();
