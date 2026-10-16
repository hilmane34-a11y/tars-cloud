#include "stt.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <math.h>
#include <string.h>
#include "config.h"
#include "tars_emotion.h"

#define MIC_PORT I2S_NUM_1
#define MIC_RATE 16000
#define MIC_THRESHOLD 10000
#define MIC_SILENCE 7000
#define RECORD_MIN_MS 500
#define SILENCE_MS 800
#define PREROLL_MS 200
#define STT_CONNECT_TIMEOUT 20000

static const size_t BUF = 256;
static const size_t PREROLL_SAMPLES = MIC_RATE * PREROLL_MS / 1000;

static int32_t rawBuf[BUF / 4];
static int16_t pcmBuf[BUF / 4];
static int16_t preBuf[PREROLL_SAMPLES];
static int16_t sendBuf[256];

static WebSocketsClient sttWS;
static bool sttStarted = false;

static const char *STT_HOST_LOCAL =
  "tars-cloud-v1.hilmane34.workers.dev";

String sttFinal, sttPartial;

bool micOK = false;
bool sttConnected = false;
bool sttReady = false;
bool sttDone = false;
bool sttError = false;
bool sttClosing = false;

extern bool wifiOK();
extern void oledSetStatus(const String &);
extern void oledSetListening();

bool sttCooling() {
  return false;
}

void closeSTT() {
  sttClosing = true;

  if (sttStarted) {
    sttWS.setReconnectInterval(0);
    sttWS.disconnect();
  }

  sttStarted = false;
  sttConnected = false;
  sttReady = false;
  sttClosing = false;

  Serial.println("TARS: STT WS CLOSED");
}

static void sttEvent(
  WStype_t type,
  uint8_t *payload,
  size_t length
) {
  if (type == WStype_CONNECTED) {
    sttConnected = true;
    sttError = false;
    Serial.println("TARS: STT WS CONNECTED");
    return;
  }

  if (type == WStype_DISCONNECTED) {
    sttConnected = false;
    sttReady = false;

    if (!sttClosing && !sttDone) {
      sttError = true;
    }

    Serial.println("TARS: STT WS DISCONNECTED");
    return;
  }

  if (type == WStype_ERROR) {
    sttConnected = false;
    sttReady = false;

    if (!sttClosing) {
      sttError = true;
    }

    Serial.println("TARS: STT WS ERROR");
    return;
  }

  if (type != WStype_TEXT || !payload || !length) {
    return;
  }

  String msg;
  if (!msg.reserve(length + 1)) {
    sttError = true;
    Serial.println("TARS: STT JSON ALLOC FAILED");
    return;
  }

  for (size_t i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  JsonDocument j;
  if (deserializeJson(j, msg)) {
    Serial.println("TARS: STT JSON PARSE FAILED");
    return;
  }

  const char *typeText = j["type"] | "";

  if (strcmp(typeText, "ready") == 0) {
    sttReady = true;
    sttError = false;
    Serial.println("TARS: STT READY");
    oledSetListening();
  } else if (strcmp(typeText, "partial") == 0) {
    sttPartial = j["text"] | "";
    sttPartial.trim();

    if (sttPartial.length()) {
      Serial.printf(
        "TARS: STT PARTIAL = %s\n",
        sttPartial.c_str()
      );
    }
  } else if (strcmp(typeText, "final") == 0) {
    sttFinal = j["text"] | "";
    sttFinal.trim();
    sttDone = true;

    Serial.printf(
      "TARS: STT FINAL = %s\n",
      sttFinal.c_str()
    );
  } else if (strcmp(typeText, "error") == 0) {
    sttError = true;
    sttDone = true;

    const char *err = j["error"] | "unknown";
    Serial.printf("TARS: STT ERROR = %s\n", err);
    oledSetStatus("STT ERROR");
  }
}

bool startSTT() {
  if (!wifiOK() || !micOK) {
    return false;
  }

  if (sttConnected && sttReady) {
    return true;
  }

  closeSTT();

  sttDone = false;
  sttError = false;
  sttFinal = "";
  sttPartial = "";

  sttWS.onEvent(sttEvent);
  sttWS.setReconnectInterval(0);
  sttWS.enableHeartbeat(15000, 5000, 2);

  Serial.printf(
    "TARS: STT TLS BEGIN heap=%u maxBlock=%u RSSI=%d\n",
    ESP.getFreeHeap(),
    ESP.getMaxAllocHeap(),
    WiFi.RSSI()
  );

  sttWS.beginSSL(STT_HOST_LOCAL, 443, "/stt");
  sttStarted = true;

  uint32_t startedAt = millis();

  while (!sttReady && !sttError &&
         millis() - startedAt < STT_CONNECT_TIMEOUT) {
    sttWS.loop();
    delay(2);
    yield();
  }

  if (!sttConnected || !sttReady || sttError) {
    Serial.println("TARS: STT CONNECT FAILED");
    closeSTT();
    sttDone = false;
    sttError = false;
    return false;
  }

  Serial.println("TARS: STT READY - WAITING FOR SPEECH");
  return true;
}

String stopSTT(uint32_t samples) {
  if (!sttConnected || !sttReady) {
    closeSTT();
    return "";
  }

  JsonDocument j;
  j["type"] = "end";
  j["timestamp"] = (double)samples / MIC_RATE;

  String msg;
  if (serializeJson(j, msg) == 0 || !sttWS.sendTXT(msg)) {
    Serial.println("TARS: STT END SEND FAILED");
    closeSTT();
    sttDone = false;
    sttError = false;
    return "";
  }

  uint32_t startedAt = millis();

  while (!sttDone && !sttError &&
         millis() - startedAt < 6000) {
    sttWS.loop();
    delay(2);
    yield();
  }

  String result = sttDone ? sttFinal : "";
  result.trim();

  if (!sttDone) {
    Serial.println("TARS: STT FINAL TIMEOUT");
  }

  // Tutup WebSocket sebelum main masuk ke TLS /ask dan TTS.
  closeSTT();

  sttDone = false;
  sttError = false;
  sttFinal = "";
  sttPartial = "";

  Serial.printf(
    "TARS: STT FINISHED result=%u\n",
    (unsigned)result.length()
  );

  return result;
}

String recordRealtime() {
  if (!startSTT()) {
    return "";
  }

  oledSetListening();

  size_t prePos = 0;
  size_t preCount = 0;

  uint32_t voiceStart = 0;
  uint32_t lastVoice = 0;
  uint32_t samples = 0;
  bool voice = false;

  for (;;) {
    sttWS.loop();

    if (sttError || !sttConnected || !sttReady) {
      Serial.println("TARS: STT SESSION LOST");
      closeSTT();
      sttDone = false;
      sttError = false;
      return "";
    }

    size_t bytes = 0;
    if (i2s_read(
          MIC_PORT,
          rawBuf,
          sizeof(rawBuf),
          &bytes,
          pdMS_TO_TICKS(30)
        ) != ESP_OK) {
      continue;
    }

    size_t count = bytes / sizeof(int32_t);
    if (count > BUF / 4) count = BUF / 4;
    if (!count) continue;

    int32_t peak = 0;
    uint64_t sum = 0;

    for (size_t i = 0; i < count; i++) {
      int32_t v = constrain(
        rawBuf[i] >> 16,
        -32768,
        32767
      );

      pcmBuf[i] = (int16_t)v;

      int32_t a = abs(v);
      if (a > peak) peak = a;
      sum += (uint64_t)a * a;
    }

    uint32_t rms =
      (uint32_t)sqrt((double)sum / count);

    if (!voice) {
      for (size_t i = 0; i < count; i++) {
        preBuf[prePos] = pcmBuf[i];
        prePos = (prePos + 1) % PREROLL_SAMPLES;

        if (preCount < PREROLL_SAMPLES) {
          preCount++;
        }
      }

      if (peak >= MIC_THRESHOLD || rms >= 3000) {
        voice = true;
        voiceStart = lastVoice = millis();

        tarsEmotionSpeechPeak(
          (uint16_t)(peak > 32767 ? 32767 : peak),
          true
        );

        size_t start =
          (preCount == PREROLL_SAMPLES) ? prePos : 0;

        size_t nsend = 0;

        for (size_t i = 0; i < preCount; i++) {
          sendBuf[nsend++] =
            preBuf[(start + i) % PREROLL_SAMPLES];

          if (nsend == 256) {
            if (!sttWS.sendBIN(
                  (uint8_t *)sendBuf,
                  nsend * sizeof(int16_t)
                )) {
              sttError = true;
              break;
            }
            nsend = 0;
          }
        }

        if (nsend && !sttError &&
            !sttWS.sendBIN(
              (uint8_t *)sendBuf,
              nsend * sizeof(int16_t)
            )) {
          sttError = true;
        }

        samples += preCount;

        Serial.printf(
          "TARS: VOICE DETECTED peak=%ld rms=%lu\n",
          (long)peak,
          (unsigned long)rms
        );
      }
    } else {
      if (!sttWS.sendBIN(
            (uint8_t *)pcmBuf,
            count * sizeof(int16_t)
          )) {
        Serial.println("TARS: STT PCM SEND FAILED");
        sttError = true;
        break;
      }

      samples += count;

      if (peak >= MIC_SILENCE || rms >= 1800) {
        lastVoice = millis();
      }

      if (millis() - voiceStart >= RECORD_MIN_MS &&
          millis() - lastVoice >= SILENCE_MS) {
        break;
      }
    }

    yield();
  }

  if (!voice || sttError) {
    closeSTT();
    sttDone = false;
    sttError = false;
    return "";
  }

  return stopSTT(samples);
}
