#include "vision_live.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <OV7670.h>
#include <mbedtls/base64.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"
#include "../lib/TARS-OV7670/Config.h"

#ifndef TARS_LIVE_TOKEN
#error "Tambahkan TARS_LIVE_TOKEN di config.h"
#endif

#define VISION_JPEG_MAX (30 * 1024)
#define VISION_LOCK_WAIT 15000
#define VISION_B64_CHUNK 768
#define VISION_RESPONSE_MAX 8192

#if OV7670_MAX_JPEG_SIZE != VISION_JPEG_MAX
#error "OV7670_MAX_JPEG_SIZE harus 30 KB"
#endif

extern OV7670 *camera;
extern SemaphoreHandle_t cameraMux;
extern bool cameraLive, cameraOK, playing;
extern bool wifiOK();
extern bool visionLiveEnabled();
extern void stopCamera();
extern void startCamera();

static uint8_t visionJpeg[VISION_JPEG_MAX];
static unsigned char base64Buffer[1025];
static char questionJson[1024];
static char jsonPrefix[1200];

static portMUX_TYPE visionLock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool visionBusy = false;
static bool visionPaused = false;
static bool cameraStoppedForVision = false;

static bool acquireVision() {
    bool ok = false;
    portENTER_CRITICAL(&visionLock);
    if (!visionBusy) visionBusy = ok = true;
    portEXIT_CRITICAL(&visionLock);
    return ok;
}

static bool acquireVisionWait(uint32_t timeoutMs) {
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
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

static String cloudURL() {
    String url = TARS_CLOUD_URL;
    while (url.endsWith("/")) url.remove(url.length() - 1);
    return url;
}

static bool writeAll(WiFiClientSecure &client,
                     const uint8_t *data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        size_t n = client.write(data + sent, length - sent);
        if (!n) return false;
        sent += n;
        yield();
    }
    return true;
}

static bool writeText(WiFiClientSecure &client, const char *text) {
    return writeAll(client, (const uint8_t *)text, strlen(text));
}

static String readLine(WiFiClientSecure &client) {
    String line;
    uint32_t start = millis();

    while (millis() - start < 15000) {
        while (client.available()) {
            char c = client.read();
            if (c == '\n') {
                if (line.endsWith("\r"))
                    line.remove(line.length() - 1);
                return line;
            }
            if (line.length() < 512) line += c;
        }
        if (!client.connected()) break;
        delay(1);
    }
    return line;
}

static bool readExact(WiFiClientSecure &client,
                      uint8_t *out, size_t length) {
    size_t received = 0;
    uint32_t start = millis();

    while (received < length && millis() - start < 20000) {
        int available = client.available();

        if (available > 0) {
            size_t wanted = length - received;
            if ((size_t)available < wanted) wanted = available;

            int n = client.read(out + received, wanted);
            if (n > 0) received += n;
        } else if (!client.connected()) {
            break;
        } else {
            delay(1);
        }
    }
    return received == length;
}

static bool captureFrame(size_t &length) {
    length = 0;

    if (!cameraMux) return false;

    if (xSemaphoreTake(cameraMux, pdMS_TO_TICKS(3000)) != pdTRUE) {
        Serial.println("TARS: VISION CAMERA LOCK FAILED");
        return false;
    }

    bool ready = camera && cameraLive && cameraOK && !playing;
    bool ok = false;

    if (ready) {
        ok = I2SCamera::encodeFrameToJPEG(
            visionJpeg, &length, 25
        );
    }

    xSemaphoreGive(cameraMux);

    if (!ok || !length || length > sizeof(visionJpeg)) {
        length = 0;
        Serial.println("TARS: VISION JPEG FAILED");
        return false;
    }

    Serial.printf("TARS: VISION JPEG READY SIZE=%u\n",
                  (unsigned)length);
    return true;
}

static bool stopCameraForTLS() {
    cameraStoppedForVision = true;
    stopCamera();

    bool stopped = !camera && !cameraLive && !cameraOK;

    Serial.printf(
        "TARS: VISION CAMERA OFF=%d HEAP=%u LARGEST=%u\n",
        stopped, ESP.getFreeHeap(), ESP.getMaxAllocHeap()
    );

    return stopped;
}

static bool buildVisionPrefix(const String &question,
                              size_t jpegLength,
                              size_t &bodyLength) {
    bodyLength = 0;

    JsonDocument doc;
    doc["question"] = question;

    size_t qlen = serializeJson(
        doc["question"], questionJson, sizeof(questionJson)
    );

    if (!qlen || qlen >= sizeof(questionJson)) return false;

    int prefixLength = snprintf(
        jsonPrefix, sizeof(jsonPrefix),
        "{\"question\":%s,\"format\":\"jpeg\",\"image\":\"",
        questionJson
    );

    if (prefixLength <= 0 ||
        (size_t)prefixLength >= sizeof(jsonPrefix))
        return false;

    size_t encodedLength = ((jpegLength + 2) / 3) * 4;

    bodyLength = (size_t)prefixLength + encodedLength + 2;
    return true;
}

static bool sendVisionBody(WiFiClientSecure &client,
                           size_t jpegLength) {
    if (!writeText(client, jsonPrefix)) return false;

    size_t offset = 0;

    while (offset < jpegLength) {
        size_t chunk = jpegLength - offset;
        if (chunk > VISION_B64_CHUNK)
            chunk = VISION_B64_CHUNK;

        size_t encoded = 0;

        int rc = mbedtls_base64_encode(
            base64Buffer, sizeof(base64Buffer),
            &encoded, visionJpeg + offset, chunk
        );

        if (rc != 0 || !encoded) {
            Serial.printf("TARS: BASE64 ERROR=%d\n", rc);
            return false;
        }

        if (!writeAll(client, base64Buffer, encoded))
            return false;

        offset += chunk;
        yield();
    }

    return writeText(client, "\"}");
}

static String readHTTPResponse(WiFiClientSecure &client) {
    String status = readLine(client);
    Serial.println("TARS: VISION " + status);

    int contentLength = -1;
    bool chunked = false;

    while (true) {
        String line = readLine(client);
        if (!line.length()) break;

        String lower = line;
        lower.toLowerCase();

        if (lower.startsWith("content-length:")) {
            contentLength = lower.substring(15).toInt();
        }

        if (lower.indexOf("transfer-encoding:") >= 0 &&
            lower.indexOf("chunked") >= 0) {
            chunked = true;
        }
    }

    String body;
    body.reserve(2048);

    if (chunked) {
        while (body.length() < VISION_RESPONSE_MAX) {
            String line = readLine(client);
            int semicolon = line.indexOf(';');
            if (semicolon >= 0) line.remove(semicolon);

            size_t chunkSize = strtoul(line.c_str(), nullptr, 16);

            if (!chunkSize) {
                while (readLine(client).length()) {}
                break;
            }

            size_t keep = VISION_RESPONSE_MAX - body.length();
            if (keep > chunkSize) keep = chunkSize;

            while (keep) {
                uint8_t buffer[256];
                size_t n = keep > sizeof(buffer) ?
                           sizeof(buffer) : keep;

                if (!readExact(client, buffer, n))
                    return body;

                body.concat((const char *)buffer, n);
                keep -= n;
            }

            size_t discard = chunkSize -
                (chunkSize > VISION_RESPONSE_MAX -
                 (body.length() - (chunkSize < body.length() ?
                                   chunkSize : body.length())) ?
                 VISION_RESPONSE_MAX -
                 (body.length() - (chunkSize < body.length() ?
                                   chunkSize : body.length())) :
                 chunkSize);

            while (discard) {
                uint8_t dump[128];
                size_t n = discard > sizeof(dump) ?
                           sizeof(dump) : discard;
                if (!readExact(client, dump, n)) return body;
                discard -= n;
            }

            readLine(client);
        }
    } else if (contentLength >= 0) {
        size_t remaining = contentLength;
        if (remaining > VISION_RESPONSE_MAX)
            remaining = VISION_RESPONSE_MAX;

        while (remaining) {
            uint8_t buffer[256];
            size_t n = remaining > sizeof(buffer) ?
                       sizeof(buffer) : remaining;

            if (!readExact(client, buffer, n)) break;

            body.concat((const char *)buffer, n);
            remaining -= n;
        }
    } else {
        uint32_t start = millis();

        while (client.connected() &&
               body.length() < VISION_RESPONSE_MAX &&
               millis() - start < 20000) {
            while (client.available() &&
                   body.length() < VISION_RESPONSE_MAX) {
                body += (char)client.read();
                start = millis();
            }
            delay(1);
        }
    }

    return body;
}

static String sendVisionJSON(const String &question,
                             size_t jpegLength) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("TARS: VISION WIFI DISCONNECTED");
        return "";
    }

    size_t bodyLength = 0;

    if (!buildVisionPrefix(question, jpegLength, bodyLength)) {
        Serial.println("TARS: VISION PREFIX FAILED");
        return "";
    }

    String url = cloudURL();

    if (!url.startsWith("https://")) {
        Serial.println("TARS: VISION URL MUST USE HTTPS");
        return "";
    }

    url.remove(0, 8);

    int slash = url.indexOf('/');
    String hostPort = slash >= 0 ? url.substring(0, slash) : url;
    String path = slash >= 0 ? url.substring(slash) : "";

    String host = hostPort;
    uint16_t port = 443;

    int colon = hostPort.lastIndexOf(':');
    if (colon >= 0) {
        port = hostPort.substring(colon + 1).toInt();
        host = hostPort.substring(0, colon);
        if (!port) port = 443;
    }

    path += "/vision";

    Serial.printf("TARS: VISION TLS HEAP=%u LARGEST=%u\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20000);

    if (!client.connect(host.c_str(), port)) {
        Serial.println("TARS: VISION TLS CONNECT FAILED");
        return "";
    }

    char header[512];

    int hlen = snprintf(
        header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n",
        path.c_str(), hostPort.c_str(),
        TARS_LIVE_TOKEN, (unsigned)bodyLength
    );

    if (hlen <= 0 || (size_t)hlen >= sizeof(header) ||
        !writeAll(client, (const uint8_t *)header, hlen)) {
        Serial.println("TARS: VISION HEADER FAILED");
        client.stop();
        return "";
    }

    if (!sendVisionBody(client, jpegLength)) {
        Serial.println("TARS: VISION BODY SEND FAILED");
        client.stop();
        return "";
    }

    client.flush();

    String response = readHTTPResponse(client);
    client.stop();

    String answer;
    JsonDocument doc;

    if (!deserializeJson(doc, response)) {
        answer = doc["response"].as<String>();
        answer.trim();
    } else {
        Serial.println("TARS: VISION RESPONSE JSON ERROR");
        Serial.println(response);
    }

    Serial.printf("TARS: VISION TLS DONE HEAP=%u LARGEST=%u\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    return answer;
}

void visionLiveBegin() {
    Serial.println("TARS: ON-DEMAND VISION READY");
}

bool visionLivePause() {
    if (!visionLiveEnabled()) return false;
    if (visionPaused) return true;

    if (!acquireVisionWait(VISION_LOCK_WAIT)) {
        Serial.println("TARS: VISION LOCK TIMEOUT");
        return false;
    }

    visionPaused = true;
    cameraStoppedForVision = false;

    Serial.println("TARS: VISION CYCLE RESERVED");
    return true;
}

void visionLiveResume() {
    if (!visionPaused) return;

    if (playing) {
        Serial.println("TARS: VISION RESUME BLOCKED - AUDIO ACTIVE");
        return;
    }

    if (cameraStoppedForVision &&
        visionLiveEnabled() &&
        WiFi.status() == WL_CONNECTED) {
        startCamera();
    }

    cameraStoppedForVision = false;
    visionPaused = false;
    releaseVision();

    Serial.printf("TARS: VISION CYCLE END HEAP=%u LARGEST=%u\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

String visionLiveAsk(const String &question) {
    if (!visionLiveEnabled() || !visionPaused || !visionBusy)
        return "";

    if (WiFi.status() != WL_CONNECTED && !wifiOK()) {
        Serial.println("TARS: VISION WIFI ERROR");
        return "";
    }

    if (playing) {
        Serial.println("TARS: VISION BLOCKED - AUDIO ACTIVE");
        return "";
    }

    size_t jpegLength = 0;

    if (!captureFrame(jpegLength)) return "";

    if (!stopCameraForTLS()) {
        Serial.println("TARS: VISION CAMERA STOP FAILED");
        memset(visionJpeg, 0, jpegLength);
        return "";
    }

    String answer = sendVisionJSON(question, jpegLength);

    memset(visionJpeg, 0, jpegLength);
    memset(base64Buffer, 0, sizeof(base64Buffer));
    memset(questionJson, 0, sizeof(questionJson));
    memset(jsonPrefix, 0, sizeof(jsonPrefix));

    return answer;
}
