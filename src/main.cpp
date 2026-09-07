#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>

#include "BluetoothA2DPSource.h"
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"

#include "config.h"

// ============================================================
// TARS CONFIG
// ============================================================

static const char *ASK_URL =
    "https://tars-cloud-v1.hilmane34.workers.dev/ask";

static const char *TTS_URL =
    "https://tars-cloud-v1.hilmane34.workers.dev/tts";

static const char *MP3_FILE = "/tars.mp3";

static const uint32_t WIFI_TIMEOUT_MS = 15000;
static const uint32_t BT_TIMEOUT_MS   = 20000;

// MP3 decoder queue.
// Jangan terlalu besar karena ESP32 tanpa PSRAM.
static const size_t MP3_QUEUE_SIZE = 8192;

// ============================================================
// OLED
// ============================================================

Adafruit_SSD1306 display(
    OLED_WIDTH,
    OLED_HEIGHT,
    &Wire,
    -1
);

// ============================================================
// AUDIO
// ============================================================

BluetoothA2DPSource a2dp_source;

File mp3File;

MP3DecoderHelix mp3Decoder;

EncodedAudioStream decoder(
    &mp3File,
    &mp3Decoder
);

bool btStarted = false;
bool decoderStarted = false;
bool audioFinished = false;

// ============================================================
// OLED HELPERS
// ============================================================

void oledClear()
{
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.display();
}

void oledText(const String &text)
{
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);

    int x = 0;
    int y = 0;

    String line;

    for (size_t i = 0; i < text.length(); i++)
    {
        char c = text[i];

        if (c == '\n')
        {
            display.setCursor(x, y);
            display.println(line);

            line = "";
            x = 0;
            y += 8;

            if (y >= OLED_HEIGHT)
                break;

            continue;
        }

        line += c;

        if (line.length() >= 21)
        {
            display.setCursor(x, y);
            display.println(line);

            line = "";
            x = 0;
            y += 8;

            if (y >= OLED_HEIGHT)
                break;
        }
    }

    if (y < OLED_HEIGHT)
    {
        display.setCursor(x, y);
        display.println(line);
    }

    display.display();
}

// ============================================================
// OLED TYPING EFFECT
// ============================================================

void oledTyping(const String &text)
{
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);

    String line;
    int y = 0;

    for (size_t i = 0; i < text.length(); i++)
    {
        char c = text[i];

        if (c == '\n' || line.length() >= 21)
        {
            display.setCursor(0, y);
            display.println(line);
            display.display();

            line = "";
            y += 8;

            if (y >= OLED_HEIGHT)
                break;

            if (c == '\n')
                continue;
        }

        line += c;

        display.fillRect(0, y, 128, 8, SSD1306_BLACK);
        display.setCursor(0, y);
        display.print(line);
        display.display();

        delay(25);
    }

    if (y < OLED_HEIGHT)
    {
        display.setCursor(0, y);
        display.print(line);
        display.display();
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
    Serial.println("TARS: WiFi ON");

    oledText("TARS\nWiFi menghubungkan...");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t start = millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(250);
        Serial.print(".");

        if (millis() - start >= WIFI_TIMEOUT_MS)
        {
            Serial.println();
            Serial.println("TARS: WiFi timeout");
            oledText("WiFi gagal");
            return false;
        }
    }

    Serial.println();
    Serial.print("TARS: WiFi OK IP=");
    Serial.println(WiFi.localIP());

    return true;
}

void wifiOff()
{
    Serial.println("TARS: WiFi OFF");

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    delay(100);
}

// ============================================================
// ASK CLOUD
// ============================================================

bool askTars(
    const String &question,
    String &answer
)
{
    if (!connectWiFi())
        return false;

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;

    Serial.println("TARS: POST /ask");

    if (!http.begin(client, ASK_URL))
    {
        Serial.println("TARS: HTTP begin gagal");
        return false;
    }

    http.addHeader(
        "Content-Type",
        "application/json"
    );

    JsonDocument request;

    request["text"] = question;

    String body;

    serializeJson(request, body);

    int code = http.POST(body);

    Serial.print("TARS: ASK HTTP=");
    Serial.println(code);

    if (code != HTTP_CODE_OK)
    {
        Serial.println(http.getString());
        http.end();
        return false;
    }

    String response = http.getString();

    http.end();

    JsonDocument json;

    DeserializationError err =
        deserializeJson(json, response);

    if (err)
    {
        Serial.print("TARS: JSON error: ");
        Serial.println(err.c_str());
        return false;
    }

    const char *result =
        json["response"] | "";

    if (!result || result[0] == '\0')
    {
        Serial.println("TARS: response kosong");
        return false;
    }

    answer = String(result);

    Serial.println("TARS ANSWER:");
    Serial.println(answer);

    return true;
}

// ============================================================
// DOWNLOAD TTS MP3
// ============================================================

bool downloadTTS(
    const String &text
)
{
    if (!connectWiFi())
        return false;

    if (LittleFS.exists(MP3_FILE))
        LittleFS.remove(MP3_FILE);

    File file =
        LittleFS.open(
            MP3_FILE,
            FILE_WRITE
        );

    if (!file)
    {
        Serial.println("TARS: gagal membuka MP3");
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;

    Serial.println("TARS: POST /tts");

    if (!http.begin(client, TTS_URL))
    {
        file.close();
        return false;
    }

    http.addHeader(
        "Content-Type",
        "application/json"
    );

    JsonDocument request;

    request["text"] = text;

    String body;

    serializeJson(request, body);

    int code = http.POST(body);

    Serial.print("TARS: TTS HTTP=");
    Serial.println(code);

    if (code != HTTP_CODE_OK)
    {
        Serial.println(http.getString());

        http.end();
        file.close();

        LittleFS.remove(MP3_FILE);

        return false;
    }

    WiFiClient *stream =
        http.getStreamPtr();

    uint8_t buffer[1024];

    int remaining =
        http.getSize();

    uint32_t lastData = millis();

    while (http.connected())
    {
        size_t available =
            stream->available();

        if (available)
        {
            size_t toRead =
                available;

            if (toRead > sizeof(buffer))
                toRead = sizeof(buffer);

            int read =
                stream->readBytes(
                    buffer,
                    toRead
                );

            if (read > 0)
            {
                file.write(
                    buffer,
                    read
                );

                lastData = millis();

                if (remaining > 0)
                    remaining -= read;
            }
        }
        else
        {
            delay(1);

            if (remaining == 0)
                break;

            if (millis() - lastData > 5000)
                break;
        }
    }

    file.flush();
    file.close();

    http.end();

    File check =
        LittleFS.open(
            MP3_FILE,
            FILE_READ
        );

    if (!check)
    {
        LittleFS.remove(MP3_FILE);
        return false;
    }

    size_t size =
        check.size();

    check.close();

    Serial.print("TARS: MP3 size=");
    Serial.println(size);

    if (size < 512)
    {
        Serial.println("TARS: MP3 terlalu kecil");

        LittleFS.remove(MP3_FILE);

        return false;
    }

    return true;
}

// ============================================================
// A2DP PCM CALLBACK
//
// MP3DecoderHelix:
// MP3 -> PCM 44.1 kHz / stereo / 16 bit
//
// ESP32-A2DP:
// callback juga mengharapkan PCM.
// ============================================================

int32_t getAudioData(
    uint8_t *data,
    int32_t byteCount
)
{
    if (!decoderStarted)
    {
        memset(
            data,
            0,
            byteCount
        );

        return byteCount;
    }

    int32_t result =
        decoder.readBytes(
            data,
            byteCount
        );

    if (result <= 0)
    {
        memset(
            data,
            0,
            byteCount
        );

        audioFinished = true;

        return byteCount;
    }

    return result;
}

// ============================================================
// START BLUETOOTH
// ============================================================

bool startBluetooth()
{
    Serial.println("TARS: Bluetooth ON");

    oledText(
        "TARS\nBluetooth ON\nMencari I7-TWS..."
    );

    btStarted = false;
    audioFinished = false;

    a2dp_source.set_auto_reconnect(
        false
    );

    a2dp_source.set_data_callback(
        getAudioData
    );

    a2dp_source.set_volume(100);

    a2dp_source.start(
        BT_HEADSET_NAME
    );

    btStarted = true;

    Serial.println(
        "TARS: A2DP start"
    );

    uint32_t start =
        millis();

    while (!a2dp_source.is_connected())
    {
        delay(100);

        if (millis() - start >= BT_TIMEOUT_MS)
        {
            Serial.println(
                "TARS: Bluetooth timeout"
            );

            return false;
        }
    }

    Serial.println(
        "TARS: I7-TWS CONNECTED"
    );

    oledText(
        "TARS\nI7-TWS connected"
    );

    return true;
}

// ============================================================
// START DECODER
// ============================================================

bool startDecoder()
{
    if (!mp3File)
    {
        mp3File =
            LittleFS.open(
                MP3_FILE,
                FILE_READ
            );
    }

    if (!mp3File)
    {
        Serial.println(
            "TARS: MP3 open gagal"
        );

        return false;
    }

    /*
     * AudioTools/Helix memakai queue internal.
     * 8 KB cukup untuk menjaga aliran PCM tanpa
     * menghabiskan RAM ESP32 terlalu besar.
     */
    decoder
        .transformationReader()
        .resizeResultQueue(
            MP3_QUEUE_SIZE
        );

    if (!decoder.begin())
    {
        Serial.println(
            "TARS: MP3 decoder gagal"
        );

        mp3File.close();

        return false;
    }

    decoderStarted = true;
    audioFinished = false;

    Serial.println(
        "TARS: MP3 decoder READY"
    );

    return true;
}

// ============================================================
// STOP DECODER
// ============================================================

void stopDecoder()
{
    decoderStarted = false;

    delay(20);

    if (mp3File)
        mp3File.close();

    Serial.println(
        "TARS: MP3 decoder STOP"
    );
}

// ============================================================
// STOP BLUETOOTH
// ============================================================

void stopBluetooth()
{
    if (!btStarted)
        return;

    Serial.println(
        "TARS: Bluetooth OFF"
    );

    a2dp_source.end(true);

    btStarted = false;

    delay(200);

    Serial.println(
        "TARS: Bluetooth released"
    );
}

// ============================================================
// PLAY MP3
// ============================================================

bool playMP3()
{
    if (!startDecoder())
        return false;

    if (!startBluetooth())
    {
        stopDecoder();
        return false;
    }

    Serial.println(
        "TARS: PLAYING"
    );

    oledText(
        "TARS\nBerbicara..."
    );

    /*
     * A2DP callback berjalan di task Bluetooth.
     * Selama callback masih meminta PCM, decoder
     * akan membaca MP3 dan mengubahnya menjadi PCM.
     *
     * Kita hanya menunggu sampai decoder mencapai EOF.
     */
    uint32_t start =
        millis();

    while (!audioFinished)
    {
        delay(10);

        /*
         * Safety timeout:
         * jangan biarkan TARS menggantung selamanya
         * jika file/decoder bermasalah.
         */
        if (millis() - start > 120000UL)
        {
            Serial.println(
                "TARS: playback timeout"
            );

            break;
        }
    }

    /*
     * Beri A2DP sedikit waktu untuk mengirim
     * buffer PCM terakhir.
     */
    delay(300);

    stopDecoder();

    stopBluetooth();

    return true;
}

// ============================================================
// TARS READY
// ============================================================

void showReady()
{
    oledText(
        "TARS\nSiap menunggu..."
    );

    Serial.println();
    Serial.println(
        "================================"
    );
    Serial.println(
        "TARS READY"
    );
    Serial.println(
        "Ketik pertanyaan di Serial"
    );
    Serial.println(
        "================================"
    );
}

// ============================================================
// PROCESS QUESTION
// ============================================================

void processQuestion(
    const String &question
)
{
    if (question.length() == 0)
        return;

    Serial.println();
    Serial.println(
        "TARS: Pertanyaan:"
    );
    Serial.println(question);

    oledText(
        "TARS\nBerpikir..."
    );

    // --------------------------------------------------------
    // 1. ASK AI
    // --------------------------------------------------------

    String answer;

    if (!askTars(
            question,
            answer))
    {
        oledText(
            "TARS\nGagal menghubungi Cloud"
        );

        connectWiFi();
        return;
    }

    // --------------------------------------------------------
    // 2. DISPLAY ANSWER
    // --------------------------------------------------------

    oledTyping(answer);

    // --------------------------------------------------------
    // 3. DOWNLOAD TTS
    // --------------------------------------------------------

    oledText(
        "TARS\nMenyiapkan suara..."
    );

    if (!downloadTTS(answer))
    {
        Serial.println(
            "TARS: TTS gagal"
        );

        oledText(
            "TARS\nTTS gagal"
        );

        return;
    }

    // --------------------------------------------------------
    // 4. WIFI OFF
    // --------------------------------------------------------

    wifiOff();

    // --------------------------------------------------------
    // 5. MP3 -> A2DP
    // --------------------------------------------------------

    playMP3();

    // --------------------------------------------------------
    // 6. MP3 DELETE
    // --------------------------------------------------------

    if (LittleFS.exists(MP3_FILE))
        LittleFS.remove(MP3_FILE);

    // --------------------------------------------------------
    // 7. WIFI ON AGAIN
    // --------------------------------------------------------

    connectWiFi();

    showReady();
}

// ============================================================
// SERIAL INPUT
// ============================================================

String serialBuffer;

void handleSerial()
{
    while (Serial.available())
    {
        char c =
            Serial.read();

        if (c == '\r')
            continue;

        if (c == '\n')
        {
            if (serialBuffer.length() > 0)
            {
                String question =
                    serialBuffer;

                serialBuffer = "";

                processQuestion(
                    question
                );
            }
        }
        else
        {
            if (serialBuffer.length() < 300)
                serialBuffer += c;
        }
    }
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(115200);

    delay(1000);

    Serial.println();
    Serial.println(
        "================================"
    );
    Serial.println(
        "       TARS ESP32 START"
    );
    Serial.println(
        "================================"
    );

    // --------------------------------------------------------
    // OLED
    // --------------------------------------------------------

    Wire.begin(
        OLED_SDA,
        OLED_SCL
    );

    if (!display.begin(
            SSD1306_SWITCHCAPVCC,
            OLED_ADDR))
    {
        Serial.println(
            "OLED gagal"
        );
    }
    else
    {
        oledText(
            "TARS\nBooting..."
        );
    }

    // --------------------------------------------------------
    // LITTLEFS
    // --------------------------------------------------------

    if (!LittleFS.begin(true))
    {
        Serial.println(
            "TARS: LittleFS gagal"
        );

        oledText(
            "TARS\nLittleFS gagal"
        );

        while (true)
            delay(1000);
    }

    Serial.println(
        "TARS: LittleFS OK"
    );

    // --------------------------------------------------------
    // WIFI
    // --------------------------------------------------------

    connectWiFi();

    // --------------------------------------------------------
    // READY
    // --------------------------------------------------------

    showReady();
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
    handleSerial();

    delay(5);
}
