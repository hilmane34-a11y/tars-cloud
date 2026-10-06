#include "camera_wifi_live.h"

#include <Arduino.h>
#include <WiFi.h>
#include <OV7670.h>
#include <I2SCamera.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern OV7670 *camera;
extern SemaphoreHandle_t cameraMux;
extern bool cameraLive;
extern bool cameraOK;
extern bool playing;

static WiFiServer liveServer(80);
static WiFiClient liveClient;

static uint8_t liveJpeg[12000];

static void sendRoot(WiFiClient &c) {
  c.println("HTTP/1.1 200 OK");
  c.println("Content-Type: text/html");
  c.println("Connection: close");
  c.println();
  c.println("<!doctype html><html><body>");
  c.println("<h3>TARS LIVE CAMERA</h3>");
  c.println("<img src=\"/stream\" style=\"width:100%;max-width:640px;\">");
  c.println("</body></html>");
}

static void send404(WiFiClient &c) {
  c.println("HTTP/1.1 404 Not Found");
  c.println("Content-Type: text/plain");
  c.println("Connection: close");
  c.println();
  c.println("TARS LIVE CAMERA");
}

static bool captureLiveJPEG(size_t &len) {
  len = 0;

  if (!cameraMux || !camera || !cameraLive || !cameraOK || playing)
    return false;

  if (xSemaphoreTake(cameraMux, pdMS_TO_TICKS(1000)) != pdTRUE)
    return false;

  bool ok = false;

  if (camera && cameraLive && cameraOK && !playing) {
    ok = I2SCamera::encodeFrameToJPEG(
      liveJpeg,
      &len,
      58
    );
  }

  xSemaphoreGive(cameraMux);

  if (!ok || len == 0 || len > sizeof(liveJpeg)) {
    len = 0;
    return false;
  }

  return true;
}

static void streamClient(WiFiClient &c) {

  c.println("HTTP/1.1 200 OK");
  c.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  c.println("Cache-Control: no-cache");
  c.println("Pragma: no-cache");
  c.println("Connection: close");
  c.println();

  uint32_t lastFrame = 0;

  while (c.connected()) {

    if (!cameraLive || !camera || !cameraOK || playing) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    if (millis() - lastFrame < 140) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    lastFrame = millis();

    size_t len = 0;

    if (!captureLiveJPEG(len)) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    c.print("--frame\r\n");
    c.print("Content-Type: image/jpeg\r\n");
    c.print("Content-Length: ");
    c.print(len);
    c.print("\r\n\r\n");

    size_t sent = 0;

    while (sent < len && c.connected()) {
      size_t n = c.write(liveJpeg + sent, len - sent);

      if (!n) {
        vTaskDelay(pdMS_TO_TICKS(2));
        continue;
      }

      sent += n;
    }

    c.print("\r\n");

    memset(liveJpeg, 0, len);

    if (sent != len)
      break;
  }

  c.stop();
}

static void liveTask(void *) {

  liveServer.begin();
  liveServer.setNoDelay(true);

  Serial.println("TARS: WIFI LIVE CAMERA SERVER READY");

  for (;;) {

    if (!WiFi.isConnected()) {
      if (liveClient)
        liveClient.stop();

      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (!liveClient || !liveClient.connected()) {

      WiFiClient incoming = liveServer.available();

      if (!incoming) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }

      liveClient = incoming;

      uint32_t start = millis();

      while (liveClient.connected() &&
             !liveClient.available() &&
             millis() - start < 2000) {
        vTaskDelay(pdMS_TO_TICKS(5));
      }

      if (!liveClient.available()) {
        liveClient.stop();
        continue;
      }

      String request = liveClient.readStringUntil('\n');
      request.trim();

      while (liveClient.available()) {
        String h = liveClient.readStringUntil('\n');
        if (h == "\r" || h.length() == 0)
          break;
      }

      Serial.println("TARS: LIVE REQUEST " + request);

      if (request.startsWith("GET /stream")) {
        streamClient(liveClient);
      }
      else if (request.startsWith("GET /")) {
        sendRoot(liveClient);
        liveClient.stop();
      }
      else {
        send404(liveClient);
        liveClient.stop();
      }

      continue;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void cameraWifiLiveBegin() {

  static bool started = false;

  if (started)
    return;

  started = true;

  BaseType_t r = xTaskCreatePinnedToCore(
    liveTask,
    "TARS_CAM_WIFI",
    4096,
    nullptr,
    1,
    nullptr,
    0
  );

  if (r == pdPASS)
    Serial.println("TARS: WIFI LIVE CAMERA TASK READY");
  else {
    started = false;
    Serial.println("TARS: WIFI LIVE CAMERA TASK FAILED");
  }
}
