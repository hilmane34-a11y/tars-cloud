#pragma once
#include <Arduino.h>

void wheelsBegin();
void wheelsDrive(int16_t left, int16_t right);
void wheelsForward(uint8_t speed);
void wheelsBackward(uint8_t speed);
void wheelsLeft(uint8_t speed);
void wheelsRight(uint8_t speed);
void wheelsStop();
