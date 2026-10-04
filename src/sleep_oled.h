#pragma once
#include <Arduino.h>
#include <Adafruit_SSD1306.h>

void sleepOLEDStart();
void sleepOLEDStop();
void sleepOLEDUpdate(Adafruit_SSD1306 &oled);
