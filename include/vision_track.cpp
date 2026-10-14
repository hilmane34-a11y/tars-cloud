#include "vision_track.h"
#include "vision_live.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

#ifndef TARS_LIVE_TOKEN
#error "Tambahkan TARS_LIVE_TOKEN di config.h"
#endif

#define TRACK_TIMEOUT 8000
#define TRACK_JPEG_MAX 20000
#define TRACK_JSON_MAX 28000

extern bool playing;
extern bool visionLiveEnabled();

static char *trackJson = nullptr;

static void clearResult(VisionTrackResult &r) {
    r.found = false;
    r.score = 0.0f;
    r.centerX = 0.0f;
    r.targetX = -1;
}

static String trackURL() {
    String url = TARS_CLOUD_URL;
    while (url.endsWith("/")) {
        url.remove(url.length() - 1);
    }
    return url + "/vision";
}

void visionTrackBegin() {
    Serial.println("TARS TRACK: HTTPS DETR READY");
}

static bool sendTrackHTTPS(
    const uint8_t *jpeg,
    size_t jpegLength,
    VisionTrackResult &result
) {
    if (!jpeg || jpegLength == 0 ||
        jpegLength > TRACK_JPEG_MAX ||
        WiFi.status() != WL_CONNECTED) {
        return false;
    }

    trackJson = (char *)malloc(TRACK_JSON_MAX);
    if (!trackJson) {
        Serial.printf(
            "TARS TRACK: JSON ALLOC FAILED HEAP=%u MAX=%u\n",
            ESP.getFreeHeap(),
            ESP.getMaxAllocHeap()
        );
        return false;
    }

    int prefix = snprintf(
        trackJson, TRACK_JSON_MAX,
        "{\"mode\":\"track\",\"format\":\"jpeg\",\"image\":\""
    );

    if (prefix <= 0 || prefix >= TRACK_JSON_MAX) {
        free(trackJson);
        trackJson = nullptr;
        return false;
    }

    size_t encodedLength = 0;
    int b64 = mbedtls_base64_encode(
        (unsigned char *)trackJson + prefix,
        TRACK_JSON_MAX - (size_t)prefix,
        &encodedLength,
        jpeg,
        jpegLength
    );

    if (b64 != 0) {
        Serial.printf("TARS TRACK: BASE64 ERROR=%d\n", b64);
        free(trackJson);
        trackJson = nullptr;
        return false;
    }

    size_t bodyLength = (size_t)prefix + encodedLength;
    if (bodyLength + 3 > TRACK_JSON_MAX) {
        free(trackJson);
        trackJson = nullptr;
        return false;
    }

    trackJson[bodyLength++] = '"';
    trackJson[bodyLength++] = '}';
    trackJson[bodyLength] = '\0';

    Serial.printf(
        "TARS TRACK: HTTPS POST JPEG=%u BODY=%u HEAP=%u MAX=%u\n",
        (unsigned)jpegLength,
        (unsigned)bodyLength,
        ESP.getFreeHeap(),
        ESP.getMaxAllocHeap()
    );

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(TRACK_TIMEOUT);

    HTTPClient http;
    http.setTimeout(TRACK_TIMEOUT);

    if (!http.begin(client, trackURL())) {
        Serial.println("TARS TRACK: HTTP BEGIN FAILED");
        free(trackJson);
        trackJson = nullptr;
        client.stop();
        return false;
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader(
        "Authorization",
        String("Bearer ") + TARS_LIVE_TOKEN
    );

    int code = http.POST((uint8_t *)trackJson, bodyLength);
    Serial.printf("TARS TRACK: HTTP=%d\n", code);

    bool success = false;

    if (code >= 200 && code < 300) {
        String response = http.getString();
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, response);

        if (!err) {
            result.found = doc["found"] | false;
            result.score = doc["score"] | 0.0f;
            result.centerX = doc["centerX"] | 0.0f;
            result.targetX = doc["targetX"] | -1;

            if (result.found &&
                result.score >= 0.60f &&
                result.targetX >= 0 &&
                result.targetX <= 319) {
                success = true;
            } else {
                clearResult(result);
                success = true;
            }
        } else {
            Serial.println("TARS TRACK: RESPONSE JSON ERROR");
        }
        response = "";
    } else if (code >= 0) {
        Serial.printf(
            "TARS TRACK: HTTP ERROR=%s\n",
            http.getString().c_str()
        );
    }

    http.end();
    client.stop();

    free(trackJson);
    trackJson = nullptr;

    Serial.printf(
        "TARS TRACK: DONE OK=%d FOUND=%d SCORE=%.2f X=%d HEAP=%u MAX=%u\n",
        success,
        result.found,
        result.score,
        result.targetX,
        ESP.getFreeHeap(),
        ESP.getMaxAllocHeap()
    );

    return success;
}

bool visionTrackPerson(VisionTrackResult &result) {
    clearResult(result);

    if (!visionLiveEnabled() ||
        playing ||
        WiFi.status() != WL_CONNECTED) {
        return false;
    }

    if (!visionLiveTrackPause()) {
        return false;
    }

    bool success = false;
    size_t jpegLength = 0;

    if (!playing &&
        visionLiveEnabled() &&
        WiFi.status() == WL_CONNECTED &&
        visionLiveTrackCapture(jpegLength)) {

        uint8_t *jpeg = visionLiveSharedJpeg();

        if (jpeg &&
            jpegLength > 0 &&
            jpegLength <= TRACK_JPEG_MAX &&
            !playing &&
            visionLiveEnabled() &&
            WiFi.status() == WL_CONNECTED) {
            success = sendTrackHTTPS(
                jpeg,
                jpegLength,
                result
            );
        }
    }

    visionLiveTrackResume();

    if (playing || !visionLiveEnabled()) {
        clearResult(result);
        return false;
    }

    return success;
}
