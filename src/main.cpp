#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Wire.h>
#include <time.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>

#include "BluetoothA2DPSource.h"
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "config.h"

// ============================================================
// TARS CLOUD
// ============================================================

static const char *TARS_ASK_URL =
    "https://tars-cloud-v1.hilmane34.workers.dev/ask";

static const char *TARS_TTS_URL =
    "https://tars-cloud-v1.hilmane34.workers.dev/tts";

#ifndef MP3_FILE
#define MP3_FILE "/tars.mp3"
#endif

// ============================================================
// TIMING
// ============================================================

static const uint32_t WIFI_TIMEOUT_MS = 15000;
static const uint32_t BT_TIMEOUT_MS = 20000;
static const uint32_t PLAY_TIMEOUT_MS = 120000;

static const uint32_t OLED_REFRESH_MS = 80;
static const uint32_t TEXT_SPEED_MS = 35;

static const size_t TTS_MAX_CHARS = 450;

// ============================================================
// AUDIO
// ============================================================

static const int32_t PCM_GAIN = 2;

static inline int16_t boostPCM(int16_t sample)
{
    int32_t value = (int32_t)sample * PCM_GAIN;

    if (value > 32767)
        value = 32767;

    if (value < -32768)
        value = -32768;

    return (int16_t)value;
}

// ============================================================
// NTP
// ============================================================

static bool ntpSynced = false;
static const time_t VALID_EPOCH = 1704067200;

static bool isTimeValid()
{
    time_t now;
    time(&now);
    return now >= VALID_EPOCH;
}

static bool syncNTPOnce()
{
    if (ntpSynced && isTimeValid())
        return true;

    Serial.println();
    Serial.println("TARS: NTP START");

    configTime(
        7 * 3600,
        0,
        "pool.ntp.org",
        "time.nist.gov",
        "time.google.com"
    );

    for (int attempt = 1; attempt <= 4; attempt++)
    {
        Serial.printf(
            "TARS: NTP attempt %d/4\n",
            attempt
        );

        delay(1000);

        if (isTimeValid())
        {
            time_t now;
            time(&now);

            struct tm info;
            localtime_r(&now, &info);

            Serial.printf(
                "TARS: NTP OK %04d-%02d-%02d %02d:%02d:%02d\n",
                info.tm_year + 1900,
                info.tm_mon + 1,
                info.tm_mday + 0,
                info.tm_hour,
                info.tm_min,
                info.tm_sec
            );

            ntpSynced = true;
            return true;
        }
    }

    Serial.println("TARS: NTP FAILED");
    return false;
}

// ============================================================
// PCM BUFFER
// ============================================================

// Output akhir selalu:
// 44100 Hz
// stereo
// 16-bit

static const size_t PCM_BUFFER_SIZE = 16384;
static const size_t PCM_PRIME_BYTES = 8192;

static uint8_t pcmBuffer[PCM_BUFFER_SIZE];

static volatile size_t pcmReadPos = 0;
static volatile size_t pcmWritePos = 0;
static volatile size_t pcmUsed = 0;

static portMUX_TYPE pcmMux =
    portMUX_INITIALIZER_UNLOCKED;

// ============================================================
// OBJECTS
// ============================================================

Adafruit_SSD1306 oled(
    OLED_WIDTH,
    OLED_HEIGHT,
    &Wire,
    -1
);

BluetoothA2DPSource a2dpSource;

File mp3File;

MP3DecoderHelix mp3Decoder;

// ============================================================
// STATE
// ============================================================

enum TarsState
{
    TARS_BOOT,
    TARS_WAITING,
    TARS_THINKING,
    TARS_PREPARING_AUDIO,
    TARS_SPEAKING,
    TARS_ERROR
};

static volatile TarsState tarsState =
    TARS_BOOT;

static volatile bool wifiIsOn = false;
static volatile bool btIsOn = false;
static volatile bool workerBusy = false;

// ============================================================
// QUESTION QUEUE
// ============================================================

struct QuestionMessage
{
    char text[256];
};

static QueueHandle_t questionQueue = nullptr;

static char serialLine[256];
static size_t serialLineLength = 0;

// ============================================================
// OLED TEXT
// ============================================================

static String speechText;

static volatile size_t speechVisibleChars = 0;

static uint32_t speechLastUpdate = 0;
static uint32_t oledLastRefresh = 0;
static uint32_t panelAnimation = 0;

// ============================================================
// AUDIO STATE
// ============================================================

static bool audioDecoderReady = false;
static bool mp3InputFinished = false;
static bool playbackRunning = false;

// ============================================================
// PCM OUTPUT STREAM
// ============================================================

class PCMOutputStream : public AudioStream
{
public:

    AudioInfo sourceInfo;

    bool begin()
    {
        return true;
    }

    void end()
    {
    }

    void setAudioInfo(AudioInfo info) override
    {
        AudioStream::setAudioInfo(info);

        sourceInfo = info;

        Serial.printf(
            "PCM format: %lu Hz, %d ch, %d bit\n",
            (unsigned long)info.sample_rate,
            info.channels,
            info.bits_per_sample
        );
    }

    int availableForWrite() override
    {
        portENTER_CRITICAL(&pcmMux);

        size_t freeBytes =
            PCM_BUFFER_SIZE - pcmUsed;

        portEXIT_CRITICAL(&pcmMux);

        return (int)freeBytes;
    }

    size_t write(
        const uint8_t *data,
        size_t size
    ) override
    {
        if (data == nullptr || size == 0)
            return 0;

        if (sourceInfo.bits_per_sample != 16)
            return 0;

        uint32_t rate =
            sourceInfo.sample_rate;

        uint8_t channels =
            sourceInfo.channels;

        const int16_t *samples =
            (const int16_t *)data;

        size_t sampleCount =
            size / sizeof(int16_t);

        size_t consumedSamples = 0;

        // ====================================================
        // 44100 STEREO
        // ====================================================

        if (rate == 44100 && channels == 2)
        {
            for (
                size_t i = 0;
                i + 1 < sampleCount;
                i += 2
            )
            {
                int16_t left =
                    boostPCM(samples[i]);

                int16_t right =
                    boostPCM(samples[i + 1]);

                uint8_t out[4];

                memcpy(out, &left, 2);
                memcpy(out + 2, &right, 2);

                if (writeRaw(out, 4) != 4)
                    break;

                consumedSamples += 2;
            }

            return consumedSamples * 2;
        }

        // ====================================================
        // 44100 MONO -> STEREO
        // ====================================================

        if (rate == 44100 && channels == 1)
        {
            for (
                size_t i = 0;
                i < sampleCount;
                i++
            )
            {
                int16_t s =
                    boostPCM(samples[i]);

                uint8_t out[4];

                memcpy(out, &s, 2);
                memcpy(out + 2, &s, 2);

                if (writeRaw(out, 4) != 4)
                    break;

                consumedSamples++;
            }

            return consumedSamples * 2;
        }

        // ====================================================
        // 22050 MONO -> 44100 STEREO
        // ====================================================

        if (rate == 22050 && channels == 1)
        {
            for (
                size_t i = 0;
                i < sampleCount;
                i++
            )
            {
                int16_t s =
                    boostPCM(samples[i]);

                uint8_t out[8];

                // Frame 1 stereo
                memcpy(out, &s, 2);
                memcpy(out + 2, &s, 2);

                // Frame 2 duplicate
                memcpy(out + 4, &s, 2);
                memcpy(out + 6, &s, 2);

                if (writeRaw(out, 8) != 8)
                    break;

                consumedSamples++;
            }

            return consumedSamples * 2;
        }

        Serial.printf(
            "TARS: UNSUPPORTED PCM %lu Hz / %d ch\n",
            (unsigned long)rate,
            channels
        );

        return 0;
    }

    size_t readPCM(
        uint8_t *data,
        size_t size
    )
    {
        if (!data || size == 0)
            return 0;

        portENTER_CRITICAL(&pcmMux);

        size_t amount =
            size;

        if (amount > pcmUsed)
            amount = pcmUsed;

        if (amount > 0)
        {
            size_t first =
                PCM_BUFFER_SIZE - pcmReadPos;

            if (amount <= first)
            {
                memcpy(
                    data,
                    &pcmBuffer[pcmReadPos],
                    amount
                );
            }
            else
            {
                memcpy(
                    data,
                    &pcmBuffer[pcmReadPos],
                    first
                );

                memcpy(
                    data + first,
                    pcmBuffer,
                    amount - first
                );
            }

            pcmReadPos =
                (pcmReadPos + amount) %
                PCM_BUFFER_SIZE;

            pcmUsed -= amount;
        }

        portEXIT_CRITICAL(&pcmMux);

        return amount;
    }

    size_t availablePCM()
    {
        portENTER_CRITICAL(&pcmMux);

        size_t result = pcmUsed;

        portEXIT_CRITICAL(&pcmMux);

        return result;
    }

    void clearBuffer()
    {
        portENTER_CRITICAL(&pcmMux);

        pcmReadPos = 0;
        pcmWritePos = 0;
        pcmUsed = 0;

        portEXIT_CRITICAL(&pcmMux);
    }

private:

    // ========================================================
    // NON-BLOCKING WRITE
    //
    // INI FIX UTAMA.
    //
    // Tidak boleh menunggu selamanya jika PCM buffer penuh.
    // ========================================================

    size_t writeRaw(
        const uint8_t *data,
        size_t size
    )
    {
        if (!data || size == 0)
            return 0;

        portENTER_CRITICAL(&pcmMux);

        size_t freeBytes =
            PCM_BUFFER_SIZE - pcmUsed;

        if (freeBytes == 0)
        {
            portEXIT_CRITICAL(&pcmMux);
            return 0;
        }

        size_t amount =
            size;

        if (amount > freeBytes)
            amount = freeBytes;

        size_t first =
            PCM_BUFFER_SIZE - pcmWritePos;

        if (amount <= first)
        {
            memcpy(
                &pcmBuffer[pcmWritePos],
                data,
                amount
            );
        }
        else
        {
            memcpy(
                &pcmBuffer[pcmWritePos],
                data,
                first
            );

            memcpy(
                pcmBuffer,
                data + first,
                amount - first
            );
        }

        pcmWritePos =
            (pcmWritePos + amount) %
            PCM_BUFFER_SIZE;

        pcmUsed += amount;

        portEXIT_CRITICAL(&pcmMux);

        return amount;
    }
};

PCMOutputStream pcmOutput;

EncodedAudioStream decoder(
    &pcmOutput,
    &mp3Decoder
);

StreamCopy mp3Copier(
    decoder,
    mp3File
);

// ============================================================
// OLED
// ============================================================

static void drawPanelFrame()
{
    oled.drawLine(0, 11, 127, 11, SSD1306_WHITE);
    oled.drawLine(0, 53, 127, 53, SSD1306_WHITE);
}

static void drawHeader(
    const char *status
)
{
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    oled.setCursor(4, 2);
    oled.print("TARS");

    oled.setCursor(76, 2);
    oled.print(status);
}

static void drawConnectionStatus()
{
    oled.setCursor(4, 56);
    oled.print("W:");
    oled.print(wifiIsOn ? "ON" : "OFF");

    oled.setCursor(68, 56);
    oled.print("BT:");
    oled.print(btIsOn ? "ON" : "OFF");
}

static void drawWaitingPanel()
{
    drawPanelFrame();
    drawHeader("READY");

    oled.setCursor(6, 18);
    oled.print("SYSTEM READY");

    oled.setCursor(6, 29);
    oled.print("AWAITING COMMAND");

    int offset =
        (panelAnimation / 2) % 10;

    oled.drawRect(
        6, 41, 116, 6,
        SSD1306_WHITE
    );

    for (int i = 0; i < 10; i++)
    {
        if (i != offset)
        {
            oled.fillRect(
                9 + i * 11,
                43,
                7,
                2,
                SSD1306_WHITE
            );
        }
    }

    drawConnectionStatus();
}

static void drawThinkingPanel()
{
    drawPanelFrame();
    drawHeader("THINK");

    oled.setCursor(6, 18);
    oled.print("PROCESSING");

    oled.setCursor(6, 29);
    oled.print("ANALYZING INPUT");

    int active =
        (panelAnimation / 3) % 12;

    for (int i = 0; i < 12; i++)
    {
        int x = 6 + i * 10;

        if (i <= active)
            oled.fillRect(x, 42, 7, 5, SSD1306_WHITE);
        else
            oled.drawRect(x, 42, 7, 5, SSD1306_WHITE);
    }

    drawConnectionStatus();
}

static void drawPreparingPanel()
{
    drawPanelFrame();
    drawHeader("AUDIO");

    oled.setCursor(6, 18);
    oled.print("PREPARING VOICE");

    oled.setCursor(6, 29);
    oled.print("LOADING MP3");

    int active =
        (panelAnimation / 2) % 12;

    for (int i = 0; i < 12; i++)
    {
        int x = 6 + i * 10;

        if (i == active)
            oled.fillRect(x, 42, 7, 5, SSD1306_WHITE);
        else
            oled.drawRect(x, 42, 7, 5, SSD1306_WHITE);
    }

    drawConnectionStatus();
}

static void drawSpeakingPanel()
{
    drawPanelFrame();
    drawHeader("SPEAK");

    size_t visible =
        speechVisibleChars;

    size_t length =
        speechText.length();

    const size_t charsPerLine = 20;
    const size_t maxLines = 4;

    size_t startIndex = 0;

    if (visible > charsPerLine * maxLines)
        startIndex =
            visible - charsPerLine * maxLines;

    size_t pos = startIndex;

    for (
        size_t line = 0;
        line < maxLines;
        line++
    )
    {
        oled.setCursor(
            4,
            15 + line * 9
        );

        size_t count = 0;

        while (
            pos < visible &&
            pos < length &&
            count < charsPerLine
        )
        {
            char c =
                speechText[pos++];

            if (c == '\n')
                break;

            oled.print(c);
            count++;
        }
    }

    drawConnectionStatus();
}

static void drawErrorPanel()
{
    drawPanelFrame();
    drawHeader("ERROR");

    oled.setCursor(6, 20);
    oled.print("SYSTEM ERROR");

    oled.setCursor(6, 32);
    oled.print("CHECK CONNECTION");

    drawConnectionStatus();
}

static void updateOLED()
{
    uint32_t now = millis();

    if (
        now - oledLastRefresh <
        OLED_REFRESH_MS
    )
        return;

    oledLastRefresh = now;

    panelAnimation++;

    oled.clearDisplay();

    switch (tarsState)
    {
        case TARS_WAITING:
            drawWaitingPanel();
            break;

        case TARS_THINKING:
            drawThinkingPanel();
            break;

        case TARS_PREPARING_AUDIO:
            drawPreparingPanel();
            break;

        case TARS_SPEAKING:
            drawSpeakingPanel();
            break;

        case TARS_ERROR:
            drawErrorPanel();
            break;

        default:
            drawWaitingPanel();
            break;
    }

    if (
        tarsState == TARS_SPEAKING &&
        speechVisibleChars <
        speechText.length() &&
        now - speechLastUpdate >=
        TEXT_SPEED_MS
    )
    {
        speechLastUpdate = now;
        speechVisibleChars++;
    }

    oled.display();
}

// ============================================================
// WIFI
// ============================================================

bool connectWiFi()
{
    if (
        WiFi.status() ==
        WL_CONNECTED
    )
    {
        wifiIsOn = true;
        return true;
    }

    Serial.println("TARS: WiFi ON");

    WiFi.mode(WIFI_STA);

    WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
    );

    uint32_t start = millis();

    while (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        delay(250);

        if (
            millis() - start >=
            WIFI_TIMEOUT_MS
        )
        {
            wifiIsOn = false;

            Serial.println(
                "TARS: WiFi timeout"
            );

            return false;
        }
    }

    wifiIsOn = true;

    Serial.print("TARS: IP = ");
    Serial.println(WiFi.localIP());

    return true;
}

void wifiOff()
{
    Serial.println("TARS: WiFi OFF");

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    wifiIsOn = false;

    delay(150);
}

// ============================================================
// ASK
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

    if (!http.begin(client, TARS_ASK_URL))
        return false;

    http.addHeader(
        "Content-Type",
        "application/json"
    );

    JsonDocument request;

    request["text"] = question;

    String body;

    serializeJson(
        request,
        body
    );

    Serial.println("TARS: POST /ask");

    int code =
        http.POST(body);

    Serial.printf(
        "ASK HTTP: %d\n",
        code
    );

    if (code != HTTP_CODE_OK)
    {
        http.end();
        return false;
    }

    String response =
        http.getString();

    http.end();

    JsonDocument json;

    if (
        deserializeJson(
            json,
            response
        )
    )
        return false;

    const char *result =
        json["response"] | "";

    if (!result || result[0] == '\0')
        return false;

    answer = String(result);

    Serial.println("TARS RESPONSE:");
    Serial.println(answer);

    return true;
}

// ============================================================
// TTS TEXT
// ============================================================

String makeTTSText(
    const String &text
)
{
    if (
        text.length() <=
        TTS_MAX_CHARS
    )
        return text;

    String result =
        text.substring(
            0,
            TTS_MAX_CHARS
        );

    int cut =
        result.lastIndexOf('.');

    if (cut < 100)
        cut =
            result.lastIndexOf(' ');

    if (cut > 100)
        result =
            result.substring(
                0,
                cut + 1
            );

    result.trim();

    return result;
}

// ============================================================
// DOWNLOAD TTS
// ============================================================

bool downloadTTS(
    const String &text
)
{
    if (!connectWiFi())
        return false;

    String ttsText =
        makeTTSText(text);

    Serial.printf(
        "TARS: TTS chars = %d\n",
        ttsText.length()
    );

    if (LittleFS.exists(MP3_FILE))
        LittleFS.remove(MP3_FILE);

    File output =
        LittleFS.open(
            MP3_FILE,
            FILE_WRITE
        );

    if (!output)
        return false;

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;

    if (!http.begin(client, TARS_TTS_URL))
    {
        output.close();
        return false;
    }

    http.addHeader(
        "Content-Type",
        "application/json"
    );

    JsonDocument request;

    request["text"] = ttsText;

    String body;

    serializeJson(
        request,
        body
    );

    Serial.println("TARS: POST /tts");

    int code =
        http.POST(body);

    Serial.printf(
        "TTS HTTP: %d\n",
        code
    );

    if (code != HTTP_CODE_OK)
    {
        http.end();
        output.close();

        LittleFS.remove(MP3_FILE);

        return false;
    }

    WiFiClient *stream =
        http.getStreamPtr();

    uint8_t buffer[1024];

    size_t total = 0;

    int contentLength =
        http.getSize();

    uint32_t lastData =
        millis();

    while (http.connected())
    {
        size_t available =
            stream->available();

        if (available > 0)
        {
            size_t amount =
                min(
                    available,
                    sizeof(buffer)
                );

            int read =
                stream->readBytes(
                    buffer,
                    amount
                );

            if (read > 0)
            {
                output.write(
                    buffer,
                    read
                );

                total += read;

                lastData = millis();

                if (contentLength > 0)
                {
                    contentLength -= read;

                    if (contentLength <= 0)
                        break;
                }
            }
        }
        else
        {
            delay(1);

            if (
                millis() - lastData >
                5000
            )
                break;
        }
    }

    output.flush();
    output.close();

    http.end();

    Serial.printf(
        "TARS: MP3 bytes = %u\n",
        (unsigned int)total
    );

    if (total < 512)
    {
        LittleFS.remove(MP3_FILE);
        return false;
    }

    return true;
}

// ============================================================
// A2DP CALLBACK
// ============================================================

int32_t getAudioData(
    uint8_t *data,
    int32_t len
)
{
    if (!data || len <= 0)
        return 0;

    size_t wanted =
        (size_t)len;

    size_t got =
        pcmOutput.readPCM(
            data,
            wanted
        );

    if (got < wanted)
    {
        memset(
            data + got,
            0,
            wanted - got
        );
    }

    return wanted;
}

// ============================================================
// DECODER
// ============================================================

bool startDecoder()
{
    if (!LittleFS.exists(MP3_FILE))
        return false;

    mp3File =
        LittleFS.open(
            MP3_FILE,
            FILE_READ
        );

    if (!mp3File)
        return false;

    pcmOutput.clearBuffer();

    mp3InputFinished = false;
    audioDecoderReady = false;

    if (!decoder.begin())
    {
        mp3File.close();
        return false;
    }

    audioDecoderReady = true;

    Serial.println(
        "TARS: MP3 DECODER READY"
    );

    return true;
}

void stopDecoder()
{
    audioDecoderReady = false;

    if (mp3File)
        mp3File.close();

    Serial.println(
        "TARS: MP3 DECODER STOP"
    );
}

// ============================================================
// BLUETOOTH
// ============================================================

bool startBluetooth()
{
    Serial.println(
        "TARS: Bluetooth START"
    );

    a2dpSource.set_auto_reconnect(false);

    a2dpSource.set_data_callback(
        getAudioData
    );

    a2dpSource.start(
        BT_HEADSET_NAME
    );

    uint32_t start =
        millis();

    while (
        a2dpSource.get_connection_state() !=
        ESP_A2D_CONNECTION_STATE_CONNECTED
    )
    {
        delay(100);

        if (
            millis() - start >=
            BT_TIMEOUT_MS
        )
        {
            Serial.println(
                "TARS: Bluetooth TIMEOUT"
            );

            btIsOn = false;

            return false;
        }
    }

    btIsOn = true;

    Serial.println(
        "TARS: A2DP CONNECTED"
    );

    return true;
}

void stopBluetooth()
{
    Serial.println(
        "TARS: Bluetooth STOP"
    );

    a2dpSource.end(true);

    delay(300);

    btIsOn = false;

    Serial.println(
        "TARS: Bluetooth OFF"
    );
}

// ============================================================
// PCM PRIME
// ============================================================

bool primePCM()
{
    Serial.println(
        "TARS: PCM PRIMING"
    );

    uint32_t start =
        millis();

    size_t previous =
        0;

    while (
        pcmOutput.availablePCM() <
        PCM_PRIME_BYTES
    )
    {
        if (
            millis() - start >
            10000
        )
        {
            Serial.println(
                "TARS: PCM PRIME TIMEOUT"
            );

            return false;
        }

        if (mp3InputFinished)
            break;

        size_t before =
            pcmOutput.availablePCM();

        size_t copied =
            mp3Copier.copy();

        size_t after =
            pcmOutput.availablePCM();

        if (copied == 0)
        {
            if (after == before)
            {
                mp3InputFinished = true;

                Serial.println(
                    "TARS: MP3 EOF DURING PRIME"
                );

                break;
            }
        }

        previous = after;

        delay(1);
    }

    size_t primed =
        pcmOutput.availablePCM();

    Serial.print(
        "TARS: PCM PRIMED = "
    );

    Serial.println(primed);

    return primed > 0;
}

// ============================================================
// PLAY
// ============================================================

bool playMP3()
{
    if (!startDecoder())
        return false;

    if (!primePCM())
    {
        stopDecoder();
        return false;
    }

    if (!startBluetooth())
    {
        stopDecoder();
        stopBluetooth();
        return false;
    }

    playbackRunning = true;

    tarsState =
        TARS_SPEAKING;

    speechVisibleChars = 0;

    speechLastUpdate =
        millis();

    Serial.println(
        "TARS: PLAY START"
    );

    uint32_t start =
        millis();

    while (playbackRunning)
    {
        // Hanya decode lagi jika buffer
        // masih punya ruang cukup.
        if (
            !mp3InputFinished &&
            pcmOutput.availableForWrite() >= 1024
        )
        {
            size_t copied =
                mp3Copier.copy();

            if (copied == 0)
            {
                mp3InputFinished = true;

                Serial.println(
                    "TARS: MP3 INPUT EOF"
                );
            }
        }

        if (
            a2dpSource.get_connection_state() !=
            ESP_A2D_CONNECTION_STATE_CONNECTED
        )
        {
            Serial.println(
                "TARS: A2DP DISCONNECTED"
            );

            playbackRunning = false;

            break;
        }

        if (
            mp3InputFinished &&
            pcmOutput.availablePCM() == 0
        )
        {
            delay(150);

            if (
                pcmOutput.availablePCM() == 0
            )
            {
                playbackRunning = false;
            }
        }

        if (
            millis() - start >
            PLAY_TIMEOUT_MS
        )
        {
            Serial.println(
                "TARS: PLAYBACK TIMEOUT"
            );

            playbackRunning = false;
        }

        delay(1);
    }

    delay(150);

    Serial.println(
        "TARS: PLAY FINISHED"
    );

    stopDecoder();

    stopBluetooth();

    pcmOutput.clearBuffer();

    Serial.println(
        "TARS: PLAY END"
    );

    return true;
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
        "================================"
    );

    Serial.println("QUESTION:");
    Serial.println(question);

    Serial.println(
        "================================"
    );

    tarsState =
        TARS_THINKING;

    String answer;

    if (!askTars(question, answer))
    {
        tarsState = TARS_ERROR;

        delay(1500);

        connectWiFi();

        tarsState =
            TARS_WAITING;

        return;
    }

    speechText = answer;
    speechVisibleChars = 0;

    tarsState =
        TARS_PREPARING_AUDIO;

    if (!downloadTTS(answer))
    {
        tarsState = TARS_ERROR;

        delay(1500);

        connectWiFi();

        tarsState =
            TARS_WAITING;

        return;
    }

    wifiOff();

    bool played =
        playMP3();

    if (!played)
    {
        Serial.println(
            "TARS: PLAY FAILED"
        );

        tarsState =
            TARS_ERROR;

        delay(1500);
    }

    if (LittleFS.exists(MP3_FILE))
        LittleFS.remove(MP3_FILE);

    if (!connectWiFi())
    {
        Serial.println(
            "TARS: WiFi recovery FAILED"
        );

        tarsState =
            TARS_ERROR;

        delay(1500);
    }

    speechVisibleChars =
        speechText.length();

    tarsState =
        TARS_WAITING;

    Serial.println();
    Serial.println(
        "TARS READY"
    );

    Serial.println(
        "Ketik pertanyaan:"
    );
}

// ============================================================
// WORKER
// ============================================================

static void tarsWorkerTask(
    void *parameter
)
{
    QuestionMessage message;

    while (true)
    {
        if (
            xQueueReceive(
                questionQueue,
                &message,
                portMAX_DELAY
            ) == pdTRUE
        )
        {
            processQuestion(
                String(message.text)
            );

            workerBusy = false;
        }
    }
}

// ============================================================
// SERIAL
// ============================================================

void handleSerial()
{
    while (Serial.available())
    {
        char c =
            (char)Serial.read();

        if (c == '\r')
            continue;

        if (c == '\n')
        {
            if (serialLineLength == 0)
                continue;

            if (workerBusy)
            {
                Serial.println(
                    "TARS: masih memproses."
                );

                serialLineLength = 0;
                serialLine[0] = '\0';

                continue;
            }

            QuestionMessage message;

            memset(
                &message,
                0,
                sizeof(message)
            );

            memcpy(
                message.text,
                serialLine,
                serialLineLength
            );

            message.text[
                serialLineLength
            ] = '\0';

            if (
                xQueueSend(
                    questionQueue,
                    &message,
                    0
                ) == pdTRUE
            )
            {
                workerBusy = true;

                Serial.println(
                    "TARS: COMMAND RECEIVED"
                );
            }

            serialLineLength = 0;
            serialLine[0] = '\0';

            continue;
        }

        if (
            serialLineLength <
            sizeof(serialLine) - 1
        )
        {
            serialLine[
                serialLineLength++
            ] = c;

            serialLine[
                serialLineLength
            ] = '\0';
        }
    }
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(SERIAL_BAUD);

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

    Wire.begin(
        OLED_SDA,
        OLED_SCL
    );

    oled.begin(
        SSD1306_SWITCHCAPVCC,
        OLED_ADDR
    );

    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    oled.setCursor(32, 22);
    oled.print("T A R S");

    oled.setCursor(43, 34);
    oled.print("BOOT");

    oled.display();

    delay(500);

    if (!LittleFS.begin(true))
    {
        Serial.println(
            "LittleFS ERROR"
        );

        while (true)
            delay(1000);
    }

    Serial.println(
        "LittleFS OK"
    );

    pcmOutput.begin();

    questionQueue =
        xQueueCreate(
            1,
            sizeof(QuestionMessage)
        );

    if (!questionQueue)
    {
        Serial.println(
            "TARS: QUEUE ERROR"
        );

        while (true)
            delay(1000);
    }

    tarsState =
        TARS_THINKING;

    while (!connectWiFi())
    {
        delay(1000);
    }

    while (!syncNTPOnce())
    {
        delay(1000);
    }

    xTaskCreatePinnedToCore(
        tarsWorkerTask,
        "TARS_WORKER",
        8192,
        nullptr,
        1,
        nullptr,
        0
    );

    tarsState =
        TARS_WAITING;

    Serial.println();
    Serial.println(
        "================================"
    );
    Serial.println(
        "           TARS READY"
    );
    Serial.println(
        "================================"
    );

    Serial.println(
        "WiFi : ON"
    );

    Serial.println(
        "NTP  : SYNCED ONCE"
    );

    Serial.println(
        "BT   : OFF"
    );

    Serial.println(
        "VOLUME: 2X + LIMITER"
    );

    Serial.println(
        "PCM  : NON-BLOCKING BUFFER"
    );

    Serial.println(
        "A2DP : PRIME BEFORE START"
    );

    Serial.println(
        "TARS: Ketik pertanyaan lalu ENTER"
    );
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
    handleSerial();

    updateOLED();

    delay(2);
}
