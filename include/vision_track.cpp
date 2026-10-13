#include "vision_track.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <string.h>

#include "config.h"
#include "vision_live.h"

#ifndef TARS_LIVE_TOKEN
#error "Tambahkan TARS_LIVE_TOKEN di config.h"
#endif

#define TRACK_TIMEOUT 10000

extern bool wifiOK();
extern bool visionLiveEnabled();
extern bool playing;

static const char TRACK_PREFIX[] =
    "{\"mode\":\"track\",\"format\":\"jpeg\",\"image\":\"";

static const char TRACK_SUFFIX[] = "\"}";

// JSON dikirim bertahap; tidak ada buffer JSON 28 KB tambahan.
class TrackBodyStream : public Stream {
public:
    TrackBodyStream(const uint8_t *data, size_t length)
        : jpeg(data),
          jpegLength(length),
          jpegOffset(0),
          prefixOffset(0),
          suffixOffset(0),
          encodedLength(0),
          encodedOffset(0),
          totalLength(
              sizeof(TRACK_PREFIX) - 1 +
              ((length + 2) / 3) * 4 +
              sizeof(TRACK_SUFFIX) - 1
          ) {
    }

    int available() override {
        size_t remaining = totalLength - emitted;
        return remaining > 0x7FFF ? 0x7FFF : (int)remaining;
    }

    int read() override {
        if (!prepare()) {
            return -1;
        }

        ++emitted;

        if (prefixOffset < sizeof(TRACK_PREFIX) - 1) {
            return TRACK_PREFIX[prefixOffset++];
        }

        if (encodedOffset < encodedLength) {
            return encoded[encodedOffset++];
        }

        return TRACK_SUFFIX[suffixOffset++];
    }

    int peek() override {
        if (!prepare()) {
            return -1;
        }

        if (prefixOffset < sizeof(TRACK_PREFIX) - 1) {
            return TRACK_PREFIX[prefixOffset];
        }

        if (encodedOffset < encodedLength) {
            return encoded[encodedOffset];
        }

        return TRACK_SUFFIX[suffixOffset];
    }

    void flush() override {}

    size_t write(uint8_t) override {
        return 0;
    }

    size_t totalSize() const {
        return totalLength;
    }

private:
    const uint8_t *jpeg;
    size_t jpegLength;
    size_t jpegOffset;
    size_t prefixOffset;
    size_t suffixOffset;
    size_t encodedLength;
    size_t encodedOffset;
    size_t totalLength;
    size_t emitted = 0;

    // 192 byte input menghasilkan maksimal 256 byte Base64.
    uint8_t encoded[320];

    bool prepare() {
        if (emitted >= totalLength) {
            return false;
        }

        if (prefixOffset < sizeof(TRACK_PREFIX) - 1) {
            return true;
        }

        if (encodedOffset < encodedLength) {
            return true;
        }

        if (jpegOffset < jpegLength) {
            size_t remaining = jpegLength - jpegOffset;
            size_t chunk = remaining > 192 ? 192 : remaining;
            size_t outputLength = 0;

            int rc = mbedtls_base64_encode(
                encoded,
                sizeof(encoded),
                &outputLength,
                jpeg + jpegOffset,
                chunk
            );

            if (rc != 0 || outputLength == 0) {
                encodedLength = 0;
                encodedOffset = 0;
                return false;
            }

            jpegOffset += chunk;
            encodedLength = outputLength;
            encodedOffset = 0;

            return true;
        }

        return suffixOffset < sizeof(TRACK_SUFFIX) - 1;
    }
};
static bool sendTrackRequest(
    const uint8_t *jpeg,
    size_t jpegLength,
    VisionTrackResult &result
) {
    result.found = false;
    result.score = 0;
    result.centerX = -1;
    result.targetX = -1;

    if (!jpeg || jpegLength == 0 || jpegLength > 20000) {
        Serial.println("TARS TRACK ERROR: INVALID JPEG");
        return false;
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf(
            "TARS TRACK ERROR: WIFI DISCONNECTED STATUS=%d\n",
            (int)WiFi.status()
        );
        return false;
    }

    const size_t bodyLength =
        sizeof(TRACK_PREFIX) - 1 +
        ((jpegLength + 2) / 3) * 4 +
        sizeof(TRACK_SUFFIX) - 1;

    Serial.printf(
        "\nTARS TRACK DIAG: BEGIN JPEG=%u BODY=%u\n",
        (unsigned)jpegLength,
        (unsigned)bodyLength
    );

    Serial.printf(
        "TARS TRACK DIAG: HEAP free=%u min=%u largest=%u\n",
        ESP.getFreeHeap(),
        ESP.getMinFreeHeap(),
        ESP.getMaxAllocHeap()
    );

    Serial.printf(
        "TARS TRACK DIAG: WIFI RSSI=%d STATUS=%d\n",
        WiFi.RSSI(),
        (int)WiFi.status()
    );

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);

    HTTPClient http;

    String url = String(TARS_CLOUD_URL) + "/vision";

    Serial.println("TARS TRACK DIAG: STAGE=HTTP_BEGIN");

    if (!http.begin(client, url)) {
        Serial.printf(
            "TARS TRACK ERROR: HTTP_BEGIN FAILED HEAP=%u MAX=%u\n",
            ESP.getFreeHeap(),
            ESP.getMaxAllocHeap()
        );
        client.stop();
        return false;
    }

    http.setConnectTimeout(6000);
    http.setTimeout(TRACK_TIMEOUT);
    http.addHeader("Content-Type", "application/json");
    http.addHeader(
        "Authorization",
        String("Bearer ") + TARS_LIVE_TOKEN
    );

    TrackBodyStream bodyStream(jpeg, jpegLength);

    Serial.printf(
        "TARS TRACK DIAG: STAGE=SEND_REQUEST HEAP=%u MAX=%u\n",
        ESP.getFreeHeap(),
        ESP.getMaxAllocHeap()
    );

    uint32_t started = millis();

    int code = http.sendRequest(
        "POST",
        &bodyStream,
        bodyLength
    );

    uint32_t elapsed = millis() - started;

    Serial.printf(
        "TARS TRACK DIAG: HTTP=%d ERROR=%s ELAPSED=%lu ms\n",
        code,
        HTTPClient::errorToString(code).c_str(),
        (unsigned long)elapsed
    );

    Serial.printf(
        "TARS TRACK DIAG: AFTER_SEND HEAP=%u MIN=%u MAX=%u WIFI=%d RSSI=%d\n",
        ESP.getFreeHeap(),
        ESP.getMinFreeHeap(),
        ESP.getMaxAllocHeap(),
        (int)WiFi.status(),
        WiFi.RSSI()
    );

    char tlsError[160] = {0};
    int tlsErrorCode = client.lastError(
        tlsError,
        sizeof(tlsError)
    );

    Serial.printf(
        "TARS TRACK DIAG: TLS_LAST_ERROR=%d DETAIL=%s\n",
        tlsErrorCode,
        tlsError[0] ? tlsError : "(no detail)"
    );

    String response;

    if (code >= 200 && code < 300) {
        Serial.println("TARS TRACK DIAG: STAGE=READ_RESPONSE");
        response = http.getString();

        Serial.printf(
            "TARS TRACK DIAG: RESPONSE_BYTES=%u\n",
            (unsigned)response.length()
        );
    } else {
        Serial.printf(
            "TARS TRACK DIAG: REQUEST_FAILED CODE=%d\n",
            code
        );
    }

    http.end();
    client.stop();

    Serial.printf(
        "TARS TRACK DIAG: CLOSED HEAP=%u MAX=%u\n",
        ESP.getFreeHeap(),
        ESP.getMaxAllocHeap()
    );

    if (code < 200 || code >= 300) {
        response = "";
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, response);
    response = "";

    if (err) {
        Serial.printf(
            "TARS TRACK ERROR: RESPONSE JSON=%s\n",
            err.c_str()
        );
        return false;
    }

    if (!doc["mode"].is<const char *>() ||
        strcmp(doc["mode"].as<const char *>(), "track") != 0) {
        Serial.println("TARS TRACK ERROR: WRONG RESPONSE MODE");
        return false;
    }

    if (!doc["found"].is<bool>()) {
        Serial.println("TARS TRACK ERROR: MISSING FOUND");
        return false;
    }

    result.found = doc["found"].as<bool>();

    if (!result.found) {
        result.targetX = -1;
        result.centerX = -1;
        result.score = 0;
        return true;
    }

    if (!doc["targetX"].is<int>() ||
        !doc["centerX"].is<float>() ||
        !doc["score"].is<float>()) {
        Serial.println("TARS TRACK ERROR: INVALID DETECTION DATA");
        result.found = false;
        return false;
    }

    result.targetX = doc["targetX"].as<int>();
    result.centerX = doc["centerX"].as<float>();
    result.score = doc["score"].as<float>();

    if (result.targetX < 0 || result.targetX > 319 ||
        result.centerX < 0.0f || result.centerX > 1.0f ||
        result.score < 0.60f || result.score > 1.0f) {
        Serial.println("TARS TRACK ERROR: DETECTION OUT OF RANGE");
        result.found = false;
        return false;
    }

    Serial.println("TARS TRACK DIAG: SUCCESS");
    return true;
}
bool visionTrackPerson(VisionTrackResult &result) {
    result.found = false;
    result.score = 0;
    result.centerX = -1;
    result.targetX = -1;

    if (!visionLiveEnabled() || playing) {
        return false;
    }

    if (!wifiOK() || WiFi.status() != WL_CONNECTED) {
        return false;
    }

    // DETR tidak menunggu atau merebut giliran Vision pengguna.
    if (!visionLiveTrackPause()) {
        return false;
    }

    bool success = false;
    size_t jpegLength = 0;

    if (visionLiveTrackCapture(jpegLength)) {
        uint8_t *sharedJpeg = visionLiveSharedJpeg();

        if (sharedJpeg) {
            success = sendTrackRequest(
                sharedJpeg,
                jpegLength,
                result
            );
        } else {
            Serial.println("TARS TRACK: SHARED JPEG UNAVAILABLE");
        }
    }

    // Jalur sukses maupun gagal selalu mencoba melepas giliran DETR.
    visionLiveTrackResume();

    if (success) {
        if (result.found) {
            Serial.printf(
                "TARS TRACK: PERSON x=%d center=%.3f score=%.2f\n",
                result.targetX,
                result.centerX,
                result.score
            );
        } else {
            Serial.println("TARS TRACK: NO PERSON");
        }
    }

    return success;
}

void visionTrackBegin() {
    Serial.println("TARS TRACK: SHARED JPEG BUFFER READY");
}
