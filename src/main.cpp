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

// MP3_FILE sengaja TIDAK didefinisikan di sini.
// Kalau config.h nanti memiliki MP3_FILE, tidak akan bentrok.
#ifndef MP3_FILE
#define MP3_FILE "/tars.mp3"
#endif

// ============================================================
// TIMING
// ============================================================

static const uint32_t WIFI_TIMEOUT_MS = 15000;
static const uint32_t BT_TIMEOUT_MS   = 20000;
static const uint32_t PLAY_TIMEOUT_MS = 120000;

// ============================================================
// PCM BUFFER
//
// 16384 bytes ~= 93 ms PCM @ 44.1 kHz stereo 16-bit.
// Dibuat cukup besar untuk mencegah A2DP underrun,
// tetapi tidak terlalu besar agar RAM ESP32 tetap aman.
// ============================================================

static const size_t PCM_BUFFER_SIZE = 16384;

static uint8_t pcmBuffer[PCM_BUFFER_SIZE];

static volatile size_t pcmReadPos  = 0;
static volatile size_t pcmWritePos = 0;
static volatile size_t pcmUsed     = 0;

static portMUX_TYPE pcmMux =
    portMUX_INITIALIZER_UNLOCKED;

// ============================================================
// STATE
// ============================================================

static bool audioDecoderReady = false;
static bool mp3InputFinished  = false;
static bool playbackRunning   = false;

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
// CUSTOM AUDIO OUTPUT
//
// MP3DecoderHelix -> EncodedAudioStream -> PCMOutputStream
// -> PCM ring buffer -> A2DP callback
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

                /*
                 * Jangan blocking terlalu lama.
                 * A2DP callback harus tetap bisa
                 * mengosongkan buffer.
                 */
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
// OLED
// ============================================================

void oledText(
    const String &text
)
{
    oled.clearDisplay();

    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);

    int lineLength = 0;

    for (size_t i = 0; i < text.length(); i++)
    {
        char c = text[i];

        if (c == '\n')
        {
            oled.println();
            lineLength = 0;
            continue;
        }

        oled.print(c);

        lineLength++;

        if (lineLength >= 21)
        {
            oled.println();
            lineLength = 0;
        }
    }

    oled.display();
}

// ============================================================
// OLED TYPING
// ============================================================

void oledTyping(
    const String &text
)
{
    oled.clearDisplay();

    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);

    int lineLength = 0;

    for (size_t i = 0; i < text.length(); i++)
    {
        char c = text[i];

        if (c == '\n')
        {
            oled.println();
            lineLength = 0;
        }
        else
        {
            oled.print(c);
            lineLength++;

            if (lineLength >= 21)
            {
                oled.println();
                lineLength = 0;
            }
        }

        oled.display();

        delay(25);
    }
}

// ============================================================
// WIFI CONNECT
// ============================================================

bool connectWiFi()
{
    if (WiFi.status() == WL_CONNECTED)
        return true;

    Serial.println();
    Serial.println("TARS: WiFi ON");

    oledText(
        "TARS\nWiFi connecting..."
    );

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
            Serial.println(
                "TARS: WiFi timeout"
            );

            oledText(
                "TARS\nWiFi gagal"
            );

            return false;
        }
    }

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

    WiFi.disconnect(
        true
    );

    WiFi.mode(
        WIFI_OFF
    );

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

    /*
     * Cloudflare certificate verification
     * pada MicroPython sebelumnya bermasalah.
     *
     * Untuk ESP32 Arduino kita bypass verifikasi
     * certificate sehingga HTTPS tetap dapat digunakan.
     */
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

    Serial.println(
        total
    );

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
// A2DP PCM CALLBACK
//
// Callback ini dipanggil oleh BluetoothA2DPSource.
// Jangan melakukan decoding MP3 atau WiFi di sini.
// Hanya ambil PCM dari ring buffer.
// ============================================================

int32_t getAudioData(
    uint8_t *data,
    int32_t len
)
{
    if (
        len <= 0
    )
    {
        return 0;
    }

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

    oledText(
        "TARS\nBluetooth ON\nMencari I7-TWS..."
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

            return false;
        }
    }

    Serial.println(
        "TARS: I7-TWS CONNECTED"
    );

    oledText(
        "TARS\nI7-TWS CONNECTED"
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

    a2dpSource.end(
        true
    );

    delay(300);

    Serial.println(
        "TARS: Bluetooth OFF"
    );
}

// ============================================================
// START MP3 DECODER
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

    /*
     * Decoder akan meneruskan hasil PCM
     * ke PCMOutputStream.
     */
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

    oledText(
        "TARS\nBerbicara..."
    );

    Serial.println(
        "TARS: PLAY START"
    );

    uint32_t start =
        millis();

    /*
     * Decoder berjalan di loop utama.
     * A2DP callback mengambil PCM dari ring buffer
     * secara paralel.
     */
    while (
        playbackRunning
    )
    {
        /*
         * Beri decoder data MP3.
         */
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

        /*
         * Setelah input MP3 habis,
         * tunggu PCM terakhir benar-benar
         * keluar dari ring buffer.
         */
        if (
            mp3InputFinished &&
            pcmOutput.availablePCM() == 0
        )
        {
            /*
             * Beri waktu kecil supaya
             * paket PCM terakhir masuk
             * ke A2DP stack.
             */
            delay(250);

            if (
                pcmOutput.availablePCM() == 0
            )
            {
                playbackRunning =
                    false;
            }
        }

        /*
         * Bluetooth terputus.
         */
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

        /*
         * Safety timeout.
         */
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

        delay(1);
    }

    /*
     * Pastikan buffer PCM kosong.
     */
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

    oledText(
        "TARS\nBerpikir..."
    );

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
        oledText(
            "TARS\nCloud gagal"
        );

        connectWiFi();

        return;
    }

    // --------------------------------------------------------
    // SHOW ANSWER
    // --------------------------------------------------------

    oledTyping(
        answer
    );

    // --------------------------------------------------------
    // TTS
    // --------------------------------------------------------

    oledText(
        "TARS\nMenyiapkan suara..."
    );

    if (
        !downloadTTS(
            answer
        )
    )
    {
        Serial.println(
            "TARS: TTS FAILED"
        );

        oledText(
            "TARS\nTTS gagal"
        );

        return;
    }

    // --------------------------------------------------------
    // WIFI OFF
    // --------------------------------------------------------

    wifiOff();

    // --------------------------------------------------------
    // MP3 -> PCM -> A2DP
    // --------------------------------------------------------

    playMP3();

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

    oledText(
        "TARS\nSiap menunggu..."
    );

    Serial.println();
    Serial.println(
        "TARS READY"
    );
    Serial.println(
        "Ketik pertanyaan:"
    );
}

// ============================================================
// SERIAL
// ============================================================

String serialInput;

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
                serialInput.length() > 0
            )
            {
                String question =
                    serialInput;

                serialInput = "";

                processQuestion(
                    question
                );
            }

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
        oledText(
            "TARS\nBooting..."
        );
    }

    // --------------------------------------------------------
    // LITTLEFS
    // --------------------------------------------------------

    if (
        !LittleFS.begin(
            true
        )
    )
    {
        Serial.println(
            "LittleFS ERROR"
        );

        oledText(
            "TARS\nLittleFS ERROR"
        );

        while (true)
        {
            delay(1000);
        }
    }

    Serial.println(
        "LittleFS OK"
    );

    // --------------------------------------------------------
    // PCM OUTPUT
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
    // READY
    // --------------------------------------------------------

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
// LOOP
// ============================================================

void loop()
{
    handleSerial();

    delay(5);
}
