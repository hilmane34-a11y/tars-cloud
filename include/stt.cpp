#include "stt.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>

#include "config.h"
#include "wifi_manager.h"
#include <tars_emotion.h>

#define MIC_PORT I2S_NUM_1

extern const uint32_t MIC_RATE;
extern const uint32_t RECORD_MIN_MS;
extern const uint32_t SILENCE_MS;
extern const uint32_t PREROLL_MS;
extern const int32_t MIC_THRESHOLD;
extern const int32_t MIC_SILENCE;

extern bool micOK;
extern void oledSetStatus(const String&);
extern void oledSetListening();

extern const char* STT_HOST;

bool sttConnected = false;
bool sttReady = false;
bool sttDone = false;
bool sttError = false;
bool sttClosing = false;

static uint32_t sttRetryAt = 0;
static bool sttRetryShown = false;

static const size_t BUF = 256;
static const size_t PREROLL_SAMPLES = 16000 * 250 / 1000;

static int32_t rawBuf[BUF / 4];
static int16_t pcmBuf[BUF / 4];
static int16_t preBuf[PREROLL_SAMPLES];
static int16_t sendBuf[256];

static WebSocketsClient sttWS;

bool sttCooling() {
  return millis() < sttRetryAt;
}

static void sttEvent(WStype_t type, uint8_t* payload, size_t length) {

  if (type == WStype_CONNECTED) {
    sttConnected = true;
    sttError = false;

    Serial.println("TARS: STT WS CONNECTED");
    oledSetStatus("STT CONNECTED");
    return;
  }

  if (type == WStype_DISCONNECTED) {
    sttConnected = false;

    if (!sttClosing && !sttDone)
      sttError = true;

    Serial.println(
      sttClosing
        ? "TARS: STT WS DISCONNECTED (NORMAL)"
        : "TARS: STT WS DISCONNECTED"
    );

    return;
  }

  if (type == WStype_ERROR) {
    if (!sttClosing)
      sttError = true;

    Serial.println("TARS: STT WS ERROR");
    oledSetStatus("STT ERROR");
    return;
  }

  if (type != WStype_TEXT)
    return;

  String msg;
  msg.reserve(length + 1);

  for (size_t i = 0; i < length; i++)
    msg += (char)payload[i];

  JsonDocument j;

  if (deserializeJson(j, msg))
    return;

  String t = j["type"].as<String>();

  if (t == "ready") {

    sttReady = true;
    sttError = false;
    sttRetryShown = false;

    Serial.println("TARS: STT REALTIME READY");
    oledSetStatus("STT READY");

  } else if (t == "partial") {

    String s = j["text"].as<String>();
    s.trim();

    if (s.length())
      Serial.println("TARS: STT PARTIAL = " + s);

  } else if (t == "final") {

    String s = j["text"].as<String>();
    s.trim();

    sttFinal = s;
    sttDone = true;

    Serial.println("TARS: YOU SAID = " + sttFinal);

  } else if (t == "error") {

    sttError = true;
    sttDone = true;

    String e = j["error"].as<String>();

    Serial.println("TARS: STT ERROR = " + e);
    oledSetStatus("STT ERROR");
  }
}

void closeSTT(uint32_t cooldown) {

  sttClosing = true;

  /*
   * Penting:
   * disconnect() saja.
   * Jangan memanggil sttWS.loop() setelah ini
   * sampai startSTT() benar-benar dimulai lagi.
   */
  sttWS.disconnect();

  sttConnected = false;
  sttReady = false;

  sttRetryAt = millis() + cooldown;
  sttRetryShown = false;

  sttClosing = false;
}

bool startSTT(bool offline) {

  if (!wifiOK() || !micOK)
    return false;

  if ((int32_t)(millis() - sttRetryAt) < 0) {

    if (!sttRetryShown) {
      Serial.println("TARS: STT RETRY COOLDOWN");
      sttRetryShown = true;
    }

    return false;
  }

  /*
   * Bersihkan koneksi lama tanpa memberi kesempatan
   * WebSocket reconnect sendiri.
   */
  sttClosing = true;
  sttWS.disconnect();
  sttClosing = false;

  sttConnected = false;
  sttReady = false;
  sttDone = false;
  sttError = false;

  sttFinal = "";
  sttPartial = "";

  sttRetryShown = false;

  sttWS.onEvent(sttEvent);
  sttWS.setReconnectInterval(60000);
  sttWS.enableHeartbeat(15000, 5000, 2);

  sttWS.beginSSL(STT_HOST, 443, "/stt");

  uint32_t st = millis();

  while (!sttReady &&
         !sttError &&
         millis() - st < 20000) {

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

  if (!sttConnected && !sttDone)
    return "";

  JsonDocument j;

  j["type"] = "end";
  j["timestamp"] = (double)samples / MIC_RATE;

  String msg;
  serializeJson(j, msg);

  if (!sttWS.sendTXT(msg)) {

    Serial.println("TARS: STT END SEND FAILED");

    closeSTT(STT_ERROR_COOLDOWN);
    return "";
  }

  uint32_t st = millis();

  while (!sttDone &&
         !sttError &&
         millis() - st < 6000) {

    sttWS.loop();

    delay(2);
    yield();
  }

  String r = sttFinal;

  /*
   * Tutup koneksi setelah final.
   * Tidak ada reconnect otomatis setelah ini.
   */
  closeSTT(STT_NORMAL_COOLDOWN);

  return r;
}

String recordSTT(bool offline) {

  if (!startSTT(offline))
    return "";

  if (offline)
    oledSetStatus("READY");
  else
    oledSetListening();

  size_t prePos = 0;
  size_t preCount = 0;

  uint32_t voiceStart = 0;
  uint32_t lastVoice = 0;
  uint32_t samples = 0;

  uint32_t listenStart = millis();

  bool voice = false;

  for (;;) {

    sttWS.loop();

    if (sttError)
      break;

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

      if (a > peak)
        peak = a;

      sum += (uint64_t)a * a;
    }

    uint32_t rms =
      count
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

        prePos =
          (prePos + 1) %
          PREROLL_SAMPLES;

        if (preCount < PREROLL_SAMPLES)
          preCount++;
      }

      if (peak >= MIC_THRESHOLD || rms >= 3000) {

        voice = true;

        voiceStart = millis();
        lastVoice = millis();

        tarsEmotionSpeechPeak(
          (uint16_t)(
            peak > 32767
              ? 32767
              : peak
          ),
          true
        );

        size_t start =
          preCount == PREROLL_SAMPLES
            ? prePos
            : 0;

        size_t nsend = 0;

        for (size_t i = 0; i < preCount; i++) {

          sendBuf[nsend++] =
            preBuf[
              (start + i) %
              PREROLL_SAMPLES
            ];

          if (nsend == 256) {

            if (!sttWS.sendBIN(
                  (uint8_t*)sendBuf,
                  nsend * 2
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
              (uint8_t*)sendBuf,
              nsend * 2
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
            (uint8_t*)pcmBuf,
            count * 2
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

      if (peak >= MIC_SILENCE || rms >= 1800)
        lastVoice = millis();

      if (millis() - voiceStart >= RECORD_MIN_MS &&
          millis() - lastVoice >= SILENCE_MS) {

        break;
      }
    }

    yield();
  }

  if (!voice || sttError) {

    closeSTT(
      sttError
        ? STT_ERROR_COOLDOWN
        : STT_NORMAL_COOLDOWN
    );

    if (!voice) {

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
