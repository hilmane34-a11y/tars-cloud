#pragma once
#include <Arduino.h>

// Dipertahankan untuk kompatibilitas dengan kode lama.
// Koneksi persisten tidak menggunakan cooldown normal.
constexpr uint32_t STT_NORMAL_COOLDOWN = 1000;
constexpr uint32_t STT_ERROR_COOLDOWN = 30000;
constexpr uint32_t STT_IDLE_TIMEOUT_MS = 10000;

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
void sttLoop();
void closeSTT(uint32_t cooldown = STT_NORMAL_COOLDOWN);
bool startSTT();
String stopSTT(uint32_t samples);
String recordRealtime();
