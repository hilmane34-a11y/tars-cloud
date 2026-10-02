#pragma once
#include <Arduino.h>

void autonomyBegin();
void autonomyStop();
void autonomySetSafety(bool cameraValid,bool pathClear);
void autonomyUpdate(bool enabled,bool busy);
bool autonomyIsMoving();
