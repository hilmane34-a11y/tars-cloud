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
#define MIC_SCK 18
#define MIC_WS 2
#define MIC_SD 15
#define MIC_RATE 16000
#define MIC_THRESHOLD 10000
#define MIC_SILENCE 7000
#define RECORD_MIN_MS 500
#define SILENCE_MS 800
#define PREROLL_MS 150

#define STT_NORMAL_COOLDOWN 1000
#define STT_ERROR_COOLDOWN 30000
#define STT_QUOTA_COOLDOWN 60000
#define STT_RECONNECT_GUARD 60000
#define STT_IDLE_TIMEOUT_MS 10000

static const size_t BUF = 256;
static const size_t PREROLL_SAMPLES = MIC_RATE * PREROLL_MS / 1000;

static int32_t rawBuf[BUF / 4];
static int16_t pcmBuf[BUF / 4];
static int16_t preBuf[PREROLL_SAMPLES];
static int16_t sendBuf[256];

static WebSocketsClient sttWS;

static portMUX_TYPE trackMux = portMUX_INITIALIZER_UNLOCKED;

static bool trackPending = false;
static bool trackActive = false;
static bool trackResponseReady = false;
static bool trackResultValid = false;

static const uint8_t *trackJpeg = nullptr;
static size_t trackJpegLength = 0;

static uint32_t trackIdCounter = 0;
static uint32_t trackCurrentId = 0;

static bool trackFound = false;
static float trackScore = 0.0f;
static float trackCenterX = 0.0f;
static int trackTargetX = -1;

static void cancelTrackRequest() {
    portENTER_CRITICAL(&trackMux);

    if (trackActive) {
        trackPending = false;
        trackResponseReady = true;
        trackResultValid = false;
    }

    portEXIT_CRITICAL(&trackMux);
}

static void serviceTrackRequest() {
    const uint8_t *jpeg = nullptr;
    size_t length = 0;
    uint32_t id = 0;
    bool sendRequest = false;

    portENTER_CRITICAL(&trackMux);

    if (trackPending && trackActive) {
        jpeg = trackJpeg;
        length = trackJpegLength;
        id = trackCurrentId;
        trackPending = false;
        sendRequest = true;
    }

    portEXIT_CRITICAL(&trackMux);

    if (!sendRequest) return;

    bool sent = false;

    if (sttConnected && sttReady && !sttClosing &&
        jpeg && length > 0) {

        String control;
        control.reserve(48);
        control = "{\"type\":\"track_begin\",\"id\":";
        control += String(id);
        control += "}";

        // Penting: kirim kontrol lalu JPEG langsung sesudahnya.
        // Jangan menyisipkan frame PCM di antara kedua pengiriman.
        bool controlSent = sttWS.sendTXT(control);

        if (controlSent) {
            sent = sttWS.sendBIN(
                const_cast<uint8_t *>(jpeg),
                length
            );
        }
    }

    if (!sent) {
        portENTER_CRITICAL(&trackMux);

        if (trackActive && trackCurrentId == id) {
            trackResponseReady = true;
            trackResultValid = false;
        }

        portEXIT_CRITICAL(&trackMux);

        Serial.println("TARS TRACK: WSS SEND FAILED");
    } else {
        Serial.printf(
            "TARS TRACK: WSS JPEG SENT ID=%lu SIZE=%u\n",
            (unsigned long)id,
            (unsigned)length
        );
    }
}

bool sttTrackDetect(
    const uint8_t *jpeg,
    size_t length,
    VisionTrackResult &result,
    uint32_t timeoutMs
) {
    result.found = false;
    result.score = 0.0f;
    result.centerX = 0.0f;
    result.targetX = -1;

    if (!jpeg || length == 0 ||
        !sttConnected || !sttReady || sttClosing) {
        return false;
    }

    uint32_t id;

    portENTER_CRITICAL(&trackMux);

    if (trackActive || trackPending) {
        portEXIT_CRITICAL(&trackMux);
        return false;
    }

    id = ++trackIdCounter;
    if (id == 0) id = ++trackIdCounter;

    trackCurrentId = id;
    trackJpeg = jpeg;
    trackJpegLength = length;

    trackFound = false;
    trackScore = 0.0f;
    trackCenterX = 0.0f;
    trackTargetX = -1;

    trackResultValid = false;
    trackResponseReady = false;
    trackActive = true;
    trackPending = true;

    portEXIT_CRITICAL(&trackMux);

    Serial.printf(
        "TARS TRACK: REQUEST ID=%lu SIZE=%u\n",
        (unsigned long)id,
        (unsigned)length
    );

    uint32_t started = millis();

    while (millis() - started < timeoutMs) {
        bool ready = false;
        bool valid = false;

        portENTER_CRITICAL(&trackMux);

        ready = trackResponseReady;

        if (ready && trackCurrentId == id) {
            valid = trackResultValid;

            if (valid) {
                result.found = trackFound;
                result.score = trackScore;
                result.centerX = trackCenterX;
                result.targetX = trackTargetX;
            }

            trackPending = false;
            trackActive = false;
            trackResponseReady = false;
            trackResultValid = false;
            trackJpeg = nullptr;
            trackJpegLength = 0;
        }

        portEXIT_CRITICAL(&trackMux);

        if (ready) {
            if (valid) {
                Serial.printf(
                    "TARS TRACK: RESULT FOUND=%d X=%d SCORE=%.2f\n",
                    result.found,
                    result.targetX,
                    result.score
                );
            } else {
                Serial.println("TARS TRACK: REQUEST FAILED");
            }

            return valid;
        }

        if (!sttConnected || !sttReady || sttClosing) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    portENTER_CRITICAL(&trackMux);

    if (trackCurrentId == id) {
        trackPending = false;
        trackActive = false;
        trackResponseReady = false;
        trackResultValid = false;
        trackJpeg = nullptr;
        trackJpegLength = 0;
    }

    portEXIT_CRITICAL(&trackMux);

    Serial.printf(
        "TARS TRACK: TIMEOUT ID=%lu\n",
        (unsigned long)id
    );

    return false;
}
static uint32_t sttRetryAt = 0;
static bool sttRetryShown = false;

String sttFinal, sttPartial;

bool micOK = false;
bool sttConnected = false;
bool sttReady = false;
bool sttDone = false;
bool sttError = false;
bool sttClosing = false;

static const char *STT_HOST_LOCAL =
  "tars-cloud-v1.hilmane34.workers.dev";

extern bool wifiOK();
extern void oledSetStatus(const String &);
extern void oledSetListening();

bool sttCooling() {
  return (int32_t)(millis() - sttRetryAt) < 0;
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
    oledSetStatus("STT CONNECTED");
    return;
  }

  if (type == WStype_DISCONNECTED) {
    sttConnected = false;

    if (!sttClosing && !sttDone) {
      sttError = true;
    }

    Serial.println(
      sttClosing
        ? "TARS: STT WS DISCONNECTED (NORMAL)"
        : "TARS: STT WS DISCONNECTED"
    );
    return;
  }

  if (type == WStype_ERROR) {
    if (!sttClosing) {
      sttError = true;
    }

    Serial.println("TARS: STT WS ERROR");
    oledSetStatus("STT ERROR");
    return;
  }

  if (type != WStype_TEXT || !payload || !length) {
    return;
  }
String type = doc["type"] | "";

if (type == "track_result") {
    uint32_t id = doc["id"] | 0U;

    bool found = doc["found"] | false;
    float score = doc["score"] | 0.0f;
    float centerX = doc["centerX"] | 0.0f;
    int targetX = doc["targetX"] | -1;

    if (found &&
        (targetX < 0 || targetX > 319 ||
         score < 0.60f || score > 1.0f)) {
        found = false;
        targetX = -1;
    }

    if (centerX < 0.0f || centerX > 1.0f) {
        centerX = 0.0f;
    }

    portENTER_CRITICAL(&trackMux);

    if (trackActive && id == trackCurrentId) {
        trackFound = found;
        trackScore = score;
        trackCenterX = centerX;
        trackTargetX = found ? targetX : -1;

        trackResultValid = true;
        trackResponseReady = true;
    }

    portEXIT_CRITICAL(&trackMux);

    return;
}
  String msg;
  if (!msg.reserve(length + 1)) {
    Serial.println("TARS: STT JSON ALLOC FAILED");
    sttError = true;
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

  const char *t = j["type"] | "";

  if (strcmp(t, "ready") == 0) {
    sttReady = true;
    sttError = false;
    sttRetryShown = false;

    Serial.println("TARS: STT REALTIME READY");
    oledSetStatus("STT READY");
  } else if (strcmp(t, "partial") == 0) {
    sttPartial = j["text"] | "";
    sttPartial.trim();

    if (sttPartial.length()) {
      Serial.printf(
        "TARS: STT PARTIAL = %s\n",
        sttPartial.c_str()
      );
    }
  } else if (strcmp(t, "final") == 0) {
    sttFinal = j["text"] | "";
    sttFinal.trim();

    sttDone = true;

    Serial.printf(
      "TARS: YOU SAID = %s\n",
      sttFinal.c_str()
    );
  } else if (strcmp(t, "error") == 0) {
    sttError = true;
    sttDone = true;

    const char *e = j["error"] | "unknown";

    Serial.printf("TARS: STT ERROR = %s\n", e);
    oledSetStatus("STT ERROR");
  }
}

void closeSTT(uint32_t cooldown) {
  cancelTrackRequest();
  sttClosing = true;

  sttWS.disconnect();

  sttConnected = false;
  sttReady = false;

  sttRetryAt = millis() + cooldown;
  sttRetryShown = false;

  sttClosing = false;
}

bool startSTT(bool offline) {
  if (!wifiOK()) {
    Serial.printf(
      "TARS: STT BLOCK WIFI status=%d\n",
      WiFi.status()
    );
    return false;
  }

  if (!micOK) {
    Serial.println("TARS: STT BLOCK MIC=false");
    return false;
  }

  if ((int32_t)(millis() - sttRetryAt) < 0) {
    if (!sttRetryShown) {
      Serial.println("TARS: STT RETRY COOLDOWN");
      sttRetryShown = true;
    }
    return false;
  }

  Serial.println("TARS: STT PREPARE WSS");

  sttClosing = true;
  sttWS.disconnect();

  sttConnected = false;
  sttReady = false;
  sttClosing = false;

  sttDone = false;
  sttError = false;
  sttFinal = "";
  sttPartial = "";
  sttRetryShown = false;

  sttWS.onEvent(sttEvent);
  sttWS.setReconnectInterval(5000);
  sttWS.enableHeartbeat(15000, 5000, 2);

  Serial.println("TARS: STT BEGIN SSL");

  Serial.printf(
    "TARS: STT DIAG heap=%u maxBlock=%u RSSI=%d\n",
    ESP.getFreeHeap(),
    ESP.getMaxAllocHeap(),
    WiFi.RSSI()
  );

  sttWS.beginSSL(STT_HOST_LOCAL, 443, "/stt");

  Serial.println("TARS: STT SSL STARTED");

  uint32_t st = millis();

  while (!sttReady && !sttError && millis() - st < 20000) {
    sttWS.loop();
    delay(2);
    yield();
  }

  if (!sttReady) {
    Serial.println(
      offline
        ? "TARS: OFFLINE STT CONNECT ERROR"
        : "TARS: STT CONNECT ERROR"
    );

    closeSTT(STT_ERROR_COOLDOWN);
    return false;
  }

  sttRetryAt = millis();
  sttRetryShown = false;

  Serial.println(
    offline
      ? "TARS: OFFLINE STT READY"
      : "TARS: ONLINE STT READY"
  );

  return true;
}

String stopSTT(uint32_t samples, bool offline) {
  if (!sttConnected && !sttDone) {
    return "";
  }

  JsonDocument j;

  j["type"] = "end";
  j["timestamp"] = (double)samples / MIC_RATE;

  String msg;

  if (serializeJson(j, msg) == 0) {
    Serial.println("TARS: STT END JSON FAILED");
    closeSTT(STT_ERROR_COOLDOWN);
    return "";
  }

  if (!sttWS.sendTXT(msg)) {
    Serial.println("TARS: STT END SEND FAILED");
    closeSTT(STT_ERROR_COOLDOWN);
    return "";
  }

  uint32_t st = millis();

  while (!sttDone && !sttError && millis() - st < 6000) {
    sttWS.loop();
    delay(2);
    yield();
  }

  String r = sttFinal;

  if (sttError && !sttDone) {
    closeSTT(STT_ERROR_COOLDOWN);
  } else {
    closeSTT(STT_NORMAL_COOLDOWN);
  }

  return r;
}

String recordSTT(bool offline) {
  if (!startSTT(offline)) {
    return "";
  }

  if (offline) {
    oledSetStatus("READY");
  } else {
    oledSetListening();
  }

  size_t prePos = 0;
  size_t preCount = 0;

  uint32_t voiceStart = 0;
  uint32_t lastVoice = 0;
  uint32_t samples = 0;
  uint32_t listenStart = millis();

  bool voice = false;

  for (;;) {
    serviceTrackRequest();
    sttWS.loop();

    if (sttError) {
      break;
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

    size_t count = bytes / 4;
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

      if (a > peak) {
        peak = a;
      }

      sum += (uint64_t)a * a;
    }

    uint32_t rms = count
      ? (uint32_t)sqrt((double)sum / count)
      : 0;

    if (!voice) {
      if (!offline &&
          millis() - listenStart >= STT_IDLE_TIMEOUT_MS) {
        Serial.println(
          "TARS: STT IDLE TIMEOUT - CLOSE NORMAL"
        );

        closeSTT(STT_NORMAL_COOLDOWN);
        return "";
      }

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

        if (nsend &&
            !sttError &&
            !sttWS.sendBIN(
              (uint8_t *)sendBuf,
              nsend * sizeof(int16_t)
            )) {
          sttError = true;
        }

        samples += preCount;

        Serial.printf(
          "TARS: %s VOICE PEAK=%ld RMS=%lu\n",
          offline ? "OFFLINE" : "ONLINE",
          (long)peak,
          (unsigned long)rms
        );
      }
    } else {
      if (!sttWS.sendBIN(
            (uint8_t *)pcmBuf,
            count * sizeof(int16_t)
          )) {
        Serial.println(
          offline
            ? "TARS: OFFLINE STT PCM SEND FAILED"
            : "TARS: STT PCM SEND FAILED"
        );

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
    if (sttError) {
      closeSTT(STT_ERROR_COOLDOWN);
    } else {
      closeSTT(STT_NORMAL_COOLDOWN);

      Serial.println(
        offline
          ? "TARS: OFFLINE MIC AUDIO TOO LOW"
          : "TARS: MIC AUDIO TOO LOW"
      );
    }

    return "";
  }

  return stopSTT(samples, offline);
}

String recordRealtime() {
  return recordSTT(false);
}

String recordOffline() {
  return recordSTT(true);
}
