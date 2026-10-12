#include "vision_live.h"
#include "config.h"
#include "wheels.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <I2SCamera.h>
#include <OV7670.h>
#include <mbedtls/base64.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdlib.h>
#include <string.h>

#ifndef TARS_LIVE_TOKEN
#error "Tambahkan TARS_LIVE_TOKEN di config.h"
#endif

#define VISION_JPEG_MAX 20000
#define VISION_JSON_MAX 28000
#define VISION_RESULT_MAX_AGE 300000UL
#define TRACK_INTERVAL 450UL
#define TRACK_PULSE_MS 70
#define TRACK_SPEED 100
#define TRACK_CENTER_MIN 120
#define TRACK_CENTER_MAX 200
#define TRACK_MAX_AGE 1800UL

extern OV7670 *camera;
extern SemaphoreHandle_t cameraMux;
extern bool cameraLive, cameraOK, playing;
extern bool wifiOK();
extern bool visionLiveEnabled();
extern void stopCamera();
extern bool startCamera();

static uint8_t visionJpeg[VISION_JPEG_MAX];
static char *visionJson = nullptr;

static portMUX_TYPE visionLock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool visionBusy = false;
static bool visionPaused = false;
static bool cameraStoppedForVision = false;

static VisionResult lastVision = {false, false, false, 0};
static VisionTrackResult trackResult = {false, false, -1, 0.5f, 0};

static VisionAim aimState = AIM_STOP;
static uint32_t lastTrackRequest = 0;
static uint32_t lastSeenAt = 0;

// Estimasi arah putaran:
// negatif = pernah menoleh kiri; positif = pernah menoleh kanan.
static int32_t turnDebt = 0;

// ------------------------------------------------------------
// UTILITAS
// ------------------------------------------------------------

static String cloudURL() {
    String s = TARS_CLOUD_URL;
    while (s.endsWith("/")) s.remove(s.length() - 1);
    return s;
}

static void freeVisionJSON() {
    if (visionJson) {
        free(visionJson);
        visionJson = nullptr;
    }
}

static bool acquireVision() {
    bool ok = false;
    portENTER_CRITICAL(&visionLock);
    if (!visionBusy) visionBusy = ok = true;
    portEXIT_CRITICAL(&visionLock);
    return ok;
}

static void releaseVision() {
    portENTER_CRITICAL(&visionLock);
    visionBusy = false;
    portEXIT_CRITICAL(&visionLock);
}

static bool acquireVisionWait(uint32_t ms) {
    uint32_t start = millis();
    while (millis() - start < ms) {
        if (acquireVision()) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

static bool captureFrame(size_t &length) {
    length = 0;
    if (!cameraMux) return false;

    if (xSemaphoreTake(cameraMux, pdMS_TO_TICKS(2500)) != pdTRUE)
        return false;

    bool ready = camera && cameraLive && cameraOK && !playing;
    bool ok = ready && I2SCamera::encodeFrameToJPEG(
        visionJpeg, &length, 45
    );

    xSemaphoreGive(cameraMux);

    if (!ok || !length || length > VISION_JPEG_MAX) {
        length = 0;
        return false;
    }
    return true;
}

static bool stopCameraForTLS() {
    cameraStoppedForVision = true;
    stopCamera();
    return !camera && !cameraLive && !cameraOK;
}

static void restoreCamera() {
    if (cameraStoppedForVision &&
        visionLiveEnabled() &&
        WiFi.status() == WL_CONNECTED &&
        !playing) {
        startCamera();
    }
    cameraStoppedForVision = false;
}

static bool buildJSON(
    const char *prefixText,
    size_t jpegLength,
    size_t &bodyLength
) {
    bodyLength = 0;
    if (!visionJson || jpegLength > VISION_JPEG_MAX) return false;

    int prefix = snprintf(
        visionJson, VISION_JSON_MAX, "%s", prefixText
    );
    if (prefix <= 0 || (size_t)prefix >= VISION_JSON_MAX)
        return false;

    size_t encLen = 0;
    int rc = mbedtls_base64_encode(
        (unsigned char *)visionJson + prefix,
        VISION_JSON_MAX - (size_t)prefix - 4,
        &encLen, visionJpeg, jpegLength
    );
    if (rc != 0) return false;

    size_t end = (size_t)prefix + encLen;
    if (end + 3 >= VISION_JSON_MAX) return false;

    visionJson[end++] = '"';
    visionJson[end++] = '}';
    visionJson[end] = '\0';
    bodyLength = end;
    return true;
}

// Membuat request, mematikan kamera selama TLS untuk menghemat RAM.
static String postVision(
    const String &prefixText,
    size_t jpegLength
) {
    if (WiFi.status() != WL_CONNECTED) return "";

    visionJson = (char *)malloc(VISION_JSON_MAX);
    if (!visionJson) return "";

    size_t bodyLength = 0;
    bool built = buildJSON(prefixText.c_str(), jpegLength, bodyLength);
    if (!built) {
        freeVisionJSON();
        return "";
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(12000);

    HTTPClient http;
    http.setTimeout(15000);

    String response;
    if (http.begin(client, cloudURL() + "/vision")) {
        http.addHeader("Content-Type", "application/json");
        http.addHeader(
            "Authorization", String("Bearer ") + TARS_LIVE_TOKEN
        );

        int code = http.POST((uint8_t *)visionJson, bodyLength);
        if (code >= 200 && code < 300)
            response = http.getString();

        Serial.printf("TARS VISION HTTP=%d\n", code);
        http.end();
    }

    freeVisionJSON();
    return response;
}

// ------------------------------------------------------------
// CACHE JAWABAN VISION TEKS LAMA
// ------------------------------------------------------------

static void storeVisionResult(const String &answer) {
    String s = answer;
    s.toLowerCase();

    bool negative =
        s.indexOf("tidak ada orang") >= 0 ||
        s.indexOf("tidak ada manusia") >= 0 ||
        s.indexOf("tidak terlihat orang") >= 0 ||
        s.indexOf("tidak terlihat manusia") >= 0;

    lastVision.valid = true;
    lastVision.human = !negative && (
        s.indexOf("ada orang") >= 0 ||
        s.indexOf("ada manusia") >= 0 ||
        s.indexOf("seseorang") >= 0 ||
        s.indexOf("seorang") >= 0
    );

    lastVision.animal =
        s.indexOf("kucing") >= 0 ||
        s.indexOf("anjing") >= 0 ||
        s.indexOf("burung") >= 0;

    lastVision.timestamp = millis();
}

VisionResult visionLiveGetLastResult() {
    VisionResult r = lastVision;
    if (r.valid &&
        millis() - r.timestamp > VISION_RESULT_MAX_AGE)
        r.valid = false;
    return r;
}

bool visionLiveHasValidResult(uint32_t maxAgeMs) {
    if (!lastVision.valid) return false;
    if (!maxAgeMs) maxAgeMs = VISION_RESULT_MAX_AGE;
    return millis() - lastVision.timestamp <= maxAgeMs;
}

void visionLiveClearLastResult() {
    lastVision = {false, false, false, 0};
}

void visionLiveClearTrack() {
    wheelsStop();
    trackResult = {false, false, -1, 0.5f, 0};
    aimState = AIM_STOP;
    turnDebt = 0;
    lastSeenAt = 0;
}

// ------------------------------------------------------------
// API VISION LAMA
// ------------------------------------------------------------

void visionLiveBegin() {
    visionLiveClearLastResult();
    visionLiveClearTrack();
    Serial.println("TARS: VISION READY");
}

bool visionLivePause() {
    if (!visionLiveEnabled()) return false;
    if (visionPaused) return true;
    if (!acquireVisionWait(15000)) return false;

    visionPaused = true;
    cameraStoppedForVision = false;
    return true;
}

void visionLiveResume() {
    if (!visionPaused || playing) return;
    restoreCamera();
    visionPaused = false;
    releaseVision();
}

String visionLiveAsk(const String &question) {
    if (!visionLiveEnabled() || !visionPaused || !visionBusy ||
        playing || !wifiOK()) return "";

    size_t len = 0;
    if (!captureFrame(len)) return "";
    if (!stopCameraForTLS()) return "";

    String prefix = "{\"question\":";
    JsonDocument qdoc;
    qdoc["q"] = question;

    String escaped;
    serializeJson(qdoc["q"], escaped);
    prefix += escaped;
    prefix += ",\"format\":\"jpeg\",\"image\":\"";

    String response = postVision(prefix, len);
    memset(visionJpeg, 0, len);
    restoreCamera();

    JsonDocument doc;
    if (deserializeJson(doc, response)) return "";

    String answer = doc["response"] | "";
    answer.trim();
    if (answer.length()) storeVisionResult(answer);
    return answer;
}

// ------------------------------------------------------------
// AUTO-AIM TRACKING
// Panggil visionLiveTrack() berkala dari task kamera.
// ------------------------------------------------------------

static void applyAim(VisionAim state) {
    switch (state) {
        case AIM_LEFT:
            wheelsLeft(TRACK_SPEED);
            delay(TRACK_PULSE_MS);
            wheelsStop();
            turnDebt -= TRACK_PULSE_MS;
            break;

        case AIM_RIGHT:
            wheelsRight(TRACK_SPEED);
            delay(TRACK_PULSE_MS);
            wheelsStop();
            turnDebt += TRACK_PULSE_MS;
            break;

        case AIM_RETURN_CENTER:
            if (turnDebt < 0) {
                wheelsRight(TRACK_SPEED);
                delay(TRACK_PULSE_MS);
                turnDebt += TRACK_PULSE_MS;
            } else if (turnDebt > 0) {
                wheelsLeft(TRACK_SPEED);
                delay(TRACK_PULSE_MS);
                turnDebt -= TRACK_PULSE_MS;
            }
            wheelsStop();
            if (turnDebt == 0) aimState = AIM_STOP;
            break;

        default:
            wheelsStop();
            break;
    }
}

VisionAim visionLiveGetAim() {
    return aimState;
}

VisionTrackResult visionLiveTrack() {
    if (!visionLiveEnabled() || playing ||
        WiFi.status() != WL_CONNECTED) {
        wheelsStop();
        aimState = AIM_STOP;
        trackResult.valid = false;
        return trackResult;
    }

    uint32_t now = millis();

    if (now - lastTrackRequest < TRACK_INTERVAL) {
        // Jangan biarkan hasil lama membuat roda terus berputar.
        if (!trackResult.valid ||
            now - trackResult.timestamp > TRACK_MAX_AGE) {
            wheelsStop();
            aimState = AIM_STOP;
        }
        return trackResult;
    }
    lastTrackRequest = now;

    // Jangan mengirim request jika kamera sedang digunakan fitur lain.
    if (visionBusy || visionPaused) {
        wheelsStop();
        aimState = AIM_STOP;
        return trackResult;
    }

    if (!acquireVision()) {
        wheelsStop();
        aimState = AIM_STOP;
        return trackResult;
    }

    size_t jpegLen = 0;
    bool captured = captureFrame(jpegLen);

    if (!captured || playing || !visionLiveEnabled()) {
        wheelsStop();
        aimState = AIM_STOP;
        releaseVision();
        return trackResult;
    }

    if (!stopCameraForTLS()) {
        wheelsStop();
        aimState = AIM_STOP;
        memset(visionJpeg, 0, jpegLen);
        restoreCamera();
        releaseVision();
        return trackResult;
    }

    String response = postVision(
        "{\"mode\":\"track\",\"format\":\"jpeg\",\"image\":\"",
        jpegLen
    );

    memset(visionJpeg, 0, jpegLen);
    restoreCamera();
    releaseVision();

    // Gagal koneksi/JSON = berhenti, bukan memakai koordinat lama.
    JsonDocument doc;
    if (response.isEmpty() || deserializeJson(doc, response)) {
        wheelsStop();
        aimState = AIM_STOP;
        trackResult.valid = false;
        return trackResult;
    }

    bool found = doc["found"] | false;
    int x = doc["targetX"] | -1;

    if (!found || x < 0 || x > 319) {
        trackResult.found = false;

        if (lastSeenAt && millis() - lastSeenAt <= TRACK_MAX_AGE &&
            turnDebt != 0) {
            aimState = AIM_RETURN_CENTER;
            applyAim(aimState);
        } else {
            wheelsStop();
            aimState = AIM_STOP;
            turnDebt = 0;
            trackResult.valid = false;
        }

        return trackResult;
    }

    now = millis();
    lastSeenAt = now;

    trackResult.valid = true;
    trackResult.found = true;
    trackResult.targetX = x;
    trackResult.centerX = x / 319.0f;
    trackResult.timestamp = now;

    if (x < TRACK_CENTER_MIN) {
        aimState = AIM_LEFT;
    } else if (x > TRACK_CENTER_MAX) {
        aimState = AIM_RIGHT;
    } else {
        aimState = AIM_CENTER;
    }

    applyAim(aimState);

    Serial.printf("TARS AUTO-AIM X=%d AIM=%d DEBT=%ld\n",
        x, (int)aimState, (long)turnDebt);

    return trackResult;
}
