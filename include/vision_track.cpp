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
#include "config.h"

#ifndef TARS_LIVE_TOKEN
#error "Tambahkan TARS_LIVE_TOKEN di config.h"
#endif

#define TRACK_TIMEOUT 8000
#define TRACK_JPEG_MAX 20000
#define TRACK_JSON_MAX 28000

extern bool playing;
extern bool visionLiveEnabled();

static void clearResult(VisionTrackResult &r) {
    r.found = false;
    r.score = 0;
    r.centerX = 0;
    r.targetX = -1;
}

static String trackURL() {
    String url = TARS_CLOUD_URL;
    while (url.endsWith("/")) url.remove(url.length() - 1);
    return url + "/vision";
}

void visionTrackBegin() {
    Serial.println("TARS TRACK: HTTPS DETR READY");
}

static bool sendTrackHTTPS(const uint8_t *jpeg, size_t len,
                           VisionTrackResult &r) {
    if (!jpeg || !len || len > TRACK_JPEG_MAX ||
        WiFi.status() != WL_CONNECTED) return false;

    const char *head = "{\"mode\":\"track\",\"format\":\"jpeg\",\"image\":\"";
    size_t prefix = strlen(head);
    size_t b64Max = 4 * ((len + 2) / 3);
    size_t capacity = prefix + b64Max + 3;

    if (capacity > TRACK_JSON_MAX) return false;

    char *body = (char *)malloc(capacity);
    if (!body) {
        Serial.printf("TARS TRACK: ALLOC FAIL HEAP=%u MAX=%u NEED=%u\n",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap(), (unsigned)capacity);
        return false;
    }

    memcpy(body, head, prefix);
    size_t encoded = 0;
    int err = mbedtls_base64_encode(
        (unsigned char *)body + prefix, capacity - prefix,
        &encoded, jpeg, len);

    if (err || prefix + encoded + 3 > capacity) {
        Serial.printf("TARS TRACK: BASE64 ERROR=%d\n", err);
        free(body);
        return false;
    }

    size_t bodyLen = prefix + encoded;
    body[bodyLen++] = '"';
    body[bodyLen++] = '}';
    body[bodyLen] = '\0';

    Serial.printf("TARS TRACK: POST JPEG=%u BODY=%u HEAP=%u\n",
        (unsigned)len, (unsigned)bodyLen, ESP.getFreeHeap());

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(TRACK_TIMEOUT);

    HTTPClient http;
    http.setTimeout(TRACK_TIMEOUT);

    bool success = false;
    if (http.begin(client, trackURL())) {
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Authorization", String("Bearer ") + TARS_LIVE_TOKEN);

        int code = http.POST((uint8_t *)body, bodyLen);
        Serial.printf("TARS TRACK: HTTP=%d", code);
        if (code < 0)
            Serial.printf(" (%s)", http.errorToString(code).c_str());
        Serial.println();

        if (code >= 200 && code < 300) {
            JsonDocument doc;
            DeserializationError e = deserializeJson(doc, http.getStream());
            if (!e) {
                r.found = doc["found"] | false;
                r.score = doc["score"] | 0.0f;
                r.centerX = doc["centerX"] | 0.0f;
                r.targetX = doc["targetX"] | -1;

                if (r.found && r.score >= 0.60f &&
                    r.targetX >= 0 && r.targetX <= 319) {
                    success = true;
                } else {
                    clearResult(r);
                    success = true;
                }
            } else {
                Serial.println("TARS TRACK: RESPONSE JSON ERROR");
            }
        } else if (code >= 0) {
            Serial.printf("TARS TRACK: SERVER ERROR=%s\n",
                          http.getString().c_str());
        }
        http.end();
    } else {
        Serial.println("TARS TRACK: HTTP BEGIN FAILED");
    }

    client.stop();
    free(body);

    Serial.printf("TARS TRACK: DONE OK=%d FOUND=%d SCORE=%.2f X=%d HEAP=%u MAX=%u\n",
        success, r.found, r.score, r.targetX,
        ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return success;
}

bool visionTrackPerson(VisionTrackResult &r) {
    clearResult(r);
    if (!visionLiveEnabled() || playing ||
        WiFi.status() != WL_CONNECTED) return false;
    if (!visionLiveTrackPause()) return false;

    bool ok = false;
    size_t len = 0;

    if (!playing && visionLiveEnabled() &&
        WiFi.status() == WL_CONNECTED &&
        visionLiveTrackCapture(len)) {
        uint8_t *jpeg = visionLiveSharedJpeg();
        if (jpeg && len && len <= TRACK_JPEG_MAX &&
            !playing && visionLiveEnabled() &&
            WiFi.status() == WL_CONNECTED)
            ok = sendTrackHTTPS(jpeg, len, r);
    }

    visionLiveTrackResume();
    if (playing || !visionLiveEnabled()) {
        clearResult(r);
        return false;
    }
    return ok;
}
