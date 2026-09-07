#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Wire.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "AudioLibs/A2DPStream.h"

#include "config.h"

// ============================================================
// OLED
// ============================================================

Adafruit_SSD1306 oled(
    OLED_WIDTH,
    OLED_HEIGHT,
    &Wire,
    -1
);

// ============================================================
// AUDIO
// ============================================================

A2DPStream a2dp;
MP3DecoderHelix mp3Decoder;
EncodedAudioStream decoder(&a2dp, &mp3Decoder);

File mp3File;

bool audioPlaying = false;
bool btStarted = false;

// ============================================================
// OLED HELPERS
// ============================================================

void oledClear()
{
    oled.clearDisplay();
    oled.display();
}

void oledText(const String &text)
{
    oled.clearDisplay();

    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(0, 0);

    int start = 0;

    while (start < text.length())
    {
        int end = text.indexOf('\n', start);

        if (end < 0)
            end = text.length();

        String line = text.substring(start, end);

        oled.println(line);

        start = end + 1;
    }

    oled.display();
}

void oledTyping(const String &text)
{
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);

    String line = "";
    int y = 0;

    for (size_t i = 0; i < text.length(); i++)
    {
        char c = text[i];

        if (c == '\n' || line.length() >= 20)
        {
            oled.setCursor(0, y);
            oled.print(line);

            line = "";
            y += 10;

            if (y >= 64)
                break;

            if (c == '\n')
                continue;
        }

        line += c;

        oled.setCursor(0, y);
        oled.print(line);

        oled.display();

        delay(18);
    }

    if (line.length() && y < 64)
    {
        oled.setCursor(0, y);
        oled.print(line);
        oled.display();
    }
}

// ============================================================
// WIFI
// ============================================================

bool connectWiFi()
{
    if (WiFi.status() == WL_CONNECTED)
        return true;

    Serial.println();
    Serial.println("[WIFI] START");

    oledText("TARS\nWiFi...");

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(250);

        Serial.print(".");

        if (millis() - start > 20000)
        {
            Serial.println();
            Serial.println("[WIFI] TIMEOUT");

            oledText("WiFi gagal");

            return false;
        }
    }

    Serial.println();
    Serial.println("[WIFI] CONNECTED");
    Serial.println(WiFi.localIP());

    oledText("WiFi OK");

    return true;
}

// ============================================================
// WIFI OFF
// ============================================================

void wifiOff()
{
    Serial.println("[WIFI] OFF");

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    delay(200);
}

// ============================================================
// HTTPS POST JSON
// ============================================================

bool httpsPostJson(
    const String &url,
    const String &json,
    String &response
)
{
    WiFiClientSecure client;

    // TARS Cloud uses HTTPS.
    // We intentionally bypass CA verification here because
    // the previous MicroPython firmware failed at x509 bundle
    // verification on the ESP32.
    client.setInsecure();

    HTTPClient http;

    if (!http.begin(client, url))
    {
        Serial.println("[HTTP] BEGIN FAILED");
        return false;
    }

    http.setTimeout(30000);

    http.addHeader(
        "Content-Type",
        "application/json"
    );

    int code = http.POST(json);

    Serial.print("[HTTP] CODE: ");
    Serial.println(code);

    if (code <= 0)
    {
        Serial.print("[HTTP] ERROR: ");
        Serial.println(http.errorToString(code));

        http.end();
        return false;
    }

    response = http.getString();

    http.end();

    return code >= 200 && code < 300;
}

// ============================================================
// ASK TARS CLOUD
// ============================================================

bool askTARS(
    const String &question,
    String &answer
)
{
    Serial.println();
    Serial.println("==============================");
    Serial.println("[AI] ASK");
    Serial.println(question);
    Serial.println("==============================");

    JsonDocument request;

    request["text"] = question;

    String body;

    serializeJson(request, body);

    String response;

    String url =
        String(TARS_CLOUD_URL) +
        "/ask";

    if (!httpsPostJson(
        url,
        body,
        response
    ))
    {
        Serial.println("[AI] REQUEST FAILED");
        return false;
    }

    JsonDocument json;

    DeserializationError error =
        deserializeJson(json, response);

    if (error)
    {
        Serial.print("[AI] JSON ERROR: ");
        Serial.println(error.c_str());

        Serial.println(response);

        return false;
    }

    const char *result =
        json["response"];

    if (!result)
    {
        Serial.println("[AI] NO RESPONSE");
        return false;
    }

    answer = String(result);

    Serial.println();
    Serial.println("[AI] ANSWER");
    Serial.println(answer);

    return answer.length() > 0;
}

// ============================================================
// DOWNLOAD TTS MP3
// ============================================================

bool downloadTTS(
    const String &text
)
{
    Serial.println();
    Serial.println("==============================");
    Serial.println("[TTS] DOWNLOAD MP3");
    Serial.println("==============================");

    if (!LittleFS.begin(true))
    {
        Serial.println("[FS] LITTLEFS FAILED");
        return false;
    }

    if (LittleFS.exists(MP3_FILE))
    {
        LittleFS.remove(MP3_FILE);
    }

    File file =
        LittleFS.open(
            MP3_FILE,
            FILE_WRITE
        );

    if (!file)
    {
        Serial.println("[FS] FILE OPEN FAILED");
        return false;
    }

    WiFiClientSecure client;

    // See comment in httpsPostJson().
    client.setInsecure();

    HTTPClient http;

    String url =
        String(TARS_CLOUD_URL) +
        "/tts";

    if (!http.begin(client, url))
    {
        Serial.println("[TTS] HTTP BEGIN FAILED");

        file.close();

        return false;
    }

    http.setTimeout(60000);

    http.addHeader(
        "Content-Type",
        "application/json"
    );

    http.addHeader(
        "Accept",
        "audio/mpeg"
    );

    JsonDocument request;

    request["text"] = text;

    String body;

    serializeJson(request, body);

    int code =
        http.POST(body);

    Serial.print("[TTS] HTTP: ");
    Serial.println(code);

    if (code != 200)
    {
        Serial.println(
            http.getString()
        );

        http.end();

        file.close();

        return false;
    }

    int total =
        http.getSize();

    Serial.print("[TTS] SIZE: ");
    Serial.println(total);

    WiFiClient *stream =
        http.getStreamPtr();

    uint8_t buffer[2048];

    size_t received = 0;

    unsigned long lastData =
        millis();

    while (
        http.connected() ||
        stream->available()
    )
    {
        size_t available =
            stream->available();

        if (available)
        {
            int len =
                stream->readBytes(
                    buffer,
                    min(
                        available,
                        sizeof(buffer)
                    )
                );

            if (len > 0)
            {
                file.write(
                    buffer,
                    len
                );

                received += len;

                lastData =
                    millis();
            }
        }
        else
        {
            delay(5);

            if (
                millis() - lastData >
                10000
            )
            {
                break;
            }
        }
    }

    file.close();

    http.end();

    Serial.print(
        "[TTS] RECEIVED: "
    );

    Serial.println(received);

    if (received < 100)
    {
        LittleFS.remove(MP3_FILE);

        Serial.println(
            "[TTS] MP3 INVALID"
        );

        return false;
    }

    Serial.println(
        "[TTS] MP3 SAVED"
    );

    return true;
}

// ============================================================
// START BLUETOOTH
// ============================================================

bool startBluetooth()
{
    Serial.println();
    Serial.println("==============================");
    Serial.println("[BT] START");
    Serial.println("==============================");

    oledText("Bluetooth\nmencari...");

    auto cfg =
        a2dp.defaultConfig(TX_MODE);

    cfg.name =
        BT_HEADSET_NAME;

    cfg.auto_reconnect = false;

    a2dp.begin(cfg);

    btStarted = true;

    Serial.print(
        "[BT] CONNECTING: "
    );

    Serial.println(
        BT_HEADSET_NAME
    );

    unsigned long start =
        millis();

    while (!a2dp)
    {
        delay(100);

        if (
            millis() - start >
            BT_CONNECT_TIMEOUT
        )
        {
            Serial.println(
                "[BT] TIMEOUT"
            );

            return false;
        }
    }

    Serial.println(
        "[BT] CONNECTED"
    );

    oledText(
        "Bluetooth\nterhubung"
    );

    return true;
}

// ============================================================
// STOP BLUETOOTH
// ============================================================

void stopBluetooth()
{
    if (!btStarted)
        return;

    Serial.println(
        "[BT] STOP"
    );

    a2dp.end(true);

    btStarted = false;

    delay(300);
}

// ============================================================
// PLAY MP3
// ============================================================

bool playMP3()
{
    Serial.println();
    Serial.println("==============================");
    Serial.println("[AUDIO] PLAY MP3");
    Serial.println("==============================");

    mp3File =
        LittleFS.open(
            MP3_FILE,
            FILE_READ
        );

    if (!mp3File)
    {
        Serial.println(
            "[AUDIO] FILE OPEN FAILED"
        );

        return false;
    }

    size_t fileSize =
        mp3File.size();

    Serial.print(
        "[AUDIO] FILE SIZE: "
    );

    Serial.println(fileSize);

    if (fileSize < 100)
    {
        mp3File.close();

        return false;
    }

    oledText(
        "TARS\nberbicara..."
    );

    /*
     * AudioTools chain:
     *
     * LittleFS MP3
     *      ↓
     * StreamCopy
     *      ↓
     * MP3DecoderHelix
     *      ↓
     * A2DPStream
     *      ↓
     * Bluetooth headset
     *
     * AudioTools' EncodedAudioStream
     * handles the MP3 → PCM stage.
     */

    decoder.begin();

    StreamCopy copier(
        decoder,
        mp3File
    );

    unsigned long lastData =
        millis();

    while (mp3File.available())
    {
        size_t copied =
            copier.copy();

        if (copied > 0)
        {
            lastData =
                millis();
        }
        else
        {
            delay(1);

            if (
                millis() - lastData >
                5000
            )
            {
                Serial.println(
                    "[AUDIO] STREAM TIMEOUT"
                );

                break;
            }
        }

        yield();
    }

    delay(300);

    copier.end();

    decoder.end();

    mp3File.close();

    Serial.println(
        "[AUDIO] PLAY FINISHED"
    );

    return true;
}

// ============================================================
// FULL TARS CYCLE
// ============================================================

void runTARSCycle(
    const String &question
)
{
    Serial.println();
    Serial.println(
        "################################"
    );

    Serial.println(
        "# START TARS CYCLE"
    );

    Serial.print(
        "# QUESTION: "
    );

    Serial.println(
        question
    );

    Serial.println(
        "################################"
    );

    oledText(
        "TARS\nberpikir..."
    );

    // --------------------------------
    // WIFI
    // --------------------------------

    if (!connectWiFi())
    {
        oledText(
            "WiFi gagal"
        );

        return;
    }

    // --------------------------------
    // ASK AI
    // --------------------------------

    String answer;

    if (!askTARS(
        question,
        answer
    ))
    {
        oledText(
            "TARS\nAI error"
        );

        return;
    }

    oledTyping(answer);

    // --------------------------------
    // TTS MP3
    // --------------------------------

    if (!downloadTTS(answer))
    {
        oledText(
            "TARS\nTTS error"
        );

        return;
    }

    // --------------------------------
    // WIFI OFF
    // --------------------------------

    wifiOff();

    // --------------------------------
    // BLUETOOTH
    // --------------------------------

    if (!startBluetooth())
    {
        stopBluetooth();

        LittleFS.remove(
            MP3_FILE
        );

        connectWiFi();

        return;
    }

    // --------------------------------
    // PLAY
    // --------------------------------

    playMP3();

    // --------------------------------
    // BT OFF
    // --------------------------------

    stopBluetooth();

    // --------------------------------
    // DELETE MP3
    // --------------------------------

    LittleFS.remove(
        MP3_FILE
    );

    // --------------------------------
    // WIFI ON AGAIN
    // --------------------------------

    connectWiFi();

    oledText(
        "TARS\nmenunggu..."
    );

    Serial.println();
    Serial.println(
        "################################"
    );

    Serial.println(
        "# TARS READY"
    );

    Serial.println(
        "################################"
    );
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(
        SERIAL_BAUD
    );

    delay(500);

    Serial.println();
    Serial.println(
        "===================================="
    );

    Serial.println(
        "TARS ESP32 FINAL"
    );

    Serial.println(
        "AI + MP3 + A2DP + OLED"
    );

    Serial.println(
        "===================================="
    );

    // --------------------------------
    // OLED
    // --------------------------------

    Wire.begin(
        OLED_SDA,
        OLED_SCL
    );

    if (
        !oled.begin(
            SSD1306_SWITCHCAPVCC,
            OLED_ADDR
        )
    )
    {
        Serial.println(
            "[OLED] FAILED"
        );
    }
    else
    {
        oledText(
            "TARS\nBOOT"
        );
    }

    // --------------------------------
    // FILESYSTEM
    // --------------------------------

    if (!LittleFS.begin(true))
    {
        Serial.println(
            "[FS] FAILED"
        );

        oledText(
            "LittleFS\nERROR"
        );
    }
    else
    {
        Serial.println(
            "[FS] OK"
        );
    }

    // --------------------------------
    // RAM
    // --------------------------------

    Serial.print(
        "[RAM] FREE HEAP: "
    );

    Serial.println(
        ESP.getFreeHeap()
    );

    // --------------------------------
    // WIFI
    // --------------------------------

    connectWiFi();

    oledText(
        "TARS\nREADY"
    );

    Serial.println();
    Serial.println(
        "===================================="
    );

    Serial.println(
        "Ketik pertanyaan di Serial Monitor"
    );

    Serial.println(
        "===================================="
    );
}

// ============================================================
// LOOP
// ============================================================

String serialQuestion;

void loop()
{
    while (Serial.available())
    {
        char c =
            Serial.read();

        if (
            c == '\n' ||
            c == '\r'
        )
        {
            if (
                serialQuestion.length() >
                0
            )
            {
                String question =
                    serialQuestion;

                serialQuestion = "";

                Serial.println();

                Serial.println(
                    "[USER]"
                );

                Serial.println(
                    question
                );

                runTARSCycle(
                    question
                );
            }
        }
        else
        {
            if (
                serialQuestion.length() <
                300
            )
            {
                serialQuestion += c;
            }
        }
    }

    delay(5);
}
