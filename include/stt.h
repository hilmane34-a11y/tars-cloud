#pragma once

#include <Arduino.h>
#include "vision_track.h"

constexpr uint32_t STT_NORMAL_COOLDOWN = 1000;
constexpr uint32_t STT_ERROR_COOLDOWN = 30000;
constexpr uint32_t STT_QUOTA_COOLDOWN = 60000;
constexpr uint32_t STT_RECONNECT_GUARD = 60000;
constexpr uint32_t STT_IDLE_TIMEOUT_MS = 10000;

extern String sttFinal;
extern String sttPartial;

extern bool micOK;

extern bool sttConnected;
extern bool sttReady;
extern bool sttDone;
extern bool sttError;
extern bool sttClosing;
extern volatile bool sttSessionActive;

bool wifiOK();
bool sttCooling();

void sttService();

void closeSTT(
    uint32_t cooldown = STT_NORMAL_COOLDOWN
);

bool startSTT(bool offline = false);

String stopSTT(
    uint32_t samples,
    bool offline = false
);

String recordSTT(bool offline);
String recordRealtime();
String recordOffline();

bool sttTrackDetect(
    const uint8_t *jpeg,
    size_t length,
    VisionTrackResult &result,
    uint32_t timeoutMs = 8000
);
