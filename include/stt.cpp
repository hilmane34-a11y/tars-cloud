#include "stt.h"
#include "vision_track.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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
#define MIC_THRESHOLD 6000
#define MIC_SILENCE 4000
#define RECORD_MIN_MS 500
#define SILENCE_MS 800
#define PREROLL_MS 150

#define STT_CONNECT_TIMEOUT_MS 10000
#define STT_READY_TIMEOUT_MS 8000
#define STT_STOP_TIMEOUT_MS 6000

static const size_t BUF = 256;
static const size_t RAW_SAMPLES = BUF / 4;
static const size_t PREROLL_SAMPLES =    MIC_RATE * PREROLL_MS / 1000;

static int32_t rawBuf[RAW_SAMPLES];
static int16_t pcmBuf[RAW_SAMPLES];
static int16_t preBuf[PREROLL_SAMPLES];
static int16_t sendBuf[256];

static WebSocketsClient sttWS;
static bool wsStarted = false;

static uint32_t sttRetryAt = 0;
static bool sttRetryShown = false;
static bool sttStopped = false;

static portMUX_TYPE trackMux =
    portMUX_INITIALIZER_UNLOCKED;

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

String sttFinal;
String sttPartial;

bool micOK = false;

bool sttConnected = false;
bool sttReady = false;
bool sttDone = false;
bool sttError = false;
bool sttClosing = false;
volatile bool sttSessionActive = false;

static const char *STT_HOST_LOCAL =
    "tars-cloud-v1.hilmane34.workers.dev";

extern bool wifiOK();
extern void oledSetStatus(const String &);
extern void oledSetListening();

static bool wsSendTXT(const String &message) {
    if (!sttConnected) return false;

    String payload = message;
    return sttWS.sendTXT(payload);
}
static bool wsSendBIN(
    const uint8_t *data,
    size_t length
) {
    if (!sttConnected || !data || !length) {
        return false;
    }

    return sttWS.sendBIN(
        const_cast<uint8_t *>(data),
        length
    );
}

bool sttCooling() {
    return (int32_t)(millis() - sttRetryAt) < 0;
}

static void failTrackRequest() {
    portENTER_CRITICAL(&trackMux);

    if (trackActive) {
        trackPending = false;
        trackResponseReady = true;
        trackResultValid = false;
    }

    portEXIT_CRITICAL(&trackMux);
}
static void cancelTrackRequest() {
    portENTER_CRITICAL(&trackMux);

    if (trackActive) {
        trackPending = false;
        trackResponseReady = true;
        trackResultValid = false;
    } else {
        trackPending = false;
        trackJpeg = nullptr;
        trackJpegLength = 0;
    }

    portEXIT_CRITICAL(&trackMux);
}
static void serviceTrackRequest() {
    const uint8_t *jpeg = nullptr;
    size_t length = 0;
    uint32_t id = 0;
    bool sendRequest = false;

    if (!sttConnected ||
        sttSessionActive ||
        sttClosing) {
        return;
    }

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

    if (!jpeg || length == 0) {
        failTrackRequest();
        return;
    }

    String control;
    control.reserve(48);
    control = "{\"type\":\"track_begin\",\"id\":";
    control += String(id);
    control += "}";

    // Tidak ada pengiriman PCM di antara kontrol dan JPEG.
    bool controlSent = wsSendTXT(control);
    bool imageSent = false;

    if (controlSent) {
        // JPEG dikirim tepat satu kali.
        imageSent = wsSendBIN(jpeg, length);
    }

    Serial.printf(
        "TARS TRACK: ID=%lu CONTROL=%d JPEG=%d SIZE=%u\n",
        (unsigned long)id,
        controlSent,
        imageSent,
        (unsigned)length
    );

    if (!controlSent || !imageSent) {
        portENTER_CRITICAL(&trackMux);

        if (trackActive && trackCurrentId == id) {
            trackResponseReady = true;
            trackResultValid = false;
        }

        portEXIT_CRITICAL(&trackMux);
        return;
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

    if (!jpeg ||
        length == 0 ||
        !sttConnected ||
        sttSessionActive ||
        sttClosing) {
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
                Serial.println(
                    "TARS TRACK: REQUEST FAILED"
                );
            }

            return valid;
        }

        if (!sttConnected ||
            sttSessionActive ||
            sttClosing) {
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

static void sttEvent(
    WStype_t type,
    uint8_t *payload,
    size_t length
) {
    if (type == WStype_CONNECTED) {
        sttConnected = true;

        Serial.println(
            "TARS: PERSISTENT WSS CONNECTED"
        );

        oledSetStatus("WSS CONNECTED");
        return;
    }

    if (type == WStype_DISCONNECTED) {
        bool wasActive = sttSessionActive;

        sttConnected = false;
        sttReady = false;
        sttSessionActive = false;
        sttStopped = true;

        if (!sttClosing && wasActive) {
            sttError = true;
        }

        failTrackRequest();

        Serial.println(
            sttClosing
                ? "TARS: WSS DISCONNECTED (NORMAL)"
                : "TARS: WSS DISCONNECTED"
        );

        return;
    }

    if (type == WStype_ERROR) {
        if (sttSessionActive && !sttClosing) {
            sttError = true;
        }

        Serial.println("TARS: WSS ERROR");
        return;
    }

    if (type != WStype_TEXT ||
        !payload ||
        !length) {
        return;
    }

    String msg;
    if (!msg.reserve(length + 1)) {
        Serial.println("TARS: STT JSON ALLOC FAILED");
        if (sttSessionActive) sttError = true;
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

    if (strcmp(t, "pong") == 0) {
        return;
    }

    if (strcmp(t, "track_result") == 0) {
        uint32_t id = j["id"] | 0U;
        bool found = j["found"] | false;
        float score = j["score"] | 0.0f;
        float centerX = j["centerX"] | 0.0f;
        int targetX = j["targetX"] | -1;

        if (found &&
            (targetX < 0 ||
             targetX > 319 ||
             score < 0.60f ||
             score > 1.0f)) {
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

    if (strcmp(t, "ready") == 0) {
        sttReady = true;
        sttError = false;
        sttRetryShown = false;

        Serial.println("TARS: STT SESSION READY");
        oledSetStatus("STT READY");
        return;
    }

    if (strcmp(t, "partial") == 0) {
        sttPartial = j["text"] | "";
        sttPartial.trim();

        if (sttPartial.length()) {
            Serial.printf(
                "TARS: STT PARTIAL = %s\n",
                sttPartial.c_str()
            );
        }

        return;
    }

    if (strcmp(t, "final") == 0) {
        sttFinal = j["text"] | "";
        sttFinal.trim();
        sttDone = true;

        Serial.printf(
            "TARS: YOU SAID = %s\n",
            sttFinal.c_str()
        );

        return;
    }

    if (strcmp(t, "stt_stopped") == 0) {
        sttStopped = true;
        sttReady = false;
        sttSessionActive = false;

        Serial.println("TARS: STT SESSION STOPPED");
        return;
    }

    if (strcmp(t, "error") == 0) {
        const char *e = j["error"] | "unknown";

        // DETR yang ditolak karena STT aktif bukan kegagalan STT.
        bool trackBusy =
            strstr(e, "DETR_BUSY") != nullptr ||
            strstr(e, "STT_OR_DETR_BUSY") != nullptr;

        if (trackBusy) {
            Serial.printf(
                "TARS TRACK: BUSY, WILL RETRY (%s)\n",
                e
            );

            failTrackRequest();
            return;
        }

        sttError = true;
        sttDone = true;

        Serial.printf("TARS: STT ERROR = %s\n", e);
        oledSetStatus("STT ERROR");
        failTrackRequest();
        return;
    }
}

void sttService() {
    if (!wsStarted && WiFi.status() == WL_CONNECTED) {
        sttWS.onEvent(sttEvent);
        sttWS.setReconnectInterval(5000);
        sttWS.enableHeartbeat(15000, 5000, 2);

        Serial.println("TARS: START PERSISTENT WSS");

        sttWS.beginSSL(
            STT_HOST_LOCAL,
            443,
            "/stt"
        );

        wsStarted = true;
    }

    if (wsStarted) {
        sttWS.loop();
    }

    serviceTrackRequest();
}

void closeSTT(uint32_t cooldown) {
    sttClosing = true;

    cancelTrackRequest();

    if (sttConnected && sttSessionActive) {
        JsonDocument j;
        j["type"] = "end";

        String message;
        serializeJson(j, message);
        wsSendTXT(message);
    }

    sttSessionActive = false;
    sttReady = false;
    sttStopped = true;

    sttRetryAt = millis() + cooldown;
    sttRetryShown = false;

    // Jangan disconnect WSS di sini.
    sttClosing = false;
}

bool startSTT(bool offline) {
    (void)offline;

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

    if (sttCooling()) {
        if (!sttRetryShown) {
            Serial.println("TARS: STT RETRY COOLDOWN");
            sttRetryShown = true;
        }

        return false;
    }

    uint32_t started = millis();

    while (!sttConnected &&
           millis() - started < STT_CONNECT_TIMEOUT_MS) {
        sttService();
        delay(2);
        yield();

        if (WiFi.status() != WL_CONNECTED) {
            break;
        }
    }

    if (!sttConnected) {
        Serial.println("TARS: WSS CONNECT TIMEOUT");

        sttRetryAt = millis() + STT_ERROR_COOLDOWN;
        sttRetryShown = false;
        return false;
    }

    sttFinal = "";
    sttPartial = "";
    sttDone = false;
    sttError = false;
    sttReady = false;
    sttStopped = false;
    sttSessionActive = false;
    sttRetryShown = false;

    Serial.println("TARS: WSS READY FOR STT/DETR");

    return true;
}

String stopSTT(uint32_t samples, bool offline) {
    (void)offline;

    if (!sttConnected || !sttSessionActive) {
        return sttFinal;
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

    if (!wsSendTXT(msg)) {
        Serial.println("TARS: STT END SEND FAILED");
        closeSTT(STT_ERROR_COOLDOWN);
        return "";
    }

    uint32_t started = millis();

    while (!sttStopped &&
           !sttError &&
           millis() - started < STT_STOP_TIMEOUT_MS) {
        sttService();
        delay(2);
        yield();
    }

    String result = sttFinal;

    sttSessionActive = false;
    sttReady = false;

    if (sttError) {
        sttRetryAt = millis() + STT_ERROR_COOLDOWN;
    } else {
        sttRetryAt = millis() + STT_NORMAL_COOLDOWN;
    }

    sttRetryShown = false;

    return result;
}

static bool sendPCM(
    const int16_t *data,
    size_t count
) {
    if (!data || count == 0) return true;

    return wsSendBIN(
        reinterpret_cast<const uint8_t *>(data),
        count * sizeof(int16_t)
    );
}

static bool sendSamples(
    const int16_t *data,
    size_t count
) {
    size_t offset = 0;

    while (offset < count) {
        size_t n = count - offset;

        if (n > 256) n = 256;

        if (!sendPCM(data + offset, n)) {
            return false;
        }

        offset += n;
    }

    return true;
}
static bool readMic(
    size_t &count,
    int32_t &peak,
    uint32_t &rms
) {
    size_t bytes = 0;

    if (i2s_read(
            MIC_PORT,
            rawBuf,
            sizeof(rawBuf),
            &bytes,
            pdMS_TO_TICKS(30)
        ) != ESP_OK) {
        count = 0;
        return false;
    }

    count = bytes / sizeof(int32_t);
    peak = 0;

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

    rms = count
        ? (uint32_t)sqrt((double)sum / count)
        : 0;

    return true;
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
        sttService();

        if (sttError) break;

        size_t count = 0;
        int32_t peak = 0;
        uint32_t rms = 0;

        if (!readMic(count, peak, rms) || count == 0) {
            continue;
        }

        if (!voice) {
            if (!offline &&
                millis() - listenStart >=
                    STT_IDLE_TIMEOUT_MS) {
                Serial.println(
                    "TARS: STT IDLE TIMEOUT"
                );

                sttRetryAt =
                    millis() + STT_NORMAL_COOLDOWN;

                return "";
            }

            for (size_t i = 0; i < count; i++) {
                preBuf[prePos] = pcmBuf[i];

                prePos++;
                if (prePos >= PREROLL_SAMPLES) {
                    prePos = 0;
                }

                if (preCount < PREROLL_SAMPLES) {
                    preCount++;
                }
            }

            if (peak >= MIC_THRESHOLD || rms >= 3000) {
                voice = true;
                voiceStart = millis();
                lastVoice = voiceStart;

                sttSessionActive = true;
                sttReady = false;
                sttDone = false;
                sttError = false;
                sttStopped = false;

                JsonDocument startMessage;
                startMessage["type"] = "stt_start";

                String startJSON;
                serializeJson(startMessage, startJSON);

                if (!wsSendTXT(startJSON)) {
                    Serial.println(
                        "TARS: STT START SEND FAILED"
                    );

                    closeSTT(STT_ERROR_COOLDOWN);
                    return "";
                }

                tarsEmotionSpeechPeak(
                    (uint16_t)(
                        peak > 32767 ? 32767 : peak
                    ),
                    true
                );

                Serial.printf(
                    "TARS: %s VOICE PEAK=%ld RMS=%lu\n",
                    offline ? "OFFLINE" : "ONLINE",
                    (long)peak,
                    (unsigned long)rms
                );
               // Tunggu READY tanpa menyimpan audio pending.
                // Audio mikrofon tetap dibaca agar buffer I2S
                // tidak menumpuk selama menunggu Speechmatics.
            uint32_t waitStart = millis();

                while (!sttReady &&
                       !sttError &&
                       millis() - waitStart <
                           STT_READY_TIMEOUT_MS) {
                    sttService();

                    size_t discardCount = 0;
                    int32_t discardPeak = 0;
                    uint32_t discardRms = 0;

                    // Audio selama menunggu READY dibuang.
                    readMic(
                        discardCount,
                        discardPeak,
                        discardRms
                    );

                    if (discardPeak >= MIC_SILENCE ||
                        discardRms >= 1800) {
                        lastVoice = millis();
                    }

                    delay(1);
                    yield();
                }

                if (!sttReady || sttError) {
                    Serial.println(
                        "TARS: STT SESSION READY TIMEOUT"
                    );

                    closeSTT(STT_ERROR_COOLDOWN);
                    return "";
                }

                Serial.println(
                    "TARS: STREAMING PCM REALTIME"
                );

                // Kirim preroll 150 ms dalam urutan waktu.
                size_t preStart =
                    preCount == PREROLL_SAMPLES
                        ? prePos
                        : 0;

                for (size_t i = 0; i < preCount; ) {
                    size_t n = preCount - i;
                    if (n > 256) n = 256;

                    for (size_t k = 0; k < n; k++) {
                        sendBuf[k] =
                            preBuf[
                                (preStart + i + k) %
                                PREROLL_SAMPLES
                            ];
                    }

                    if (!sendPCM(sendBuf, n)) {
                        sttError = true;
                        break;
                    }

                    i += n;
                }

                if (sttError) break;

                samples += preCount;

                // Setelah READY, audio berikutnya langsung
                // dikirim secara realtime di loop perekaman.
                // Lanjutkan rekaman realtime sampai jeda ucapan.
                while (!sttError) {
                    sttService();

                    if (sttError) break;

                    size_t liveCount = 0;
                    int32_t livePeak = 0;
                    uint32_t liveRms = 0;

                    if (!readMic(
                            liveCount,
                            livePeak,
                            liveRms
                        ) || liveCount == 0) {
                        continue;
                    }

                    if (!sendPCM(pcmBuf, liveCount)) {
                        Serial.println(
                            "TARS: STT PCM SEND FAILED"
                        );

                        sttError = true;
                        break;
                    }

                    samples += liveCount;

                    if (livePeak >= MIC_SILENCE ||
                        liveRms >= 1800) {
                        lastVoice = millis();
                    }

                    if (millis() - voiceStart >=
                            RECORD_MIN_MS &&
                        millis() - lastVoice >= SILENCE_MS) {
                        break;
                    }

                    yield();
                }

                break;
            }
        }

        yield();
    }

    if (!voice || sttError) {
        if (sttError) {
            closeSTT(STT_ERROR_COOLDOWN);
        } else {
            sttRetryAt =
                millis() + STT_NORMAL_COOLDOWN;

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
