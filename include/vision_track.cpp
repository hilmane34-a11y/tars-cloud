#include "vision_track.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <I2SCamera.h>
#include <mbedtls/base64.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include <vision_live.h>
#include <OV7670.h>

#ifndef TARS_LIVE_TOKEN
#error "Tambahkan TARS_LIVE_TOKEN di config.h"
#endif

#define TRACK_JPEG_MAX 20000
#define TRACK_JSON_MAX 28000
#define TRACK_TIMEOUT 45000

extern OV7670 *camera;
extern SemaphoreHandle_t cameraMux;
extern bool cameraLive;
extern bool cameraOK;
extern bool playing;

extern bool wifiOK();
extern bool visionLiveEnabled();
extern void stopCamera();
extern bool startCamera();

static const char TRACK_PREFIX[] =
  "{\"mode\":\"track\",\"format\":\"jpeg\",\"image\":\"";

static const char TRACK_SUFFIX[] = "\"}";

// Stream JSON: mengirim prefix, Base64 per potongan,
// lalu suffix. Tidak membuat buffer JSON besar di heap.
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
    if (!prepare()) return -1;

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
    if (!prepare()) return -1;

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
  uint8_t encoded[256];

  bool prepare() {
    if (emitted >= totalLength) return false;

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

      if (rc != 0) {
        encodedLength = 0;
        encodedOffset = 0;
        return false;
      }

      jpegOffset += chunk;
      encodedLength = outputLength;
      encodedOffset = 0;

      return encodedLength > 0;
    }

    return suffixOffset < sizeof(TRACK_SUFFIX) - 1;
  }
};

static bool captureTrackFrame(
  uint8_t *jpeg,
  size_t capacity,
  size_t &length
) {
  length = 0;

  if (!jpeg || capacity < TRACK_JPEG_MAX || !cameraMux) {
    return false;
  }

  if (xSemaphoreTake(
        cameraMux,
        pdMS_TO_TICKS(5000)) != pdTRUE) {
    Serial.println("TARS TRACK: CAMERA LOCK TIMEOUT");
    return false;
  }

  bool ok = false;

  if (camera && cameraLive && cameraOK && !playing) {
    ok = I2SCamera::encodeFrameToJPEG(
      jpeg,
      &length,
      55
    );
  }

  xSemaphoreGive(cameraMux);

  if (!ok || length == 0 || length > capacity) {
    memset(jpeg, 0, capacity);
    Serial.println("TARS TRACK: JPEG CAPTURE FAILED");
    return false;
  }

  Serial.printf(
    "TARS TRACK: JPEG=%u bytes\n",
    (unsigned)length
  );

  return true;
}

static bool sendTrackRequest(
  uint8_t *&jpeg,
  size_t jpegLength,
  VisionTrackResult &result
) {
  result.found = false;
  result.score = 0;
  result.centerX = -1;
  result.targetX = -1;

  if (!jpeg || jpegLength == 0 ||
      jpegLength > TRACK_JPEG_MAX) {
    return false;
  }

  const size_t bodyLength =
    sizeof(TRACK_PREFIX) - 1 +
    ((jpegLength + 2) / 3) * 4 +
    sizeof(TRACK_SUFFIX) - 1;

  if (bodyLength + 1 > TRACK_JSON_MAX) {
    Serial.println("TARS TRACK: JSON TOO LARGE");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = String(TARS_CLOUD_URL) + "/vision";

  if (!http.begin(client, url)) {
    Serial.println("TARS TRACK: HTTP BEGIN FAILED");
    return false;
  }

  http.setTimeout(TRACK_TIMEOUT);
  http.addHeader("Content-Type", "application/json");
  http.addHeader(
    "Authorization",
    String("Bearer ") + TARS_LIVE_TOKEN
  );

  TrackBodyStream bodyStream(jpeg, jpegLength);

  Serial.println("TARS TRACK: POST DETR");

  int code = http.sendRequest(
    "POST",
    &bodyStream,
    bodyLength
  );

  // JPEG tidak dibutuhkan lagi setelah pengiriman selesai.
  memset(jpeg, 0, TRACK_JPEG_MAX);
  free(jpeg);
  jpeg = nullptr;

  Serial.printf("TARS TRACK: HTTP=%d\n", code);

  if (code < 200 || code >= 300) {
    http.end();
    return false;
  }

  // Respons tracking hanya berisi beberapa field kecil.
  String response = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, response);

  response = "";

  if (err) {
    Serial.println("TARS TRACK: INVALID JSON RESPONSE");
    return false;
  }

  if (!doc["mode"].is<const char *>() ||
      strcmp(doc["mode"].as<const char *>(), "track") != 0) {
    Serial.println("TARS TRACK: WRONG RESPONSE MODE");
    return false;
  }

  if (!doc["found"].is<bool>()) {
    Serial.println("TARS TRACK: MISSING FOUND FIELD");
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
    Serial.println("TARS TRACK: INVALID DETECTION DATA");
    result.found = false;
    return false;
  }

  result.targetX = doc["targetX"].as<int>();
  result.centerX = doc["centerX"].as<float>();
  result.score = doc["score"].as<float>();

  if (result.targetX < 0 || result.targetX > 319 ||
      result.centerX < 0.0f || result.centerX > 1.0f ||
      result.score < 0.60f || result.score > 1.0f) {
    Serial.println("TARS TRACK: DETECTION OUT OF RANGE");
    result.found = false;
    return false;
  }

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

  if (!wifiOK()) {
    Serial.println("TARS TRACK: WIFI UNAVAILABLE");
    return false;
  }

  uint8_t *trackJpeg =
    (uint8_t *)malloc(TRACK_JPEG_MAX);

  if (!trackJpeg) {
    Serial.println("TARS TRACK: JPEG ALLOC FAILED");
    return false;
  }

  bool visionPaused = false;
  bool cameraWasStopped = false;
  bool success = false;
  size_t jpegLength = 0;

  if (!visionLivePause()) {
    Serial.println("TARS TRACK: VISION BUSY");
    goto cleanup;
  }

  visionPaused = true;

  if (!captureTrackFrame(
        trackJpeg,
        TRACK_JPEG_MAX,
        jpegLength)) {
    goto cleanup;
  }

  stopCamera();

  cameraWasStopped = !camera && !cameraLive && !cameraOK;

  if (!cameraWasStopped) {
    Serial.println("TARS TRACK: CAMERA STOP FAILED");
    goto cleanup;
  }

  success = sendTrackRequest(
    trackJpeg,
    jpegLength,
    result
  );

cleanup:
  if (trackJpeg) {
    memset(trackJpeg, 0, TRACK_JPEG_MAX);
    free(trackJpeg);
    trackJpeg = nullptr;
  }

  if (visionPaused) {
    visionLiveResume();
  }

  if (cameraWasStopped) {
    if (!startCamera()) {
      Serial.println("TARS TRACK: CAMERA RESTART FAILED");
      success = false;
    }
  }

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
  Serial.println("TARS TRACK: DETR MODULE READY");
}
