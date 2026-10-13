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

static uint8_t trackJpeg[TRACK_JPEG_MAX];

static bool captureTrackFrame(size_t &length) {
  length = 0;

  if (!cameraMux) return false;

  if (xSemaphoreTake(
        cameraMux,
        pdMS_TO_TICKS(5000)) != pdTRUE) {
    Serial.println("TARS TRACK: CAMERA LOCK TIMEOUT");
    return false;
  }

  bool ok = false;

  if (camera &&
      cameraLive &&
      cameraOK &&
      !playing) {
    ok = I2SCamera::encodeFrameToJPEG(
      trackJpeg,
      &length,
      55
    );
  }

  xSemaphoreGive(cameraMux);

  if (!ok ||
      length == 0 ||
      length > TRACK_JPEG_MAX) {
    memset(trackJpeg, 0, sizeof(trackJpeg));
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
  size_t jpegLength,
  VisionTrackResult &result
) {
  result.found = false;
  result.score = 0;
  result.centerX = -1;
  result.targetX = -1;

  const char *prefix =
    "{\"mode\":\"track\",\"format\":\"jpeg\",\"image\":\"";

  const char *suffix = "\"}";

  size_t prefixLength = strlen(prefix);
  size_t suffixLength = strlen(suffix);
  size_t base64Capacity =
    ((jpegLength + 2) / 3) * 4;

  size_t required =
    prefixLength +
    base64Capacity +
    suffixLength +
    1;

  if (required > TRACK_JSON_MAX) {
    Serial.println("TARS TRACK: JSON TOO LARGE");
    return false;
  }

  char *body = (char *)malloc(TRACK_JSON_MAX);

  if (!body) {
    Serial.println("TARS TRACK: JSON ALLOC FAILED");
    return false;
  }

  memcpy(body, prefix, prefixLength);

  size_t encodedLength = 0;

  int encodeResult = mbedtls_base64_encode(
    (unsigned char *)body + prefixLength,
    TRACK_JSON_MAX - prefixLength - suffixLength,
    &encodedLength,
    trackJpeg,
    jpegLength
  );

  if (encodeResult != 0 ||
      encodedLength != base64Capacity) {
    free(body);
    Serial.println("TARS TRACK: BASE64 FAILED");
    return false;
  }

  size_t bodyLength =
    prefixLength + encodedLength;

  memcpy(body + bodyLength, suffix, suffixLength);
  bodyLength += suffixLength;
  body[bodyLength] = '\0';

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  String url = String(TARS_CLOUD_URL) + "/vision";

  if (!http.begin(client, url)) {
    free(body);
    Serial.println("TARS TRACK: HTTP BEGIN FAILED");
    return false;
  }

  http.setTimeout(TRACK_TIMEOUT);
  http.addHeader("Content-Type", "application/json");
  http.addHeader(
    "Authorization",
    String("Bearer ") + TARS_LIVE_TOKEN
  );

  Serial.println("TARS TRACK: POST DETR");

  int code = http.POST(
    (uint8_t *)body,
    bodyLength
  );

  free(body);

  Serial.printf(
    "TARS TRACK: HTTP=%d\n",
    code
  );

  if (code < 200 || code >= 300) {
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();

  JsonDocument doc;

  DeserializationError err =
    deserializeJson(doc, response);

  response = "";

  if (err) {
    Serial.println("TARS TRACK: INVALID JSON RESPONSE");
    return false;
  }

  String responseMode = doc["mode"].as<String>();
if (responseMode != "track") {
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

  if (!visionLivePause()) {
    Serial.println("TARS TRACK: VISION BUSY");
    return false;
  }

  bool cameraWasStopped = false;
  bool success = false;
  size_t jpegLength = 0;

  if (!captureTrackFrame(jpegLength)) {
    goto cleanup;
  }

  stopCamera();

  cameraWasStopped =
    !camera && !cameraLive && !cameraOK;

  if (!cameraWasStopped) {
    Serial.println("TARS TRACK: CAMERA STOP FAILED");
    goto cleanup;
  }

  success = sendTrackRequest(jpegLength, result);

cleanup:
  memset(trackJpeg, 0, sizeof(trackJpeg));

  visionLiveResume();

  // vision_live.cpp tidak mengetahui bahwa kamera
  // dihentikan oleh modul tracking ini.
  // Pastikan kamera dipulihkan melalui API yang ada.
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
