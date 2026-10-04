
#include "vision_live.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <OV7670.h>
#include <mbedtls/base64.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"

#ifndef TARS_LIVE_TOKEN
#error "Tambahkan TARS_LIVE_TOKEN di config.h"
#endif

#ifndef OV7670_MAX_JPEG_SIZE
#error "OV7670_MAX_JPEG_SIZE belum didefinisikan"
#endif

#define VISION_JPEG_MAX OV7670_MAX_JPEG_SIZE
#define VISION_LOCK_WAIT 15000
#define VISION_B64_CHUNK 768
#define VISION_RESPONSE_MAX 8192

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

// CAPTURE SATU FRAME SEBELUM KAMERA DIMATIKAN
static bool captureFrame(size_t &length) {
    length = 0;

    if (!cameraMux) return false;

    if (xSemaphoreTake(cameraMux, pdMS_TO_TICKS(3000)) != pdTRUE) {
        Serial.println("TARS: VISION CAMERA LOCK FAILED");
        return false;
    }

    bool ready = camera && cameraLive && cameraOK && !playing;
    bool ok = false;

    if (ready)
        ok = I2SCamera::encodeFrameToJPEG(
            visionJpeg, &length, 25
        );

    xSemaphoreGive(cameraMux);

    if (!ok || !length || length > VISION_JPEG_MAX) {
        Serial.printf(
            "TARS: VISION JPEG FAILED OK=%d SIZE=%u\n",
            ok, (unsigned)length
        );
        length = 0;
        return false;
    }

    Serial.printf("TARS: VISION JPEG READY SIZE=%u\n",
                  (unsigned)length);
    return true;
}

// KAMERA OFF SETELAH JPEG BERHASIL
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

// KIRIM DATA SECARA UTUH MESKIPUN WRITE BISA PARSIAL
static bool writeAll(
    WiFiClientSecure &client,
    const uint8_t *data,
    size_t length
) {
    size_t sent = 0;

    while (sent < length) {
        size_t n = client.write(data + sent, length - sent);

        if (!n) {
            if (!client.connected()) return false;
            delay(1);
            continue;
        }

        sent += n;
    }

    return true;
}

static bool writeText(
    WiFiClientSecure &client,
    const char *data
) {
    return writeAll(
        client,
        (const uint8_t *)data,
        strlen(data)
    );
}

// PARSE URL WORKER
static bool parseCloudURL(
    String &host,
    uint16_t &port,
    String &path
) {
    String url = cloudURL();

    if (!url.startsWith("https://")) return false;

    url.remove(0, 8);

    int slash = url.indexOf('/');
    String authority = slash < 0 ? url : url.substring(0, slash);
    path = slash < 0 ? "" : url.substring(slash);

    port = 443;
    host = authority;

    int colon = authority.lastIndexOf(':');

    if (colon >= 0) {
        host = authority.substring(0, colon);
        port = authority.substring(colon + 1).toInt();

        if (!port) return false;
    }

    if (!host.length()) return false;

    path += "/vision";
    return true;
}

// BACA BODY RESPONSE DENGAN BATAS RAM
static bool readResponseData(
    WiFiClientSecure &client,
    String &body,
    size_t length
) {
    uint8_t buffer[256];

    while (length) {
        size_t count = length > sizeof(buffer) ?
                       sizeof(buffer) : length;

        int received = client.readBytes(buffer, count);

        if (received <= 0) return false;

        if (body.length() + received > VISION_RESPONSE_MAX)
            return false;

        body.concat((const char *)buffer, received);
        length -= received;
    }

    return true;
}

static String readHTTPResponse(
    WiFiClientSecure &client,
    int &status
) {
    status = 0;

    String line = client.readStringUntil('\n');
    line.trim();

    if (line.startsWith("HTTP/")) {
        int pos = line.indexOf(' ');
        if (pos > 0)
            status = line.substring(pos + 1).toInt();
    }

    int contentLength = -1;
    bool chunked = false;

    while (client.connected() || client.available()) {
        line = client.readStringUntil('\n');
        line.trim();

        if (!line.length()) break;

        String lower = line;
        lower.toLowerCase();

        if (lower.startsWith("content-length:"))
            contentLength = lower.substring(15).toInt();

        if (lower.startsWith("transfer-encoding:") &&
            lower.indexOf("chunked") >= 0)
            chunked = true;
    }

    String body;
    body.reserve(2048);

    if (chunked) {
        while (client.connected() || client.available()) {
            line = client.readStringUntil('\n');
            line.trim();

            int semicolon = line.indexOf(';');
            if (semicolon >= 0)
                line.remove(semicolon);

            size_t chunkSize = strtoul(
                line.c_str(), nullptr, 16
            );

            if (!chunkSize) {
                while (client.connected() || client.available()) {
                    line = client.readStringUntil('\n');
                    if (line == "\r" || !line.length()) break;
                }
                break;
            }

            if (body.length() + chunkSize > VISION_RESPONSE_MAX)
                return "";

            if (!readResponseData(client, body, chunkSize))
                return "";

            client.readStringUntil('\n');
        }
    } else if (contentLength >= 0) {
        if (contentLength > VISION_RESPONSE_MAX)
            return "";

        if (!readResponseData(
            client, body, (size_t)contentLength
        ))
            return "";
    } else {
        while (client.connected() || client.available()) {
            while (client.available()) {
                char c = client.read();

                if (body.length() >= VISION_RESPONSE_MAX)
                    return "";

                body += c;
            }

            if (!client.connected()) break;
            delay(1);
        }
    }

    return body;
}

// KIRIM FOTO + PERTANYAAN DALAM SATU JSON STREAMING
static String sendVisionJSON(
    const String &question,
    size_t jpegLength
) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("TARS: VISION WIFI DISCONNECTED");
        return "";
    }

    String host, path;
    uint16_t port;

    if (!parseCloudURL(host, port, path)) {
        Serial.println("TARS: VISION URL INVALID");
        return "";
    }

    JsonDocument questionDoc;
    questionDoc["question"] = question;

    size_t qlen = serializeJson(
        questionDoc["question"],
        questionJson,
        sizeof(questionJson)
    );

    if (!qlen || qlen >= sizeof(questionJson)) {
        Serial.println("TARS: VISION QUESTION TOO LONG");
        return "";
    }

    int prefixLength = snprintf(
        jsonPrefix,
        sizeof(jsonPrefix),
        "{\"question\":%s,\"format\":\"jpeg\",\"image\":\"",
        questionJson
    );

    if (prefixLength <= 0 ||
        (size_t)prefixLength >= sizeof(jsonPrefix))
        return "";

    size_t encodedLength = ((jpegLength + 2) / 3) * 4;
    size_t bodyLength = (size_t)prefixLength +
                        encodedLength + 2;

    Serial.printf(
        "TARS: VISION TLS HEAP=%u LARGEST=%u JPEG=%u BODY=%u\n",
        ESP.getFreeHeap(),
        ESP.getMaxAllocHeap(),
        (unsigned)jpegLength,
        (unsigned)bodyLength
    );

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20000);

    if (!client.connect(host.c_str(), port)) {
        Serial.println("TARS: VISION TLS CONNECT FAILED");
        return "";
    }

    char header[700];

    int headerLength = snprintf(
        header,
        sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n",
        path.c_str(),
        host.c_str(),
        TARS_LIVE_TOKEN,
        (unsigned)bodyLength
    );

    if (headerLength <= 0 ||
        (size_t)headerLength >= sizeof(header)) {
        client.stop();
        return "";
    }

    bool ok = writeAll(
        client,
        (const uint8_t *)header,
        headerLength
    );

    if (ok)
        ok = writeAll(
            client,
            (const uint8_t *)jsonPrefix,
            prefixLength
        );

    // BASE64 DIKIRIM PER POTONGAN 768 BYTE
    for (size_t offset = 0;
         ok && offset < jpegLength;) {

        size_t chunk = jpegLength - offset;

        if (chunk > VISION_B64_CHUNK)
            chunk = VISION_B64_CHUNK;

        size_t outputLength = 0;

        int result = mbedtls_base64_encode(
            base64Buffer,
            sizeof(base64Buffer),
            &outputLength,
            visionJpeg + offset,
            chunk
        );

        if (result != 0 || !outputLength) {
            Serial.printf(
                "TARS: VISION BASE64 ERROR=%d\n",
                result
            );
            ok = false;
            break;
        }

        ok = writeAll(
            client,
            base64Buffer,
            outputLength
        );

        offset += chunk;
    }

    if (ok)
        ok = writeText(client, "\"}");

    if (!ok) {
        Serial.println("TARS: VISION STREAM SEND FAILED");
        client.stop();
        return "";
    }

    client.flush();

    int code = 0;
    String response = readHTTPResponse(client, code);

    Serial.printf("TARS: VISION HTTP=%d\n", code);

    client.stop();

    String answer;

    if (code >= 200 && code < 300) {
        JsonDocument doc;

        if (!deserializeJson(doc, response)) {
            answer = doc["response"].as<String>();
            answer.trim();
        } else {
            Serial.println("TARS: VISION RESPONSE JSON ERROR");
        }
    } else {
        Serial.println("TARS: VISION HTTP ERROR");
        Serial.println(response);
    }

    Serial.printf(
        "TARS: VISION TLS DONE HEAP=%u LARGEST=%u\n",
        ESP.getFreeHeap(),
        ESP.getMaxAllocHeap()
    );

    return answer;
}

// BEGIN: TIDAK ADA TASK UPLOAD BERKALA
void visionLiveBegin() {
    Serial.println("TARS: ON-DEMAND VISION READY");
}

// PAUSE: KUNCI SIKLUS, BELUM MEMATIKAN KAMERA
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

// RESUME: DIPANGGIL SETELAH TTS DAN AUDIO SELESAI
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

    Serial.printf(
        "TARS: VISION CYCLE END HEAP=%u LARGEST=%u\n",
        ESP.getFreeHeap(),
        ESP.getMaxAllocHeap()
    );
}

// ASK: CAPTURE -> CAMERA OFF -> TLS STREAMING
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

    if (!captureFrame(jpegLength))
        return "";

    if (!stopCameraForTLS()) {
        Serial.println("TARS: VISION CAMERA STOP FAILED");
        return "";
    }

    String answer = sendVisionJSON(question, jpegLength);

    memset(visionJpeg, 0, jpegLength);
    memset(base64Buffer, 0, sizeof(base64Buffer));

    return answer;
}
