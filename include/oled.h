#pragma once

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

extern Adafruit_SSD1306 oled;

extern bool oledOK;
extern volatile uint8_t oledSpecial;
extern volatile uint32_t oledDeadUntil;
extern volatile uint32_t oledDoorStart;

extern String oledText;
extern String oledStatus;

extern uint32_t oledTypePos;
extern uint32_t oledLastType;
extern uint32_t oledLastWave;
extern uint32_t oledPage;
extern uint32_t oledLastPage;

void oledSetStatus(const String& s);
void oledSetListening();
void oledStartSpeak(const String& s);
void oledShowText(const String& s, const String& status);

void drawSpecialOLED(uint8_t mode);
void oledTask(void* parameter);
