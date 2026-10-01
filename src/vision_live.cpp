#include "vision_live.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <OV7670.h>
#include "config.h"

#ifndef TARS_LIVE_TOKEN
#error "Tambahkan TARS_LIVE_TOKEN di config.h"
#endif

#define VISION_LIVE_JPEG_MAX 20000
#define VISION_LIVE_INTERVAL 2000

extern OV7670 *camera;
extern SemaphoreHandle_t cameraMux;

extern bool cameraLive;
extern bool cameraOK;
extern bool playing;
extern bool sttConnected;

extern bool wifiOK();
extern bool visionLiveEnabled();

static uint8_t visionJpeg[VISION_LIVE_JPEG_MAX];
static TaskHandle_t visionTaskHandle = nullptr;
static portMUX_TYPE visionLock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool visionBusy = false;

static bool acquireVision() {
    bool ok = false;
    portENTER_CRITICAL(&visionLock);
    if (!visionBusy) {
        visionBusy = true;
        ok = true;
    }
    portEXIT_CRITICAL(&visionLock);
    return ok;
}

static void releaseVision() {
    portENTER_CRITICAL(&visionLock);
    visionBusy = false;
    portEXIT_CRITICAL(&visionLock);
}

static String cloudURL() {
    String url = TARS_CLOUD_URL;
    while (url.endsWith("/")) {
        url.remove(url.length() - 1);
    }
    return url;
}

static bool captureAndUpload() {
    if (!cameraLive || !camera || !cameraOK) {
        Serial.println("TARS: LIVE CAMERA NOT READY");
        return false;
    }

    size_t jpegLength = 0;

    if (cameraMux) {
        if (xSemaphoreTake(cameraMux, pdMS_TO_TICKS(2000)) != pdTRUE) {
            Serial.println("TARS: LIVE CAMERA MUTEX TIMEOUT");
            return false;
        }
    }

    bool captured = I2SCamera::encodeFrameToJPEG(
        visionJpeg, &jpegLength, 25
    );

    if (cameraMux) xSemaphoreGive(cameraMux);

    if (!captured || jpegLength == 0 ||
        jpegLength > VISION_LIVE_JPEG_MAX) {
        Serial.printf(
            "TARS: LIVE JPEG FAILED size=%u\n",
            (unsigned)jpegLength
        );
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);

    HTTPClient http;
    String url = cloudURL() + "/live/frame";

    if (!http.begin(client, url)) {
        Serial.println("TARS: LIVE FRAME BEGIN FAILED");
        return false;
    }

    http.setTimeout(20000);
    http.addHeader("Content-Type", "image/jpeg");
    http.addHeader(
        "Authorization",
        String("Bearer ") + TARS_LIVE_TOKEN
    );

    int code = http.POST(visionJpeg, jpegLength);

    Serial.printf(
        "TARS: LIVE FRAME HTTP=%d JPEG=%u\n",
        code, (unsigned)jpegLength
    );

    if (code < 200 || code >= 300) {
        String err = http.getString();
        Serial.println("TARS: LIVE FRAME ERROR " + err);
        http.end();
        return false;
    }

    http.end();
    return true;
}

static String analyzeLatest(const String &question) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20000);

    HTTPClient http;
    String url = cloudURL() + "/live/analyze";

    if (!http.begin(client, url)) {
        Serial.println("TARS: LIVE ANALYZE BEGIN FAILED");
        return "";
    }

    http.setTimeout(30000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader(
        "Authorization",
        String("Bearer ") + TARS_LIVE_TOKEN
    );

    JsonDocument requestDoc;
    requestDoc["question"] = question;

    String body;
    serializeJson(requestDoc, body);

    int code = http.POST(body);

    Serial.printf("TARS: LIVE ANALYZE HTTP=%d\n", code);

    if (code < 200 || code >= 300) {
        String err = http.getString();
        Serial.println("TARS: LIVE ANALYZE ERROR " + err);
        http.end();
        return "";
    }

    String response = http.getString();
    http.end();

    JsonDocument result;
    if (deserializeJson(result, response)) {
        Serial.println("TARS: LIVE ANALYZE JSON ERROR");
        return "";
    }

    String answer = result["response"].as<String>();
    answer.trim();

    Serial.printf(
        "TARS: LIVE ANALYZE ANSWER=%u CHARS\n",
        (unsigned)answer.length()
    );

    return answer;
}

static void visionLiveTask(void *) {
    Serial.println("TARS: LIVE UPLOAD TASK START");

    for (;;) {
        if (visionLiveEnabled() &&
            WiFi.status() == WL_CONNECTED &&
            cameraLive && cameraOK &&
            !playing && !sttConnected &&
            acquireVision()) {

            captureAndUpload();
            releaseVision();

            vTaskDelay(pdMS_TO_TICKS(VISION_LIVE_INTERVAL));
        } else {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
}

void visionLiveBegin() {
    if (visionTaskHandle) return;

    BaseType_t result = xTaskCreatePinnedToCore(
        visionLiveTask,
        "TARS_VISION",
        8192,
        nullptr,
        1,
        &visionTaskHandle,
        1
    );

    if (result != pdPASS) {
        visionTaskHandle = nullptr;
        Serial.println("TARS: LIVE TASK CREATE FAILED");
    } else {
        Serial.println("TARS: LIVE TASK READY");
    }
}

String visionLiveAsk(const String &question) {
    if (WiFi.status() != WL_CONNECTED || !wifiOK()) {
        Serial.println("TARS: LIVE ASK WIFI ERROR");
        return "";
    }

    if (!acquireVision()) {
        Serial.println("TARS: LIVE BUSY");
        return "";
    }

    Serial.println("TARS: LIVE ASK CAPTURE FRESH FRAME");

    bool uploaded = captureAndUpload();
    String answer;

    if (uploaded) {
        answer = analyzeLatest(question);
    }

    releaseVision();

    return answer;
}
