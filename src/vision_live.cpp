#include "vision_live.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <OV7670.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "config.h"

#ifndef TARS_LIVE_TOKEN
#error "Tambahkan TARS_LIVE_TOKEN di config.h"
#endif

#define VISION_LIVE_JPEG_MAX  10000
#define VISION_LIVE_INTERVAL  2000
#define VISION_HEAP_MIN       40000
#define VISION_LARGEST_MIN    30000
#define VISION_LOCK_WAIT      15000

extern OV7670 *camera;
extern SemaphoreHandle_t cameraMux;

extern bool cameraLive;
extern bool cameraOK;
extern bool playing;

extern bool wifiOK();
extern bool visionLiveEnabled();
extern void stopCamera();
extern void startCamera();

static uint8_t visionJpeg[VISION_LIVE_JPEG_MAX];
static TaskHandle_t visionTaskHandle = nullptr;

static portMUX_TYPE visionLock =
    portMUX_INITIALIZER_UNLOCKED;

static volatile bool visionBusy = false;
static volatile bool visionPaused = false;


// =====================================================
// VISION LOCK
// =====================================================

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

static bool acquireVisionWait(uint32_t timeoutMs) {
    uint32_t started = millis();

    while (millis() - started < timeoutMs) {
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


// =====================================================
// URL
// =====================================================

static String cloudURL() {
    String url = TARS_CLOUD_URL;

    while (url.endsWith("/")) {
        url.remove(url.length() - 1);
    }

    return url;
}


// =====================================================
// HEAP CHECK
// =====================================================

static bool visionHeapReady() {
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t largest = ESP.getMaxAllocHeap();

    Serial.printf(
        "TARS: LIVE HEAP FREE=%u LARGEST=%u\n",
        freeHeap, largest
    );

    if (freeHeap < VISION_HEAP_MIN ||
        largest < VISION_LARGEST_MIN) {
        Serial.println("TARS: LIVE TLS SKIPPED - LOW HEAP");
        return false;
    }

    return true;
}


// =====================================================
// CAPTURE JPEG
// =====================================================

static bool captureFrame(size_t &jpegLength) {
    jpegLength = 0;

    if (!cameraMux) {
        Serial.println("TARS: LIVE CAMERA MUTEX NULL");
        return false;
    }

    if (xSemaphoreTake(
        cameraMux,
        pdMS_TO_TICKS(2000)
    ) != pdTRUE) {
        Serial.println("TARS: LIVE CAMERA MUTEX TIMEOUT");
        return false;
    }

    // Periksa ulang setelah mutex didapat.
    bool ready = camera && cameraLive && cameraOK;

    bool captured = false;

    if (ready) {
        captured = I2SCamera::encodeFrameToJPEG(
            visionJpeg,
            &jpegLength,
            25
        );
    }

    xSemaphoreGive(cameraMux);

    if (!captured ||
        jpegLength == 0 ||
        jpegLength > VISION_LIVE_JPEG_MAX) {
        Serial.println("TARS: LIVE JPEG FAILED");
        jpegLength = 0;
        return false;
    }

    return true;
}


// =====================================================
// UPLOAD FRAME
// =====================================================

static bool uploadFrame(size_t jpegLength) {
    if (!jpegLength ||
        jpegLength > VISION_LIVE_JPEG_MAX ||
        WiFi.status() != WL_CONNECTED ||
        !visionHeapReady()) {
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10000);

    HTTPClient http;

    if (!http.begin(client, cloudURL() + "/live/frame")) {
        Serial.println("TARS: LIVE FRAME BEGIN FAILED");
        return false;
    }

    http.setTimeout(12000);
    http.addHeader("Content-Type", "image/jpeg");
    http.addHeader(
        "Authorization",
        String("Bearer ") + TARS_LIVE_TOKEN
    );

    int code = http.POST(visionJpeg, jpegLength);

    Serial.printf(
        "TARS: LIVE FRAME HTTP=%d SIZE=%u\n",
        code, (unsigned)jpegLength
    );

    bool ok = code >= 200 && code < 300;

    if (!ok) {
        Serial.println(
            "TARS: LIVE FRAME ERROR " + http.getString()
        );
    }

    http.end();

    Serial.printf(
        "TARS: LIVE UPLOAD DONE HEAP=%u LARGEST=%u\n",
        ESP.getFreeHeap(),
        ESP.getMaxAllocHeap()
    );

    return ok;
}


// =====================================================
// CAPTURE + UPLOAD
// =====================================================

static bool captureAndUpload() {
    size_t jpegLength = 0;

    if (!captureFrame(jpegLength)) return false;

    return uploadFrame(jpegLength);
}


// =====================================================
// ANALYZE LATEST FRAME FROM CLOUD KV
// =====================================================

static String analyzeLatest(const String &question) {
    if (WiFi.status() != WL_CONNECTED ||
        !visionHeapReady()) {
        return "";
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);

    HTTPClient http;

    if (!http.begin(client, cloudURL() + "/live/analyze")) {
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
        Serial.println(
            "TARS: LIVE ANALYZE ERROR " + http.getString()
        );
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

    return answer;
}


// =====================================================
// PERIODIC LIVE VISION TASK
// =====================================================

static void visionLiveTask(void *) {
    Serial.println("TARS: LIVE UPLOAD TASK START");

    TickType_t lastWake = xTaskGetTickCount();

    for (;;) {
        if (!visionPaused &&
            visionLiveEnabled() &&
            WiFi.status() == WL_CONNECTED &&
            cameraLive &&
            cameraOK &&
            !playing &&
            acquireVision()) {

            if (!visionPaused &&
                visionLiveEnabled() &&
                cameraLive &&
                cameraOK &&
                !playing) {
                captureAndUpload();
            }

            releaseVision();
        }

        vTaskDelayUntil(
            &lastWake,
            pdMS_TO_TICKS(VISION_LIVE_INTERVAL)
        );
    }
}


// =====================================================
// START LIVE TASK
// =====================================================

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


// =====================================================
// PAUSE LIVE + STOP CAMERA
// =====================================================

bool visionLivePause() {
    if (!visionLiveEnabled()) return false;

    if (visionPaused) return true;

    // Tunggu upload yang sedang berjalan selesai.
    if (!acquireVisionWait(VISION_LOCK_WAIT)) {
        Serial.println("TARS: LIVE PAUSE LOCK TIMEOUT");
        return false;
    }

    // Lock tetap ditahan sampai visionLiveResume().
    visionPaused = true;

    Serial.println("TARS: LIVE PAUSE - STOP CAMERA");

    stopCamera();

    Serial.printf(
        "TARS: LIVE PAUSED HEAP=%u LARGEST=%u\n",
        ESP.getFreeHeap(),
        ESP.getMaxAllocHeap()
    );

    return true;
}


// =====================================================
// RESUME LIVE + RESTART CAMERA
// =====================================================

void visionLiveResume() {
    if (!visionPaused) return;

    Serial.println("TARS: LIVE RESUME");

    if (visionLiveEnabled() &&
        WiFi.status() == WL_CONNECTED) {
        startCamera();
    }

    visionPaused = false;
    releaseVision();

    Serial.println("TARS: LIVE UPLOAD RESUMED");
}


// =====================================================
// ANALYZE LATEST FRAME
// Kamera harus sudah dipause oleh main.cpp.
// =====================================================

String visionLiveAsk(const String &question) {
    if (!visionLiveEnabled()) return "";

    if (!visionPaused || !visionBusy) {
        Serial.println("TARS: LIVE ASK REQUIRES PAUSE");
        return "";
    }

    if (WiFi.status() != WL_CONNECTED && !wifiOK()) {
        Serial.println("TARS: LIVE ASK WIFI ERROR");
        return "";
    }

    Serial.printf(
        "TARS: VISION TLS HEAP=%u LARGEST=%u\n",
        ESP.getFreeHeap(),
        ESP.getMaxAllocHeap()
    );

    // Tidak capture ulang. Analisis frame terakhir di KV.
    String answer = analyzeLatest(question);

    Serial.printf(
        "TARS: VISION TLS DONE HEAP=%u LARGEST=%u\n",
        ESP.getFreeHeap(),
        ESP.getMaxAllocHeap()
    );

    return answer;
}
