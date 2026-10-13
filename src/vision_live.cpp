#include "vision_live.h"

#include <Arduino.h>
#include <I2SCamera.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <OV7670.h>
#include <mbedtls/base64.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#ifndef TARS_LIVE_TOKEN
#error "Tambahkan TARS_LIVE_TOKEN di config.h"
#endif

#define VISION_JPEG_MAX 20000
#define VISION_JSON_MAX 28000
#define VISION_LOCK_WAIT 15000

extern OV7670 *camera;
extern SemaphoreHandle_t cameraMux;
extern bool cameraLive;
extern bool cameraOK;
extern bool playing;

extern bool wifiOK();
extern bool visionLiveEnabled();
extern void stopCamera();
extern void startCamera();

static uint8_t visionJpeg[VISION_JPEG_MAX];
static char *visionJson = nullptr;
static char questionJson[1024];

enum VisionOwner : uint8_t {
    VISION_OWNER_NONE = 0,
    VISION_OWNER_USER,
    VISION_OWNER_TRACK
};

static portMUX_TYPE visionLock = portMUX_INITIALIZER_UNLOCKED;
static volatile VisionOwner visionOwner = VISION_OWNER_NONE;
static volatile bool visionUserWaiting = false;
extern volatile bool cameraTransition;
static bool cameraStoppedForVision = false;
static size_t sharedJpegLength = 0;

static void freeVisionJSON() {
    if (visionJson) {
        free(visionJson);
        visionJson = nullptr;
    }
}

static String cloudURL() {
    String url = TARS_CLOUD_URL;

    while (url.endsWith("/")) {
        url.remove(url.length() - 1);
    }

    return url;
}

static bool ownerIs(VisionOwner owner) {
    bool result;

    portENTER_CRITICAL(&visionLock);
    result = visionOwner == owner;
    portEXIT_CRITICAL(&visionLock);

    return result;
}

static bool acquireUserVision() {
    portENTER_CRITICAL(&visionLock);
    visionUserWaiting = true;
    portEXIT_CRITICAL(&visionLock);

    uint32_t started = millis();

    while (millis() - started < VISION_LOCK_WAIT) {
        bool acquired = false;

        portENTER_CRITICAL(&visionLock);

        if (visionOwner == VISION_OWNER_NONE) {
            visionOwner = VISION_OWNER_USER;
            visionUserWaiting = false;
            acquired = true;
        } else if (visionOwner == VISION_OWNER_USER) {
            visionUserWaiting = false;
            acquired = true;
        }

        portEXIT_CRITICAL(&visionLock);

        if (acquired) {
            cameraStoppedForVision = false;
            sharedJpegLength = 0;
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    portENTER_CRITICAL(&visionLock);
    visionUserWaiting = false;
    portEXIT_CRITICAL(&visionLock);

    return false;
}

static bool acquireTrackVision() {
    bool acquired = false;

    portENTER_CRITICAL(&visionLock);

    if (visionOwner == VISION_OWNER_NONE &&
        !visionUserWaiting) {
        visionOwner = VISION_OWNER_TRACK;
        acquired = true;
    }

    portEXIT_CRITICAL(&visionLock);

    if (acquired) {
        cameraStoppedForVision = false;
        sharedJpegLength = 0;
    }

    return acquired;
}

static void releaseVision(VisionOwner owner) {
    portENTER_CRITICAL(&visionLock);

    if (visionOwner == owner) {
        visionOwner = VISION_OWNER_NONE;
    }

    portEXIT_CRITICAL(&visionLock);
}
static bool captureFrame(size_t &length) {
    length = 0;
    if (cameraTransition || playing || !cameraMux) {
        return false;
    }
    if (xSemaphoreTake(
            cameraMux,
            pdMS_TO_TICKS(3000)) != pdTRUE) {
        if (!cameraTransition) {
            Serial.println("TARS: VISION CAMERA LOCK FAILED");
        }
        return false;
    }
    // Periksa ulang setelah memperoleh kunci kamera.
    bool ready =
        !cameraTransition &&
        !playing &&
        camera != nullptr &&
        cameraLive &&
        cameraOK;
    bool ok = false;
    if (ready) {
        ok = I2SCamera::encodeFrameToJPEG(
            visionJpeg,
            &length,
            55
        );
    }
    xSemaphoreGive(cameraMux);
    // Siklus kamera mungkin berubah ketika capture berlangsung.
    if (cameraTransition ||
        !ok ||
        length == 0 ||
        length > VISION_JPEG_MAX) {
        if (length > 0 && length <= VISION_JPEG_MAX) {
            memset(visionJpeg, 0, length);
        }
        length = 0;
        if (!cameraTransition) {Serial.println("TARS: VISION JPEG FAILED");
        }
        return false;
    }
    Serial.printf(
        "TARS: VISION JPEG READY SIZE=%u\n",
        (unsigned)length
    );
    return true;
}
static bool stopCameraForTLS() {
    stopCamera();

    cameraStoppedForVision =
        !camera && !cameraLive && !cameraOK;

    Serial.printf(
        "TARS: VISION CAMERA OFF=%d HEAP=%u LARGEST=%u\n",
        cameraStoppedForVision,
        ESP.getFreeHeap(),
        ESP.getMaxAllocHeap()
    );

    return cameraStoppedForVision;
}
static bool prepareSharedTrackFrame(size_t &length) {
    length = 0;
    if (!ownerIs(VISION_OWNER_TRACK) ||
        playing ||
        !visionLiveEnabled()) {
        return false;
    }
    // Ambil JPEG dengan cameraMux.
    // Kamera tetap menyala selama dan setelah capture.
    if (!captureFrame(length)) {
        return false;
    }
    sharedJpegLength = length;
    Serial.printf(
        "TARS TRACK: JPEG READY SIZE=%u CAMERA KEPT ON\n",
        (unsigned)length
    );
    return true;
}
static bool buildVisionJSON(
    const String &question,
    size_t jpegLength,
    size_t &bodyLength
) {
    bodyLength = 0;

    if (!visionJson || jpegLength > VISION_JPEG_MAX) {
        return false;
    }

    JsonDocument questionDoc;
    questionDoc["question"] = question;

    size_t qlen = serializeJson(
        questionDoc["question"],
        questionJson,
        sizeof(questionJson)
    );

    if (!qlen || qlen >= sizeof(questionJson)) {
        return false;
    }

    int prefix = snprintf(
        visionJson,
        VISION_JSON_MAX,
        "{\"question\":%s,\"format\":\"jpeg\",\"image\":\"",
        questionJson
    );

    if (prefix <= 0 ||
        (size_t)prefix >= VISION_JSON_MAX) {
        return false;
    }

    size_t encodedLength = 0;

    int result = mbedtls_base64_encode(
        (unsigned char *)visionJson + prefix,
        VISION_JSON_MAX - prefix,
        &encodedLength,
        visionJpeg,
        jpegLength
    );

    if (result != 0) {
        Serial.printf(
            "TARS: VISION BASE64 ERROR=%d\n",
            result
        );
        return false;
    }

    size_t end = (size_t)prefix + encodedLength;

    if (end + 3 > VISION_JSON_MAX) {
        return false;
    }

    visionJson[end++] = '"';
    visionJson[end++] = '}';
    visionJson[end] = '\0';

    bodyLength = end;

    return true;
}

static String sendVisionJSON(
    const String &question,
    size_t jpegLength
) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("TARS: VISION WIFI DISCONNECTED");
        return "";
    }

    Serial.printf(
        "TARS: VISION TLS HEAP=%u LARGEST=%u\n",
        ESP.getFreeHeap(),
        ESP.getMaxAllocHeap()
    );

    visionJson = (char *)malloc(VISION_JSON_MAX);

    if (!visionJson) {
        Serial.printf(
            "TARS: VISION JSON MALLOC FAILED HEAP=%u LARGEST=%u\n",
            ESP.getFreeHeap(),
            ESP.getMaxAllocHeap()
        );
        return "";
    }

    size_t bodyLength = 0;

    if (!buildVisionJSON(question, jpegLength, bodyLength)) {
        Serial.println("TARS: VISION JSON BUILD FAILED");
        freeVisionJSON();
        return "";
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20000);

    HTTPClient http;

    if (!http.begin(client, cloudURL() + "/vision")) {
        Serial.println("TARS: VISION HTTP BEGIN FAILED");
        freeVisionJSON();
        client.stop();
        return "";
    }

    http.setTimeout(45000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader(
        "Authorization",
        String("Bearer ") + TARS_LIVE_TOKEN
    );

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

        response = "";
    } else if (code >= 0) {
        Serial.println(
            "TARS: VISION HTTP ERROR " + http.getString()
        );
    }

    http.end();
    client.stop();

    freeVisionJSON();

    Serial.printf(
        "TARS: VISION TLS DONE HEAP=%u LARGEST=%u\n",
        ESP.getFreeHeap(),
        ESP.getMaxAllocHeap()
    );

    return answer;
}

void visionLiveBegin() {
    Serial.println("TARS: ON-DEMAND VISION READY");
}

bool visionLivePause() {
    if (!visionLiveEnabled()) {
        return false;
    }

    if (ownerIs(VISION_OWNER_USER)) {
        return true;
    }

    if (!acquireUserVision()) {
        Serial.println("TARS: VISION LOCK TIMEOUT");
        return false;
    }

    Serial.println("TARS: VISION USER PRIORITY RESERVED");
    return true;
}

void visionLiveResume() {
    if (!ownerIs(VISION_OWNER_USER)) {
        return;
    }
    if (playing || cameraTransition) {
        Serial.println(
            "TARS: VISION RESUME DEFERRED"
        );
        return;
    }
    if (cameraStoppedForVision &&
        visionLiveEnabled() &&
        WiFi.status() == WL_CONNECTED) {
        startCamera();
            Serial.println(
                "TARS: VISION CAMERA RESTART FAILED"
            );
            return;
        }
        if (!camera ||
            !cameraOK ||
            !cameraLive) {
            Serial.println(
                "TARS: VISION CAMERA NOT READY"
            );
            return;
        }
    }
    if (sharedJpegLength > 0) {
        memset(visionJpeg, 0, sharedJpegLength);
    }
    sharedJpegLength = 0;
    cameraStoppedForVision = false;
    releaseVision(VISION_OWNER_USER);
    Serial.printf(
        "TARS: VISION CYCLE END HEAP=%u LARGEST=%u\n",
        ESP.getFreeHeap(),
        ESP.getMaxAllocHeap()
    );
}
bool visionLiveTrackPause() {
    if (!visionLiveEnabled() ||
        playing ||
        WiFi.status() != WL_CONNECTED) {
        return false;
    }

    return acquireTrackVision();
}

bool visionLiveTrackCapture(size_t &length) {
    return prepareSharedTrackFrame(length);
}
uint8_t *visionLiveSharedJpeg() {
    if (!ownerIs(VISION_OWNER_TRACK) ||
        sharedJpegLength == 0) {
        return nullptr;
    }
    return visionJpeg;
}
void visionLiveTrackResume() {
    if (!ownerIs(VISION_OWNER_TRACK)) {
        return;
    }
    if (sharedJpegLength > 0) {
        memset(visionJpeg, 0, sharedJpegLength);
    }
    sharedJpegLength = 0;
    // Kamera hanya dinyalakan kembali jika memang
    // sebelumnya dihentikan oleh jalur Vision ini.
    if (cameraStoppedForVision &&
        visionLiveEnabled() &&
        WiFi.status() == WL_CONNECTED) {
        startCamera();
    }
    cameraStoppedForVision = false;
    releaseVision(VISION_OWNER_TRACK);
}
String visionLiveAsk(const String &question) {
    if (!visionLiveEnabled() ||
        !ownerIs(VISION_OWNER_USER)) {
        return "";
    }

    if (WiFi.status() != WL_CONNECTED && !wifiOK()) {
        Serial.println("TARS: VISION WIFI ERROR");
        return "";
    }

    if (playing) {
        Serial.println(
            "TARS: VISION BLOCKED - AUDIO ACTIVE"
        );
        return "";
    }

    size_t jpegLength = 0;

    if (!captureFrame(jpegLength)) {
        return "";
    }

    sharedJpegLength = jpegLength;

    if (!stopCameraForTLS()) {
        Serial.println("TARS: VISION CAMERA STOP FAILED");
        memset(visionJpeg, 0, jpegLength);
        sharedJpegLength = 0;
        return "";
    }

    String answer = sendVisionJSON(question, jpegLength);

    memset(visionJpeg, 0, jpegLength);
    sharedJpegLength = 0;

    return answer;
}
