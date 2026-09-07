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
static const uint32_t BT_TIMEOUT_MS   = 20000;
static const uint32_t PLAY_TIMEOUT_MS = 120000;

static const uint32_t OLED_REFRESH_MS = 80;
static const uint32_t TEXT_SPEED_MS   = 35;

// ============================================================
// PCM BUFFER
// ============================================================

static const size_t PCM_BUFFER_SIZE = 16384;

static uint8_t pcmBuffer[PCM_BUFFER_SIZE];

static volatile size_t pcmReadPos  = 0;
static volatile size_t pcmWritePos = 0;
static volatile size_t pcmUsed     = 0;

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
// SYSTEM STATE
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
static volatile bool btIsOn   = false;
static volatile bool workerBusy = false;

// ============================================================
// SPEAKING TEXT
// Hanya worker yang menulis.
// loop() hanya membaca untuk OLED.
// ============================================================

static String speechText;

static volatile size_t speechVisibleChars = 0;
static volatile uint32_t speechLastUpdate = 0;

static uint32_t oledLastRefresh = 0;
static uint32_t panelAnimation = 0;

// ============================================================
// SERIAL INPUT
// ============================================================

static String serialInput;

// ============================================================
// AUDIO STATE
// ============================================================

static bool audioDecoderReady = false;
static bool mp3InputFinished  = false;
static bool playbackRunning   = false;

// ============================================================
// PCM OUTPUT STREAM
// ============================================================

class PCMOutputStream : public AudioStream
{
public:

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

        Serial.print("PCM format: ");
        Serial.print(info.sample_rate);
        Serial.print(" Hz, ");
        Serial.print(info.channels);
        Serial.print(" ch, ");
        Serial.print(info.bits_per_sample);
        Serial.println(" bit");
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
        size_t written = 0;

        while (written < size)
        {
            portENTER_CRITICAL(&pcmMux);

            size_t freeBytes =
                PCM_BUFFER_SIZE - pcmUsed;

            if (freeBytes == 0)
            {
                portEXIT_CRITICAL(&pcmMux);

                delay(1);
                continue;
            }

            size_t chunk =
                size - written;

            if (chunk > freeBytes)
                chunk = freeBytes;

            size_t untilEnd =
                PCM_BUFFER_SIZE - pcmWritePos;

            if (chunk > untilEnd)
                chunk = untilEnd;

            memcpy(
                &pcmBuffer[pcmWritePos],
                data + written,
                chunk
            );

            pcmWritePos += chunk;

            if (pcmWritePos >= PCM_BUFFER_SIZE)
                pcmWritePos = 0;

            pcmUsed += chunk;
            written += chunk;

            portEXIT_CRITICAL(&pcmMux);
        }

        return written;
    }

    size_t readPCM(
        uint8_t *data,
        size_t size
    )
    {
        size_t read = 0;

        portENTER_CRITICAL(&pcmMux);

        size_t available =
            pcmUsed;

        if (size > available)
            size = available;

        size_t untilEnd =
            PCM_BUFFER_SIZE - pcmReadPos;

        if (size > untilEnd)
            size = untilEnd;

        if (size > 0)
        {
            memcpy(
                data,
                &pcmBuffer[pcmReadPos],
                size
            );

            pcmReadPos += size;

            if (pcmReadPos >= PCM_BUFFER_SIZE)
                pcmReadPos = 0;

            pcmUsed -= size;

            read = size;
        }

        portEXIT_CRITICAL(&pcmMux);

        return read;
    }

    size_t availablePCM()
    {
        portENTER_CRITICAL(&pcmMux);

        size_t value =
            pcmUsed;

        portEXIT_CRITICAL(&pcmMux);

        return value;
    }

    void clearBuffer()
    {
        portENTER_CRITICAL(&pcmMux);

        pcmReadPos = 0;
        pcmWritePos = 0;
        pcmUsed = 0;

        portEXIT_CRITICAL(&pcmMux);
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
// OLED LOW LEVEL
// ============================================================

static void drawPanelFrame()
{
    oled.drawRect(
        0,
        0,
        128,
        64,
        SSD1306_WHITE
    );

    oled.drawLine(
        0,
        11,
        127,
        11,
        SSD1306_WHITE
    );

    oled.drawLine(
        0,
        53,
        127,
        53,
        SSD1306_WHITE
    );
}

// ============================================================
// OLED STATUS HEADER
// ============================================================

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

// ============================================================
// OLED CONNECTION STATUS
// ============================================================

static void drawConnectionStatus()
{
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    oled.setCursor(4, 56);

    oled.print("W:");

    oled.print(
        wifiIsOn ? "ON" : "OFF"
    );

    oled.setCursor(68, 56);

    oled.print("BT:");

    oled.print(
        btIsOn ? "ON" : "OFF"
    );
}

// ============================================================
// OLED WAITING PANEL
// ============================================================

static void drawWaitingPanel()
{
    drawPanelFrame();

    drawHeader("READY");

    oled.setTextSize(1);

    oled.setCursor(6, 18);
    oled.print("SYSTEM READY");

    oled.setCursor(6, 29);
    oled.print("AWAITING COMMAND");

    // indikator mekanis
    int offset =
        (panelAnimation / 2) % 10;

    oled.drawRect(
        6,
        41,
        116,
        6,
        SSD1306_WHITE
    );

    for (int i = 0; i < 10; i++)
    {
        if (i == offset)
            continue;

        oled.fillRect(
            9 + i * 11,
            43,
            7,
            2,
            SSD1306_WHITE
        );
    }

    drawConnectionStatus();
}

// ============================================================
// OLED THINKING PANEL
// ============================================================

static void drawThinkingPanel()
{
    drawPanelFrame();

    drawHeader("THINK");

    oled.setTextSize(1);

    oled.setCursor(6, 18);
    oled.print("PROCESSING");

    oled.setCursor(6, 29);
    oled.print("ANALYZING INPUT");

    int active =
        (panelAnimation / 3) % 12;

    for (int i = 0; i < 12; i++)
    {
        int x =
            6 + i * 10;

        if (i <= active)
        {
            oled.fillRect(
                x,
                42,
                7,
                5,
                SSD1306_WHITE
            );
        }
        else
        {
            oled.drawRect(
                x,
                42,
                7,
                5,
                SSD1306_WHITE
            );
        }
    }

    drawConnectionStatus();
}

// ============================================================
// OLED PREPARING AUDIO
// ============================================================

static void drawPreparingPanel()
{
    drawPanelFrame();

    drawHeader("AUDIO");

    oled.setTextSize(1);

    oled.setCursor(6, 18);
    oled.print("PREPARING VOICE");

    oled.setCursor(6, 29);
    oled.print("LOADING MP3");

    int active =
        (panelAnimation / 2) % 12;

    for (int i = 0; i < 12; i++)
    {
        int x =
            6 + i * 10;

        if (i == active)
        {
            oled.fillRect(
                x,
                42,
                7,
                5,
                SSD1306_WHITE
            );
        }
        else
        {
            oled.drawRect(
                x,
                42,
                7,
                5,
                SSD1306_WHITE
            );
        }
    }

    drawConnectionStatus();
}

// ============================================================
// OLED SPEAKING TEXT
// ============================================================

static void drawSpeakingPanel()
{
    drawPanelFrame();

    drawHeader("SPEAK");

    oled.setTextSize(1);

    const size_t visible =
        speechVisibleChars;

    const size_t length =
        speechText.length();

    size_t charsPerLine = 20;
    size_t maxLines = 4;

    size_t startIndex = 0;

    /*
     * Tampilkan bagian teks terbaru
     * agar layar tidak berhenti pada
     * awal jawaban ketika jawabannya panjang.
     */
    if (visible > charsPerLine * maxLines)
    {
        startIndex =
            visible -
            charsPerLine * maxLines;
    }

    size_t pos =
        startIndex;

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
                speechText[pos];

            if (c == '\n')
            {
                pos++;
                break;
            }

            oled.print(c);

            pos++;
            count++;
        }
    }

    // Audio indicator
    int wave =
        (panelAnimation / 2) % 10;

    for (int i = 0; i < 10; i++)
    {
        int h =
            2 + ((i + wave) % 5);

        oled.fillRect(
            6 + i * 11,
            50 - h,
            7,
            h,
            SSD1306_WHITE
        );
    }

    drawConnectionStatus();
}

// ============================================================
// OLED ERROR
// ============================================================

static void drawErrorPanel()
{
    drawPanelFrame();

    drawHeader("ERROR");

    oled.setTextSize(1);

    oled.setCursor(6, 20);
    oled.print("SYSTEM ERROR");

    oled.setCursor(6, 32);
    oled.print("CHECK CONNECTION");

    oled.drawRect(
        6,
        43,
        116,
        5,
        SSD1306_WHITE
    );

    drawConnectionStatus();
}

// ============================================================
// OLED RENDER
//
// Hanya dipanggil dari loop utama.
// Jadi worker task tidak menyentuh OLED.
// ============================================================

static void updateOLED()
{
    uint32_t now =
        millis();

    if (
        now - oledLastRefresh <
        OLED_REFRESH_MS
    )
    {
        return;
    }

    oledLastRefresh =
        now;

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

    /*
     * Typing non-blocking.
     */
    if (
        tarsState == TARS_SPEAKING &&
        speechVisibleChars <
        speechText.length()
    )
    {
        if (
            now - speechLastUpdate >=
            TEXT_SPEED_MS
        )
        {
            speechLastUpdate =
                now;

            speechVisibleChars++;
        }
    }

    oled.display();
}

// ============================================================
// WIFI CONNECT
//
// Worker task boleh blocking di sini.
// loop utama tetap jalan.
// ============================================================

bool connectWiFi()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        wifiIsOn = true;
        return true;
    }

    Serial.println();
    Serial.println("TARS: WiFi ON");

    WiFi.mode(WIFI_STA);

    WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
    );

    uint32_t start =
        millis();

    while (
        WiFi.status() != WL_CONNECTED
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

    Serial.print(
        "TARS: IP = "
    );

    Serial.println(
        WiFi.localIP()
    );

    return true;
}

// ============================================================
// WIFI OFF
// ============================================================

void wifiOff()
{
    Serial.println(
        "TARS: WiFi OFF"
    );

    WiFi.disconnect(true);

    WiFi.mode(WIFI_OFF);

    wifiIsOn = false;

    delay(100);
}

// ============================================================
// ASK TARS CLOUD
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

    if (
        !http.begin(
            client,
            TARS_ASK_URL
        )
    )
    {
        Serial.println(
            "TARS: HTTP begin gagal"
        );

        return false;
    }

    http.addHeader(
        "Content-Type",
        "application/json"
    );

    JsonDocument request;

    request["text"] =
        question;

    String body;

    serializeJson(
        request,
        body
    );

    Serial.println(
        "TARS: POST /ask"
    );

    int code =
        http.POST(body);

    Serial.print(
        "ASK HTTP: "
    );

    Serial.println(code);

    if (
        code != HTTP_CODE_OK
    )
    {
        Serial.println(
            http.getString()
        );

        http.end();

        return false;
    }

    String response =
        http.getString();

    http.end();

    JsonDocument json;

    DeserializationError error =
        deserializeJson(
            json,
            response
        );

    if (error)
    {
        Serial.print(
            "JSON error: "
        );

        Serial.println(
            error.c_str()
        );

        return false;
    }

    const char *result =
        json["response"] | "";

    if (
        result == nullptr ||
        result[0] == '\0'
    )
    {
        Serial.println(
            "TARS: response kosong"
        );

        return false;
    }

    answer =
        String(result);

    Serial.println();
    Serial.println(
        "TARS RESPONSE:"
    );
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

    if (
        LittleFS.exists(
            MP3_FILE
        )
    )
    {
        LittleFS.remove(
            MP3_FILE
        );
    }

    File output =
        LittleFS.open(
            MP3_FILE,
            FILE_WRITE
        );

    if (!output)
    {
        Serial.println(
            "TARS: MP3 file gagal dibuat"
        );

        return false;
    }

    WiFiClientSecure client;

    client.setInsecure();

    HTTPClient http;

    if (
        !http.begin(
            client,
            TARS_TTS_URL
        )
    )
    {
        output.close();
        return false;
    }

    http.addHeader(
        "Content-Type",
        "application/json"
    );

    JsonDocument request;

    request["text"] =
        text;

    String body;

    serializeJson(
        request,
        body
    );

    Serial.println(
        "TARS: POST /tts"
    );

    int code =
        http.POST(body);

    Serial.print(
        "TTS HTTP: "
    );

    Serial.println(code);

    if (
        code != HTTP_CODE_OK
    )
    {
        Serial.println(
            http.getString()
        );

        http.end();
        output.close();

        LittleFS.remove(
            MP3_FILE
        );

        return false;
    }

    WiFiClient *stream =
        http.getStreamPtr();

    uint8_t buffer[1024];

    int contentLength =
        http.getSize();

    size_t total =
        0;

    uint32_t lastData =
        millis();

    while (
        http.connected()
    )
    {
        size_t available =
            stream->available();

        if (available > 0)
        {
            size_t amount =
                available;

            if (
                amount >
                sizeof(buffer)
            )
            {
                amount =
                    sizeof(buffer);
            }

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

                total +=
                    read;

                lastData =
                    millis();

                if (
                    contentLength > 0
                )
                {
                    contentLength -=
                        read;

                    if (
                        contentLength <= 0
                    )
                    {
                        break;
                    }
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
            {
                break;
            }
        }
    }

    output.flush();
    output.close();

    http.end();

    Serial.print(
        "TARS: MP3 bytes = "
    );

    Serial.println(total);

    if (total < 512)
    {
        LittleFS.remove(
            MP3_FILE
        );

        Serial.println(
            "TARS: MP3 invalid"
        );

        return false;
    }

    return true;
}

// ============================================================
// A2DP CALLBACK
//
// TIDAK melakukan:
// - WiFi
// - HTTP
// - MP3 decode
// - Serial print
//
// Hanya mengambil PCM.
// ============================================================

int32_t getAudioData(
    uint8_t *data,
    int32_t len
)
{
    if (len <= 0)
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

        return wanted;
    }

    return got;
}

// ============================================================
// START BLUETOOTH
// ============================================================

bool startBluetooth()
{
    Serial.println(
        "TARS: Bluetooth START"
    );

    a2dpSource.set_auto_reconnect(
        false
    );

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
                "TARS: Bluetooth timeout"
            );

            btIsOn = false;

            return false;
        }
    }

    btIsOn = true;

    Serial.println(
        "TARS: I7-TWS CONNECTED"
    );

    return true;
}

// ============================================================
// STOP BLUETOOTH
// ============================================================

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

    pcmOutput.clearBuffer();

    mp3InputFinished =
        false;

    audioDecoderReady =
        false;

    if (
        !decoder.begin()
    )
    {
        Serial.println(
            "TARS: MP3 decoder gagal"
        );

        mp3File.close();

        return false;
    }

    audioDecoderReady =
        true;

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
    audioDecoderReady =
        false;

    delay(50);

    if (mp3File)
    {
        mp3File.close();
    }

    Serial.println(
        "TARS: MP3 decoder STOP"
    );
}

// ============================================================
// PLAY MP3
// ============================================================

bool playMP3()
{
    if (!startBluetooth())
    {
        stopBluetooth();
        return false;
    }

    if (!startDecoder())
    {
        stopBluetooth();
        return false;
    }

    playbackRunning =
        true;

    mp3InputFinished =
        false;

    tarsState =
        TARS_SPEAKING;

    speechVisibleChars =
        0;

    speechLastUpdate =
        millis();

    Serial.println(
        "TARS: PLAY START"
    );

    uint32_t start =
        millis();

    while (
        playbackRunning
    )
    {
        if (!mp3InputFinished)
        {
            size_t copied =
                mp3Copier.copy();

            if (copied == 0)
            {
                mp3InputFinished =
                    true;

                Serial.println(
                    "TARS: MP3 INPUT EOF"
                );
            }
        }

        if (
            mp3InputFinished &&
            pcmOutput.availablePCM() == 0
        )
        {
            delay(250);

            if (
                pcmOutput.availablePCM() == 0
            )
            {
                playbackRunning =
                    false;
            }
        }

        if (
            a2dpSource.get_connection_state() !=
            ESP_A2D_CONNECTION_STATE_CONNECTED
        )
        {
            Serial.println(
                "TARS: A2DP disconnected"
            );

            playbackRunning =
                false;
        }

        if (
            millis() - start >
            PLAY_TIMEOUT_MS
        )
        {
            Serial.println(
                "TARS: playback timeout"
            );

            playbackRunning =
                false;
        }

        /*
         * Sangat penting:
         * worker task tidak menguasai CPU terus.
         * loop utama tetap berjalan untuk OLED
         * dan Serial.
         */
        delay(1);
    }

    delay(100);

    stopDecoder();

    stopBluetooth();

    Serial.println(
        "TARS: PLAY END"
    );

    return true;
}

// ============================================================
// PROCESS QUESTION
//
// Berjalan di FreeRTOS worker task,
// bukan di loop utama.
// ============================================================

void processQuestion(
    const String &question
)
{
    if (
        question.length() == 0
    )
    {
        return;
    }

    Serial.println();
    Serial.println(
        "================================"
    );
    Serial.println(
        "QUESTION:"
    );
    Serial.println(question);
    Serial.println(
        "================================"
    );

    tarsState =
        TARS_THINKING;

    // --------------------------------------------------------
    // ASK AI
    // --------------------------------------------------------

    String answer;

    if (
        !askTars(
            question,
            answer
        )
    )
    {
        Serial.println(
            "TARS: ASK FAILED"
        );

        tarsState =
            TARS_ERROR;

        connectWiFi();

        delay(1500);

        tarsState =
            TARS_WAITING;

        return;
    }

    // --------------------------------------------------------
    // SIMPAN TEXT UNTUK OLED
    // --------------------------------------------------------

    speechText =
        answer;

    speechVisibleChars =
        0;

    // --------------------------------------------------------
    // TTS
    // --------------------------------------------------------

    tarsState =
        TARS_PREPARING_AUDIO;

    if (
        !downloadTTS(
            answer
        )
    )
    {
        Serial.println(
            "TARS: TTS FAILED"
        );

        tarsState =
            TARS_ERROR;

        delay(1500);

        connectWiFi();

        tarsState =
            TARS_WAITING;

        return;
    }

    // --------------------------------------------------------
    // WIFI OFF
    // --------------------------------------------------------

    wifiOff();

    // --------------------------------------------------------
    // PLAY MP3
    // --------------------------------------------------------

    if (!playMP3())
    {
        Serial.println(
            "TARS: PLAY FAILED"
        );

        tarsState =
            TARS_ERROR;

        delay(1500);
    }

    // --------------------------------------------------------
    // DELETE MP3
    // --------------------------------------------------------

    if (
        LittleFS.exists(
            MP3_FILE
        )
    )
    {
        LittleFS.remove(
            MP3_FILE
        );
    }

    // --------------------------------------------------------
    // WIFI ON
    // --------------------------------------------------------

    connectWiFi();

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
// FREE RTOS WORKER
// ============================================================

static void tarsWorkerTask(
    void *parameter
)
{
    String question;

    while (true)
    {
        /*
         * Worker hanya aktif ketika
         * ada pertanyaan baru.
         */
        if (
            workerBusy
        )
        {
            vTaskDelay(
                pdMS_TO_TICKS(20)
            );

            continue;
        }

        /*
         * Ambil pertanyaan dari
         * serial queue sederhana.
         */
        if (
            serialInput.length() > 0
        )
        {
            question =
                serialInput;

            serialInput =
                "";

            workerBusy =
                true;

            processQuestion(
                question
            );

            workerBusy =
                false;

            question =
                "";
        }

        vTaskDelay(
            pdMS_TO_TICKS(10)
        );
    }
}

// ============================================================
// SERIAL
//
// loop utama hanya membaca karakter.
// Tidak pernah menjalankan HTTP/TTS/A2DP.
// ============================================================

void handleSerial()
{
    while (
        Serial.available()
    )
    {
        char c =
            (char)Serial.read();

        if (c == '\r')
            continue;

        if (c == '\n')
        {
            if (
                serialInput.length() == 0
            )
            {
                continue;
            }

            if (workerBusy)
            {
                Serial.println();
                Serial.println(
                    "TARS: masih memproses."
                );
                Serial.println(
                    "Tunggu sampai TARS READY."
                );

                serialInput =
                    "";

                continue;
            }

            /*
             * serialInput sudah berisi
             * pertanyaan.
             *
             * Worker task akan mengambilnya.
             */
            continue;
        }

        if (
            serialInput.length() < 300
        )
        {
            serialInput += c;
        }
    }
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(
        115200
    );

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

    if (
        !oled.begin(
            SSD1306_SWITCHCAPVCC,
            OLED_ADDR
        )
    )
    {
        Serial.println(
            "OLED ERROR"
        );
    }
    else
    {
        oled.clearDisplay();
        oled.display();

        tarsState =
            TARS_BOOT;

        oled.clearDisplay();

        oled.drawRect(
            0,
            0,
            128,
            64,
            SSD1306_WHITE
        );

        oled.setTextSize(1);
        oled.setTextColor(
            SSD1306_WHITE
        );

        oled.setCursor(
            32,
            22
        );

        oled.print(
            "T A R S"
        );

        oled.setCursor(
            43,
            34
        );

        oled.print(
            "BOOT"
        );

        oled.display();

        delay(500);
    }

    // --------------------------------------------------------
    // LITTLEFS
    // --------------------------------------------------------

    if (
        !LittleFS.begin(true)
    )
    {
        Serial.println(
            "LittleFS ERROR"
        );

        tarsState =
            TARS_ERROR;

        while (true)
        {
            updateOLED();

            delay(100);
        }
    }

    Serial.println(
        "LittleFS OK"
    );

    // --------------------------------------------------------
    // PCM
    // --------------------------------------------------------

    pcmOutput.begin();

    // --------------------------------------------------------
    // WIFI
    // --------------------------------------------------------

    if (
        !connectWiFi()
    )
    {
        Serial.println(
            "TARS: WiFi belum tersedia"
        );
    }

    // --------------------------------------------------------
    // START WORKER TASK
    // --------------------------------------------------------

    xTaskCreatePinnedToCore(
        tarsWorkerTask,
        "TARS_Worker",
        8192,
        nullptr,
        1,
        nullptr,
        0
    );

    // --------------------------------------------------------
    // READY
    // --------------------------------------------------------

    tarsState =
        TARS_WAITING;

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
// LOOP
//
// LOOP SEKARANG RINGAN.
//
// Tidak ada:
// - HTTP
// - WiFi connect blocking
// - TTS
// - MP3 decoding
// - A2DP start
//
// Jadi OLED dan Serial tetap responsif.
// ============================================================

void loop()
{
    handleSerial();

    updateOLED();

    delay(2);
}
