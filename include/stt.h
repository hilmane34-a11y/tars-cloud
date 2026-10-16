#pragma once
#include <Arduino.h>

extern String sttFinal;
extern String sttPartial;

extern bool micOK;
extern bool sttConnected;
extern bool sttReady;
extern bool sttDone;
extern bool sttError;
extern bool sttClosing;

bool wifiOK();
bool sttCooling();

void closeSTT();
bool startSTT();
String stopSTT(uint32_t samples);
String recordRealtime();
