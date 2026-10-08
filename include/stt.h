#pragma once

#include <Arduino.h>

constexpr uint32_t STT_NORMAL_COOLDOWN = 1000;
constexpr uint32_t STT_ERROR_COOLDOWN = 30000;
constexpr uint32_t STT_QUOTA_COOLDOWN = 60000;
constexpr uint32_t STT_RECONNECT_GUARD = 60000;
constexpr uint32_t STT_IDLE_TIMEOUT_MS = 10000;

extern String sttFinal;
extern String sttPartial;
extern bool micOK;

bool wifiOK();

extern bool sttConnected;
extern bool sttReady;
extern bool sttDone;
extern bool sttError;
extern bool sttClosing;

bool sttCooling();
void closeSTT(uint32_t cooldown = STT_NORMAL_COOLDOWN);
bool startSTT(bool offline = false);
String stopSTT(uint32_t samples, bool offline = false);
String recordSTT(bool offline);
String recordRealtime();
String recordOffline();
