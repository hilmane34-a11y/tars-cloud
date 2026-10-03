
#include "vision_live.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <OV7670.h>
#include <mbedtls/base64.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"

#ifndef TARS_LIVE_TOKEN
#error "Tambahkan TARS_LIVE_TOKEN di config.h"
#endif

#define VISION_JPEG_MAX 10000
#define VISION_JSON_MAX 14500
#define VISION_LOCK_WAIT 15000

extern OV7670 *camera;
extern SemaphoreHandle_t cameraMux;
extern bool cameraLive, cameraOK, playing;
extern bool wifiOK();
extern bool visionLiveEnabled();
extern void stopCamera();
extern void startCamera();

static uint8_t visionJpeg[VISION_JPEG_MAX];
static char visionJson[VISION_JSON_MAX];
static char questionJson[1024];

static portMUX_TYPE visionLock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool visionBusy = false;
static bool visionPaused = false;
static bool cameraStoppedForVision = false;

static bool acquireVision() {
    bool ok = false;
    portENTER_CRITICAL(&visionLock);
    if (!visionBusy) visionBusy = ok = true;
    portEXIT_CRITICAL(&visionLock);
    return ok;
}

static bool acquireVisionWait(uint32_t timeoutMs) {
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        if (acquireVision()) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

static void releaseVision() {
    portENTER_CRITICAL(&visionLock);
    visionBusy = false;
    portEXIT_CRITICAL(&visionLock);
}

static String cloudURL() {
    String url = TARS_CLOUD_URL;
    while (url.endsWith("/")) url.remove(url.length() - 1);
    return url;
}

// CAPTURE SATU FRAME SEBELUM KAMERA DIMATIKAN
static bool captureFrame(size_t &length) {
    length = 0;

    if (!cameraMux) return false;

    if (xSemaphoreTake(cameraMux, pdMS_TO_TICKS(3000)) != pdTRUE) {
        Serial.println("TARS: VISION CAMERA LOCK FAILED");
        return false;
    }

    bool ready = camera && cameraLive && cameraOK && !playing;
    bool ok = false;

    if (ready)
        ok = I2SCamera::encodeFrameToJPEG(
            visionJpeg, &length, 25
        );

    xSemaphoreGive(cameraMux);

    if (!ok || !length || length > VISION_JPEG_MAX) {
        length = 0;
        Serial.println("TARS: VISION JPEG FAILED");
        return false;
    }

    Serial.printf("TARS: VISION JPEG READY SIZE=%u\n",
                  (unsigned)length);
    return true;
}

// KAMERA OFF SETELAH JPEG BERHASIL
static bool stopCameraForTLS(){
    cameraStoppedForVision=true;
    stopCamera();

    bool stopped=!camera && !cameraLive && !cameraOK;

    Serial.printf("TARS: VISION CAMERA OFF=%d HEAP=%u LARGEST=%u\n",
        stopped,ESP.getFreeHeap(),ESP.getMaxAllocHeap());

    return stopped;
}
// JSON + BASE64 JPEG
static bool buildVisionJSON(const String &question, size_t jpegLength,
                            size_t &bodyLength) {
    bodyLength = 0;

    JsonDocument questionDoc;
    questionDoc["question"] = question;

    size_t qlen = serializeJson(
        questionDoc["question"],
        questionJson,
        sizeof(questionJson)
    );

    if (!qlen || qlen >= sizeof(questionJson)) return false;

    int prefix = snprintf(
        visionJson, sizeof(visionJson),
        "{\"question\":%s,\"format\":\"jpeg\",\"image\":\"",
        questionJson
    );

    if (prefix <= 0 || (size_t)prefix >= sizeof(visionJson))
        return false;

    size_t encodedLength = 0;
    int result = mbedtls_base64_encode(
        (unsigned char *)visionJson + prefix,
        sizeof(visionJson) - prefix,
        &encodedLength,
        visionJpeg,
        jpegLength
    );

    if (result != 0) {
        Serial.printf("TARS: VISION BASE64 ERROR=%d\n", result);
        return false;
    }

    size_t end = (size_t)prefix + encodedLength;
    if (end + 3 > sizeof(visionJson)) return false;

    visionJson[end++] = '"';
    visionJson[end++] = '}';
    visionJson[end] = '\0';
    bodyLength = end;

    return true;
}

// KIRIM FOTO + PERTANYAAN DALAM SATU JSON
static String sendVisionJSON(const String &question,
                             size_t jpegLength) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("TARS: VISION WIFI DISCONNECTED");
        return "";
    }

    Serial.printf("TARS: VISION TLS HEAP=%u LARGEST=%u\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    size_t bodyLength = 0;
    if (!buildVisionJSON(question, jpegLength, bodyLength)) {
        Serial.println("TARS: VISION JSON BUILD FAILED");
        return "";
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20000);

    HTTPClient http;
    if (!http.begin(client, cloudURL() + "/vision")) {
        Serial.println("TARS: VISION HTTP BEGIN FAILED");
        return "";
    }

    http.setTimeout(45000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization",
                   String("Bearer ") + TARS_LIVE_TOKEN);

    int code = http.POST(
        (uint8_t *)visionJson,
        bodyLength
    );

    Serial.printf("TARS: VISION HTTP=%d\n", code);

    String answer;
    if (code >= 200 && code < 300) {
        String response = http.getString();
        JsonDocument doc;

        if (!deserializeJson(doc, response)) {
            answer = doc["response"].as<String>();
            answer.trim();
        } else {
            Serial.println("TARS: VISION RESPONSE JSON ERROR");
        }
    } else {
        Serial.println("TARS: VISION HTTP ERROR " + http.getString());
    }

    http.end();

    Serial.printf("TARS: VISION TLS DONE HEAP=%u LARGEST=%u\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    return answer;
}

// BEGIN: TIDAK ADA TASK UPLOAD BERKALA
void visionLiveBegin() {
    Serial.println("TARS: ON-DEMAND VISION READY");
}

// PAUSE: KUNCI SIKLUS, BELUM MEMATIKAN KAMERA
bool visionLivePause() {
    if (!visionLiveEnabled()) return false;
    if (visionPaused) return true;

    if (!acquireVisionWait(VISION_LOCK_WAIT)) {
        Serial.println("TARS: VISION LOCK TIMEOUT");
        return false;
    }

    visionPaused = true;
    cameraStoppedForVision = false;

    Serial.println("TARS: VISION CYCLE RESERVED");
    return true;
}

// RESUME: DIPANGGIL SETELAH TTS DAN AUDIO SELESAI
   void visionLiveResume() {
    if (!visionPaused) return;

    if (playing) {
        Serial.println("TARS: VISION RESUME BLOCKED - AUDIO ACTIVE");
        return;
    }
    if (cameraStoppedForVision &&
        visionLiveEnabled() &&
        WiFi.status() == WL_CONNECTED) {
        startCamera();
    }
    cameraStoppedForVision = false;
    visionPaused = false;
    releaseVision();
    Serial.printf("TARS: VISION CYCLE END HEAP=%u LARGEST=%u\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}
// ASK: CAPTURE -> CAMERA OFF -> TLS -> JSON
String visionLiveAsk(const String &question) {
    if (!visionLiveEnabled() || !visionPaused || !visionBusy)
        return "";

    if (WiFi.status() != WL_CONNECTED && !wifiOK()) {
        Serial.println("TARS: VISION WIFI ERROR");
        return "";
    }

    if (playing) {
        Serial.println("TARS: VISION BLOCKED - AUDIO ACTIVE");
        return "";
    }

    size_t jpegLength = 0;

    if (!captureFrame(jpegLength)) return "";

    if (!stopCameraForTLS()) {
        Serial.println("TARS: VISION CAMERA STOP FAILED");
        return "";
    }

    String answer = sendVisionJSON(question, jpegLength);

    memset(visionJpeg, 0, jpegLength);
    memset(visionJson, 0, sizeof(visionJson));

    return answer;
}
