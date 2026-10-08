#pragma once

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

constexpr uint32_t OLED_TYPE_MS = 39;
constexpr uint32_t OLED_WAVE_MS = 70;
constexpr uint32_t OLED_PAGE_MS = 2200;

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

void drawSpecialOLED(uint8_t m);
void drawCameraOLED();
void oledTask(void*);
