#include "camera_wifi_live.h"

#include <Arduino.h>
#include <WiFi.h>
#include <I2SCamera.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

extern SemaphoreHandle_t cameraMux;
extern bool cameraLive;
extern bool cameraOK;
extern bool playing;

#define LIVE_PORT       80
#define LIVE_JPEG_MAX   8192
#define LIVE_QUALITY    40
#define LIVE_INTERVAL   200
#define LIVE_TASK_STACK 3072

static WiFiServer liveServer(LIVE_PORT);

/*
   INTERNAL RAM
   8 KB saja supaya heap TARS tetap lega.
*/
static uint8_t liveJpeg[LIVE_JPEG_MAX];

static void sendPage(WiFiClient &client)
{
    static const char page[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>TARS CAMERA</title>"
        "<style>"
        "html,body{margin:0;background:#000;color:#fff;text-align:center}"
        "img{width:100%;max-width:640px;height:auto}"
        "</style>"
        "</head>"
        "<body>"
        "<h3>TARS LIVE CAMERA</h3>"
        "<img src='/stream'>"
        "</body>"
        "</html>";

    client.print(page);
}

static void sendStreamHeader(WiFiClient &client)
{
    client.print(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-cache\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n"
    );
}

static bool captureLiveJPEG(size_t &length)
{
    length = 0;

    if (!cameraMux)
        return false;

    if (!cameraLive || !cameraOK || !camera || playing)
        return false;

    if (xSemaphoreTake(cameraMux, pdMS_TO_TICKS(1000)) != pdTRUE)
        return false;

    bool ok = false;

    if (cameraLive && cameraOK && camera && !playing) {
        ok = I2SCamera::encodeFrameToJPEG(
            liveJpeg,
            &length,
            LIVE_QUALITY
        );
    }

    xSemaphoreGive(cameraMux);

    if (!ok || length == 0 || length > LIVE_JPEG_MAX) {
        length = 0;
        return false;
    }

    return true;
}

static void streamClient(WiFiClient &client)
{
    sendStreamHeader(client);

    uint32_t lastFrame = 0;

    while (client.connected()) {

        /*
           Jangan ganggu kamera saat TARS sedang
           memproses STT/TTS/vision.
        */
        if (playing || !cameraLive || !cameraOK || !camera) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (millis() - lastFrame < LIVE_INTERVAL) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        lastFrame = millis();

        size_t jpegLength = 0;

        if (!captureLiveJPEG(jpegLength)) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        client.print(
            "--frame\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: "
        );

        client.print(jpegLength);

        client.print("\r\n\r\n");

        size_t sent = 0;

        while (sent < jpegLength && client.connected()) {

            size_t chunk = jpegLength - sent;

            if (chunk > 1024)
                chunk = 1024;

            size_t n = client.write(
                liveJpeg + sent,
                chunk
            );

            if (!n)
                break;

            sent += n;

            vTaskDelay(1);
        }

        client.print("\r\n");
    }

    client.stop();
}

static void cameraWifiTask(void *)
{
    for (;;) {

        if (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        WiFiClient client = liveServer.available();

        if (!client) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        client.setTimeout(1000);

        char request[128];
        size_t pos = 0;

        uint32_t start = millis();

        while (client.connected() &&
               millis() - start < 1000) {

            while (client.available()) {

                char c = client.read();

                if (c == '\n') {
                    request[pos] = '\0';
                    break;
                }

                if (pos < sizeof(request) - 1)
                    request[pos++] = c;
            }

            if (pos && request[pos - 1] == '\r')
                request[pos - 1] = '\0';

            if (strstr(request, "GET /stream")) {
                streamClient(client);
                break;
            }

            if (strstr(request, "GET /")) {
                sendPage(client);
                client.stop();
                break;
            }

            vTaskDelay(1);
        }

        if (client.connected())
            client.stop();
    }
}

void cameraWifiLiveBegin()
{
    liveServer.begin();
    liveServer.setNoDelay(true);

    xTaskCreatePinnedToCore(
        cameraWifiTask,
        "TARS_WIFI_CAM",
        LIVE_TASK_STACK,
        nullptr,
        1,
        nullptr,
        0
    );

    Serial.println("TARS: WIFI CAMERA LIVE READY");
    Serial.printf(
        "TARS: CAMERA LIVE PORT=%u JPEG=%u QUALITY=%u\n",
        LIVE_PORT,
        LIVE_JPEG_MAX,
        LIVE_QUALITY
    );
}
