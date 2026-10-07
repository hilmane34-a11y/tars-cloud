#include "camera_wifi_live.h"

#include <Arduino.h>
#include <WiFi.h>
#include <I2SCamera.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

extern SemaphoreHandle_t cameraMux;
extern bool cameraLive;
extern bool cameraOK;
extern bool playing;

#define LIVE_PORT        80
#define LIVE_JPEG_MAX    10240
#define LIVE_QUALITY     50
#define LIVE_INTERVAL    200
#define LIVE_TASK_STACK  3072

static WiFiServer liveServer(LIVE_PORT);
static uint8_t liveJpeg[LIVE_JPEG_MAX];
static bool liveServerStarted = false;

static void sendPage(WiFiClient &client)
{
    client.print(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<!DOCTYPE html>"
        "<html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta http-equiv='refresh' content='10'>"
        "<title>TARS CAMERA</title>"
        "<style>"
        "html,body{margin:0;background:#000;color:#fff;text-align:center;font-family:Arial}"
        "h3{margin:10px}"
        "img{display:block;width:100%;max-width:640px;height:auto;margin:auto}"
        "</style>"
        "</head><body>"
        "<h3>TARS LIVE CAMERA</h3>"
        "<img src='/stream'>"
        "</body></html>"
    );
}

static void sendStreamHeader(WiFiClient &client)
{
    client.print(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
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

    if (!cameraLive || !cameraOK || playing)
        return false;

    if (xSemaphoreTake(cameraMux, pdMS_TO_TICKS(1500)) != pdTRUE)
        return false;

    bool ok = false;

    if (cameraLive && cameraOK && !playing) {
        size_t outLen = LIVE_JPEG_MAX;

        ok = I2SCamera::encodeFrameToJPEG(
            liveJpeg,
            &outLen,
            LIVE_QUALITY
        );

        if (ok)
            length = outLen;
    }

    xSemaphoreGive(cameraMux);

    if (!ok || length == 0 || length > LIVE_JPEG_MAX) {
        length = 0;
        return false;
    }

    return true;
}

static bool sendJPEGFrame(
    WiFiClient &client,
    size_t jpegLength
)
{
    if (!client.connected())
        return false;

    client.print("--frame\r\n");
    client.print("Content-Type: image/jpeg\r\n");
    client.print("Content-Length: ");
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

        if (n == 0)
            return false;

        sent += n;
        vTaskDelay(1);
    }

    client.print("\r\n");

    return sent == jpegLength;
}

static void streamClient(WiFiClient &client)
{
    Serial.println("TARS LIVE: STREAM CONNECTED");

    sendStreamHeader(client);

    uint32_t lastFrame = 0;
    uint32_t failCount = 0;

    while (client.connected()) {

        if (playing || !cameraLive || !cameraOK) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint32_t now = millis();

        if (now - lastFrame < LIVE_INTERVAL) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        lastFrame = now;

        size_t jpegLength = 0;

        if (!captureLiveJPEG(jpegLength)) {

            failCount++;

            if (failCount >= 20) {
                Serial.println(
                    "TARS LIVE: JPEG CAPTURE FAILED"
                );
                failCount = 0;
            }

            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        failCount = 0;

        if (!sendJPEGFrame(client, jpegLength))
            break;
    }

    client.stop();

    Serial.println("TARS LIVE: STREAM DISCONNECTED");
}

static void ensureLiveServer()
{
    if (WiFi.status() != WL_CONNECTED)
        return;

    if (liveServerStarted)
        return;

    liveServer.begin();
    liveServer.setNoDelay(true);

    liveServerStarted = true;

    Serial.println("TARS LIVE: HTTP SERVER STARTED");
    Serial.printf(
        "TARS LIVE: LISTENING PORT %u\n",
        LIVE_PORT
    );
}

static void cameraWifiTask(void *)
{
    bool wasConnected = false;

    for (;;) {

        bool connected =
            WiFi.status() == WL_CONNECTED;

        if (!connected) {

            if (wasConnected) {
                Serial.println(
                    "TARS LIVE: WIFI DISCONNECTED"
                );

                liveServerStarted = false;
            }

            wasConnected = false;

            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!wasConnected) {

            Serial.println(
                "TARS LIVE: WIFI CONNECTED"
            );

            Serial.print(
                "TARS LIVE: IP = "
            );
            Serial.println(
                WiFi.localIP()
            );

            ensureLiveServer();

            wasConnected = true;
        }

        ensureLiveServer();

        WiFiClient client =
            liveServer.available();

        if (!client) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        Serial.println(
            "TARS LIVE: HTTP CLIENT CONNECTED"
        );

        client.setTimeout(1500);

        char request[256];
        memset(request, 0, sizeof(request));

        size_t pos = 0;
        bool gotRequest = false;

        uint32_t start = millis();

        while (
            client.connected() &&
            millis() - start < 1500
        ) {

            while (client.available()) {

                char c = client.read();

                if (pos < sizeof(request) - 1)
                    request[pos++] = c;

                if (
                    pos >= 4 &&
                    request[pos - 4] == '\r' &&
                    request[pos - 3] == '\n' &&
                    request[pos - 2] == '\r' &&
                    request[pos - 1] == '\n'
                ) {

                    request[pos] = '\0';
                    gotRequest = true;
                    break;
                }
            }

            if (gotRequest)
                break;

            vTaskDelay(1);
        }

        if (!gotRequest) {
            client.stop();
            continue;
        }

        Serial.printf(
            "TARS LIVE REQUEST: %.80s\n",
            request
        );

        if (strstr(request, "GET /stream")) {

            streamClient(client);

        } else if (strstr(request, "GET /")) {

            sendPage(client);
            client.stop();

        } else {

            client.print(
                "HTTP/1.1 404 Not Found\r\n"
                "Connection: close\r\n"
                "Content-Type: text/plain\r\n"
                "\r\n"
                "TARS CAMERA 404"
            );

            client.stop();
        }

        vTaskDelay(1);
    }
}

void cameraWifiLiveBegin()
{
    liveServerStarted = false;

    Serial.println(
        "TARS: WIFI CAMERA LIVE INIT"
    );

    if (WiFi.status() == WL_CONNECTED) {

        ensureLiveServer();

        Serial.print(
            "TARS: CAMERA URL = http://"
        );
        Serial.print(
            WiFi.localIP()
        );
        Serial.println("/");

    } else {

        Serial.println(
            "TARS: WIFI NOT CONNECTED"
        );
    }

    xTaskCreatePinnedToCore(
        cameraWifiTask,
        "TARS_WIFI_CAM",
        LIVE_TASK_STACK,
        nullptr,
        1,
        nullptr,
        0
    );

    Serial.println(
        "TARS: WIFI CAMERA LIVE READY"
    );

    Serial.printf(
        "TARS: CAMERA LIVE PORT=%u JPEG=%u QUALITY=%u\n",
        LIVE_PORT,
        LIVE_JPEG_MAX,
        LIVE_QUALITY
    );

    Serial.println(
        "TARS: CAMERA LIVE SERVER ACTIVE"
    );
}
