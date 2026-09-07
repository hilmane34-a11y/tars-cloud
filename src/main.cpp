#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Wire.h>
#include <time.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "BluetoothA2DPSource.h"
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "config.h"


// ============================================================
// TARS ESP32
//
// MP3:
//   22050 Hz / MONO / 16 bit
//
// Decoder:
//   Helix
//
// PCM conversion:
//   22050 mono
//       |
//       v
//   44100 stereo
//
// Output:
//   ESP32 A2DP Source
//       |
//       v
//   I7-TWS
//
// RAM LOW VERSION
// ============================================================


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
// BLUETOOTH
// ============================================================

BluetoothA2DPSource a2dpSource;


// ============================================================
// AUDIO CONSTANTS
// ============================================================

// 16 KB PCM ring.
// 44100 * 2ch * 2byte = 176400 byte/sec
// 16384 byte ~= 93 ms audio.
static const size_t PCM_BUFFER_SIZE = 16384;

// StreamCopy input buffer.
static const size_t MP3_COPY_BUFFER = 1024;

// Small temporary PCM conversion buffer.
static const size_t PCM_OUTPUT_BUFFER = 1024;

// Expected TTS format.
static const uint32_t INPUT_SAMPLE_RATE = 22050;
static const uint8_t INPUT_CHANNELS = 1;
static const uint8_t INPUT_BITS = 16;

static const uint32_t OUTPUT_SAMPLE_RATE = 44100;
static const uint8_t OUTPUT_CHANNELS = 2;
static const uint8_t OUTPUT_BITS = 16;


// ============================================================
// TIMING
// ============================================================

static const uint32_t WIFI_TIMEOUT_MS = 15000;
static const uint32_t NTP_TIMEOUT_MS = 15000;
static const uint32_t BT_TIMEOUT_MS = 20000;
static const uint32_t PLAY_TIMEOUT_MS = 120000;


// ============================================================
// AUDIO
// ============================================================

static const float PCM_GAIN = 2.0f;

static const char *MP3_PATH =
    "/tts.mp3";


// ============================================================
// STATE
// ============================================================

volatile bool btConnected = false;
volatile bool btAudioStarted = false;
volatile bool playbackRunning = false;

volatile uint32_t btCallbackCalls = 0;

bool ntpSynced = false;


// ============================================================
// HEAP DEBUG
// ============================================================

void printHeap(
    const char *label
)
{
    size_t freeHeap =
        heap_caps_get_free_size(
            MALLOC_CAP_8BIT
        );

    size_t largestBlock =
        heap_caps_get_largest_free_block(
            MALLOC_CAP_8BIT
        );

    size_t minimumHeap =
        heap_caps_get_minimum_free_size(
            MALLOC_CAP_8BIT
        );

    Serial.printf(
        "HEAP %s: free=%u largest=%u min=%u\n",
        label,
        (unsigned)freeHeap,
        (unsigned)largestBlock,
        (unsigned)minimumHeap
    );
}


// ============================================================
// PCM RING BUFFER
// ============================================================

class PCMRingBuffer {

private:

    uint8_t *buffer = nullptr;

    size_t capacity = 0;

    volatile size_t readPos = 0;
    volatile size_t writePos = 0;
    volatile size_t used = 0;

    portMUX_TYPE mux =
        portMUX_INITIALIZER_UNLOCKED;


public:

    bool begin(
        size_t size
    )
    {
        if (buffer != nullptr) {

            free(buffer);

            buffer = nullptr;
        }

        buffer =
            (uint8_t *)malloc(size);

        if (!buffer) {

            capacity = 0;

            return false;
        }

        capacity = size;

        clear();

        return true;
    }


    void end()
    {
        if (buffer) {

            free(buffer);

            buffer = nullptr;
        }

        capacity = 0;
        readPos = 0;
        writePos = 0;
        used = 0;
    }


    void clear()
    {
        portENTER_CRITICAL(
            &mux
        );

        readPos = 0;
        writePos = 0;
        used = 0;

        portEXIT_CRITICAL(
            &mux
        );
    }


    size_t available()
    {
        size_t value;

        portENTER_CRITICAL(
            &mux
        );

        value = used;

        portEXIT_CRITICAL(
            &mux
        );

        return value;
    }


    size_t freeSpace()
    {
        size_t value;

        portENTER_CRITICAL(
            &mux
        );

        value =
            capacity - used;

        portEXIT_CRITICAL(
            &mux
        );

        return value;
    }


    size_t write(
        const uint8_t *src,
        size_t len
    )
    {
        if (
            !buffer ||
            !src ||
            len == 0
        ) {
            return 0;
        }

        portENTER_CRITICAL(
            &mux
        );

        size_t freeBytes =
            capacity - used;

        if (len > freeBytes) {
            len = freeBytes;
        }

        if (len > 0) {

            size_t first =
                capacity - writePos;

            if (first > len) {
                first = len;
            }

            memcpy(
                buffer + writePos,
                src,
                first
            );

            size_t second =
                len - first;

            if (second > 0) {

                memcpy(
                    buffer,
                    src + first,
                    second
                );
            }

            writePos += len;

            if (
                writePos >= capacity
            ) {
                writePos -= capacity;
            }

            used += len;
        }

        portEXIT_CRITICAL(
            &mux
        );

        return len;
    }


    size_t read(
        uint8_t *dst,
        size_t len
    )
    {
        if (
            !buffer ||
            !dst ||
            len == 0
        ) {
            return 0;
        }

        portENTER_CRITICAL(
            &mux
        );

        if (len > used) {
            len = used;
        }

        if (len > 0) {

            size_t first =
                capacity - readPos;

            if (first > len) {
                first = len;
            }

            memcpy(
                dst,
                buffer + readPos,
                first
            );

            size_t second =
                len - first;

            if (second > 0) {

                memcpy(
                    dst + first,
                    buffer,
                    second
                );
            }

            readPos += len;

            if (
                readPos >= capacity
            ) {
                readPos -= capacity;
            }

            used -= len;
        }

        portEXIT_CRITICAL(
            &mux
        );

        return len;
    }
};


PCMRingBuffer pcmRing;


// ============================================================
// PCM OUTPUT STREAM
//
// INPUT:
//   22050 Hz
//   mono
//   16 bit
//
// OUTPUT:
//   44100 Hz
//   stereo
//   16 bit
//
// 22050 -> 44100:
//   setiap sample diulang 2 kali.
//
// mono -> stereo:
//   sample yang sama dikirim ke L dan R.
//
// 1 input sample:
//   2 temporal frames
//   x 2 channel
//   x 2 byte
//   = 8 output bytes
// ============================================================

class PCMOutputStream : public AudioStream {

private:

    AudioInfo currentInfo;

    uint8_t outputBuffer[
        PCM_OUTPUT_BUFFER
    ];


    int16_t applyGain(
        int16_t sample
    )
    {
        int32_t value =
            (int32_t)(
                (float)sample *
                PCM_GAIN
            );

        if (
            value > 32767
        ) {
            value = 32767;
        }

        if (
            value < -32768
        ) {
            value = -32768;
        }

        return (int16_t)value;
    }


    bool waitForSpace(
        size_t required
    )
    {
        uint32_t start =
            millis();

        while (
            pcmRing.freeSpace() <
            required
        ) {

            // A2DP harus sudah aktif.
            // Beri kesempatan task BT
            // untuk mengonsumsi PCM.
            delay(2);
            yield();


            if (
                millis() - start >
                5000
            ) {

                Serial.println(
                    "PCM: WAIT SPACE TIMEOUT"
                );

                printHeap(
                    "PCM_TIMEOUT"
                );

                return false;
            }
        }

        return true;
    }


public:

    void setAudioInfo(
        AudioInfo info
    ) override
    {
        currentInfo = info;

        AudioStream::setAudioInfo(
            info
        );

        Serial.printf(
            "PCM format: %d Hz, %d ch, %d bit\n",
            info.sample_rate,
            info.channels,
            info.bits_per_sample
        );
    }


    AudioInfo audioInfo()
    {
        return currentInfo;
    }


    int availableForWrite()
        override
    {
        size_t freeBytes =
            pcmRing.freeSpace();

        // 22050 mono -> 44100 stereo
        // output expansion = 4x.
        //
        // Jangan laporkan seluruh ring
        // sebagai input capacity.
        if (
            currentInfo.sample_rate ==
                INPUT_SAMPLE_RATE &&
            currentInfo.channels ==
                INPUT_CHANNELS &&
            currentInfo.bits_per_sample ==
                INPUT_BITS
        ) {

            size_t inputBytes =
                freeBytes / 4;

            if (
                inputBytes > 1024
            ) {
                inputBytes = 1024;
            }

            return (
                int
            )inputBytes;
        }


        return (
            int
        )min(
            freeBytes,
            (size_t)1024
        );
    }


    size_t write(
        const uint8_t *data,
        size_t len
    ) override
    {
        if (
            !data ||
            len == 0
        ) {
            return 0;
        }


        // ----------------------------------------------------
        // TARS expects ElevenLabs MP3:
        //
        // 22050 Hz
        // mono
        // 16 bit
        // ----------------------------------------------------

        if (
            currentInfo.sample_rate !=
                INPUT_SAMPLE_RATE ||
            currentInfo.channels !=
                INPUT_CHANNELS ||
            currentInfo.bits_per_sample !=
                INPUT_BITS
        ) {

            Serial.printf(
                "PCM ERROR: unsupported %d Hz %d ch %d bit\n",
                currentInfo.sample_rate,
                currentInfo.channels,
                currentInfo.bits_per_sample
            );

            return 0;
        }


        const int16_t *samples =
            (const int16_t *)data;


        size_t totalSamples =
            len / 2;

        size_t samplePos = 0;


        while (
            samplePos <
            totalSamples
        ) {

            // 1024 output bytes / 8 bytes
            // per input sample = 128 samples.
            size_t chunkSamples =
                sizeof(outputBuffer) / 8;


            size_t remaining =
                totalSamples -
                samplePos;

            if (
                chunkSamples >
                remaining
            ) {
                chunkSamples =
                    remaining;
            }


            size_t requiredOutput =
                chunkSamples * 8;


            // ------------------------------------------------
            // Never write a partial block.
            // Wait until complete output
            // block can fit.
            // ------------------------------------------------

            if (
                !waitForSpace(
                    requiredOutput
                )
            ) {
                return 0;
            }


            int16_t *out =
                (int16_t *)outputBuffer;


            size_t outIndex = 0;


            for (
                size_t i = 0;
                i < chunkSamples;
                i++
            ) {

                int16_t sample =
                    applyGain(
                        samples[
                            samplePos + i
                        ]
                    );


                // 22050 -> 44100
                //
                // frame 1
                out[outIndex++] =
                    sample;

                out[outIndex++] =
                    sample;


                // frame 2
                out[outIndex++] =
                    sample;

                out[outIndex++] =
                    sample;
            }


            size_t written =
                pcmRing.write(
                    outputBuffer,
                    requiredOutput
                );


            if (
                written !=
                requiredOutput
            ) {

                Serial.printf(
                    "PCM ERROR: wrote %u/%u\n",
                    (unsigned)written,
                    (unsigned)requiredOutput
                );

                return 0;
            }


            samplePos +=
                chunkSamples;
        }


        return len;
    }
};


PCMOutputStream pcmOutput;


// ============================================================
// MP3 DECODER
// ============================================================

MP3DecoderHelix mp3Decoder;

EncodedAudioStream mp3Stream(
    &pcmOutput,
    &mp3Decoder
);


// ============================================================
// OLED
// ============================================================

void oledText(
    const char *text
)
{
    oled.clearDisplay();

    oled.setTextSize(1);

    oled.setTextColor(
        SSD1306_WHITE
    );

    oled.setCursor(
        0,
        0
    );

    oled.println(text);

    oled.display();
}


// ============================================================
// TIME
// ============================================================

bool isTimeValid()
{
    time_t now =
        time(nullptr);

    return (
        now > 1700000000
    );
}


bool syncNTP()
{
    Serial.println(
        "TARS: TIME INVALID - NTP REQUIRED"
    );

    Serial.println(
        "TARS: NTP START"
    );


    configTime(
        7 * 3600,
        0,
        "pool.ntp.org",
        "time.nist.gov",
        "time.google.com"
    );


    uint32_t start =
        millis();

    int attempt = 0;


    while (
        !isTimeValid() &&
        millis() - start <
            NTP_TIMEOUT_MS
    ) {

        attempt++;

        Serial.printf(
            "TARS: NTP attempt %d\n",
            attempt
        );

        delay(1000);
    }


    if (
        !isTimeValid()
    ) {

        Serial.println(
            "TARS: NTP FAILED"
        );

        ntpSynced = false;

        return false;
    }


    time_t now =
        time(nullptr);

    struct tm timeinfo;

    localtime_r(
        &now,
        &timeinfo
    );


    Serial.printf(
        "TARS: NTP OK %04d-%02d-%02d %02d:%02d:%02d\n",
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
    );


    ntpSynced = true;

    return true;
}


bool ensureTimeValid()
{
    if (
        ntpSynced &&
        isTimeValid()
    ) {
        return true;
    }


    if (
        isTimeValid()
    ) {

        ntpSynced = true;

        return true;
    }


    return syncNTP();
}


// ============================================================
// WIFI
// ============================================================

bool connectWiFi(
    bool requireTime = false
)
{
    Serial.println(
        "TARS: WiFi ON"
    );


    WiFi.mode(
        WIFI_STA
    );


    if (
        WiFi.status() ==
        WL_CONNECTED
    ) {

        Serial.print(
            "TARS: IP = "
        );

        Serial.println(
            WiFi.localIP()
        );


        if (
            requireTime &&
            !ensureTimeValid()
        ) {
            return false;
        }


        return true;
    }


    WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
    );


    uint32_t start =
        millis();


    while (
        WiFi.status() !=
            WL_CONNECTED &&
        millis() - start <
            WIFI_TIMEOUT_MS
    ) {

        delay(250);

        Serial.print(".");
    }


    Serial.println();


    if (
        WiFi.status() !=
        WL_CONNECTED
    ) {

        Serial.println(
            "TARS: WiFi FAILED"
        );

        return false;
    }


    Serial.print(
        "TARS: IP = "
    );

    Serial.println(
        WiFi.localIP()
    );


    if (
        requireTime &&
        !ensureTimeValid()
    ) {
        return false;
    }


    return true;
}


void disconnectWiFi()
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


    delay(300);
}


// ============================================================
// ASK AI
// ============================================================

bool askAI(
    const String &question,
    String &answer
)
{
    if (
        !connectWiFi(true)
    ) {
        return false;
    }


    Serial.println(
        "TARS: POST /ask"
    );


    WiFiClientSecure client;

    client.setInsecure();


    HTTPClient http;


    String url =
        String(WORKER_URL) +
        "/ask";


    if (
        !http.begin(
            client,
            url
        )
    ) {

        Serial.println(
            "ASK HTTP BEGIN FAILED"
        );

        return false;
    }


    http.addHeader(
        "Content-Type",
        "application/json"
    );


    JsonDocument requestDoc;

    requestDoc["text"] =
        question;


    String requestBody;

    serializeJson(
        requestDoc,
        requestBody
    );


    int code =
        http.POST(
            requestBody
        );


    Serial.print(
        "ASK HTTP: "
    );

    Serial.println(
        code
    );


    if (
        code != 200
    ) {

        http.end();

        return false;
    }


    String payload =
        http.getString();


    http.end();


    JsonDocument responseDoc;


    DeserializationError err =
        deserializeJson(
            responseDoc,
            payload
        );


    if (err) {

        Serial.print(
            "ASK JSON ERROR: "
        );

        Serial.println(
            err.c_str()
        );

        return false;
    }


    answer =
        responseDoc["response"] |
        "";


    Serial.println(
        "TARS RESPONSE:"
    );

    Serial.println(
        answer
    );


    return (
        answer.length() > 0
    );
}


// ============================================================
// TTS DOWNLOAD
// ============================================================

bool downloadTTS(
    const String &text
)
{
    if (
        !connectWiFi(true)
    ) {
        return false;
    }


    String ttsText =
        text;


    if (
        ttsText.length() > 450
    ) {

        ttsText =
            ttsText.substring(
                0,
                450
            );
    }


    Serial.print(
        "TARS: TTS chars = "
    );

    Serial.println(
        ttsText.length()
    );


    Serial.println(
        "TARS: POST /tts"
    );


    WiFiClientSecure client;

    client.setInsecure();


    HTTPClient http;


    String url =
        String(WORKER_URL) +
        "/tts";


    if (
        !http.begin(
            client,
            url
        )
    ) {

        Serial.println(
            "TTS HTTP BEGIN FAILED"
        );

        return false;
    }


    http.addHeader(
        "Content-Type",
        "application/json"
    );


    JsonDocument requestDoc;

    requestDoc["text"] =
        ttsText;


    String requestBody;

    serializeJson(
        requestDoc,
        requestBody
    );


    int code =
        http.POST(
            requestBody
        );


    Serial.print(
        "TTS HTTP: "
    );

    Serial.println(
        code
    );


    if (
        code != 200
    ) {

        http.end();

        return false;
    }


    if (
        LittleFS.exists(
            MP3_PATH
        )
    ) {

        LittleFS.remove(
            MP3_PATH
        );
    }


    File file =
        LittleFS.open(
            MP3_PATH,
            FILE_WRITE
        );


    if (!file) {

        Serial.println(
            "TARS: MP3 FILE OPEN FAILED"
        );

        http.end();

        return false;
    }


    WiFiClient *stream =
        http.getStreamPtr();


    int totalLength =
        http.getSize();


    uint8_t buffer[
        1024
    ];


    size_t total = 0;

    uint32_t start =
        millis();


    while (
        http.connected() &&
        (
            totalLength > 0 ||
            totalLength == -1
        )
    ) {

        size_t available =
            stream->available();


        if (
            available > 0
        ) {

            size_t readSize =
                available;


            if (
                readSize >
                sizeof(buffer)
            ) {
                readSize =
                    sizeof(buffer);
            }


            int len =
                stream->readBytes(
                    buffer,
                    readSize
                );


            if (
                len > 0
            ) {

                file.write(
                    buffer,
                    len
                );


                total +=
                    len;


                if (
                    totalLength > 0
                ) {
                    totalLength -=
                        len;
                }


                start =
                    millis();
            }
        }
        else {

            delay(1);
        }


        if (
            millis() - start >
            30000
        ) {
            break;
        }
    }


    file.flush();

    file.close();

    http.end();


    Serial.print(
        "TARS: MP3 bytes = "
    );

    Serial.println(
        total
    );


    return (
        total > 100
    );
}


// ============================================================
// A2DP DATA CALLBACK
// ============================================================

int32_t getAudioData(
    uint8_t *data,
    int32_t len
)
{
    if (
        !data ||
        len <= 0
    ) {
        return 0;
    }


    btCallbackCalls++;


    size_t got =
        pcmRing.read(
            data,
            len
        );


    if (
        got <
        (size_t)len
    ) {

        memset(
            data + got,
            0,
            len - got
        );
    }


    return len;
}


// ============================================================
// BLUETOOTH CONNECTION CALLBACK
// ============================================================

void onBTConnectionState(
    esp_a2d_connection_state_t state,
    void *obj
)
{
    Serial.print(
        "TARS: A2DP STATE = "
    );


    Serial.println(
        a2dpSource.to_str(
            state
        )
    );


    if (
        state ==
        ESP_A2D_CONNECTION_STATE_CONNECTED
    ) {

        btConnected = true;

        Serial.println(
            "TARS: A2DP CONNECTED"
        );
    }
    else {

        btConnected = false;
    }
}


// ============================================================
// BLUETOOTH AUDIO CALLBACK
// ============================================================

void onBTAudioState(
    esp_a2d_audio_state_t state,
    void *obj
)
{
    Serial.print(
        "TARS: A2DP AUDIO = "
    );


    Serial.println(
        a2dpSource.to_str(
            state
        )
    );


    if (
        state ==
        ESP_A2D_AUDIO_STATE_STARTED
    ) {

        btAudioStarted = true;

        Serial.println(
            "TARS: A2DP AUDIO STARTED"
        );
    }
    else {

        btAudioStarted = false;
    }
}


// ============================================================
// START BLUETOOTH
// ============================================================

bool startBluetooth()
{
    Serial.println(
        "TARS: Bluetooth START"
    );


    btConnected = false;

    btAudioStarted = false;

    btCallbackCalls = 0;


    a2dpSource.set_auto_reconnect(
        false
    );


    a2dpSource.set_data_callback(
        getAudioData
    );


    a2dpSource.set_on_connection_state_changed(
        onBTConnectionState
    );


    a2dpSource.set_on_audio_state_changed(
        onBTAudioState
    );


    a2dpSource.start(
        BT_HEADSET_NAME
    );


    uint32_t start =
        millis();


    while (
        !btConnected &&
        millis() - start <
            BT_TIMEOUT_MS
    ) {

        delay(50);

        yield();
    }


    if (
        !btConnected
    ) {

        Serial.println(
            "TARS: Bluetooth TIMEOUT"
        );

        return false;
    }


    Serial.println(
        "TARS: Bluetooth READY"
    );


    // Give A2DP task time to
    // establish media transport.
    delay(300);


    printHeap(
        "BT_READY"
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


    btAudioStarted = false;


    delay(100);


    a2dpSource.end(
        true
    );


    btConnected = false;


    delay(500);


    Serial.printf(
        "TARS: A2DP callbacks = %u\n",
        (unsigned)btCallbackCalls
    );


    printHeap(
        "BT_OFF"
    );


    Serial.println(
        "TARS: Bluetooth OFF"
    );
}


// ============================================================
// PLAY MP3
// ============================================================

bool playMP3()
{
    Serial.println(
        "TARS: PLAY START"
    );


    playbackRunning = true;


    pcmRing.clear();


    // --------------------------------------------------------
    // Bluetooth FIRST.
    // --------------------------------------------------------

    if (
        !startBluetooth()
    ) {

        playbackRunning = false;

        stopBluetooth();

        return false;
    }


    // --------------------------------------------------------
    // Open MP3.
    // --------------------------------------------------------

    File mp3File =
        LittleFS.open(
            MP3_PATH,
            FILE_READ
        );


    if (!mp3File) {

        Serial.println(
            "TARS: MP3 FILE FAILED"
        );

        playbackRunning = false;

        stopBluetooth();

        return false;
    }


    size_t mp3Size =
        mp3File.size();


    Serial.printf(
        "TARS: MP3 SIZE = %u\n",
        (unsigned)mp3Size
    );


    if (
        mp3Size == 0
    ) {

        mp3File.close();

        playbackRunning = false;

        stopBluetooth();

        return false;
    }


    // --------------------------------------------------------
    // HEAP BEFORE HELIX
    // --------------------------------------------------------

    printHeap(
        "BEFORE_HELIX"
    );


    // --------------------------------------------------------
    // Helix memory settings.
    //
    // 22050 mono:
    // 1152 samples * 2 bytes = 2304 bytes.
    //
    // 4096 gives safe headroom without using 4608.
    //
    // MP3 frame buffer remains default-safe 2048.
    // --------------------------------------------------------

    mp3Decoder.setMaxPCMSize(
        4096
    );


    mp3Decoder.setMaxFrameSize(
        2048
    );


    Serial.printf(
        "HELIX: PCM MAX = %u\n",
        (unsigned)
        mp3Decoder.maxPCMSize()
    );


    Serial.printf(
        "HELIX: FRAME MAX = %u\n",
        (unsigned)
        mp3Decoder.maxFrameSize()
    );


    // --------------------------------------------------------
    // Disable automatic audio-change propagation until
    // decoder actually detects the MP3 format.
    // --------------------------------------------------------

    mp3Stream.setNotifyAudioChange(
        false
    );


    // --------------------------------------------------------
    // Start decoder.
    // --------------------------------------------------------

    if (
        !mp3Stream.begin()
    ) {

        Serial.println(
            "TARS: MP3 STREAM BEGIN FAILED"
        );

        mp3File.close();

        playbackRunning = false;

        stopBluetooth();

        return false;
    }


    Serial.println(
        "TARS: MP3 DECODER READY"
    );


    printHeap(
        "AFTER_HELIX"
    );


    // --------------------------------------------------------
    // IMPORTANT:
    //
    // StreamCopy must receive:
    //
    //   destination = mp3Stream
    //   source      = mp3File
    //
    // The repository version previously created a global
    // StreamCopy without an MP3 source.
    // --------------------------------------------------------

    StreamCopy mp3Copier(
        mp3Stream,
        mp3File,
        MP3_COPY_BUFFER
    );


    // Do NOT use check_available_for_write here.
    //
    // mp3Stream is the encoded destination and its
    // availableForWrite() is not the same thing as the
    // final PCM ring capacity.
    //
    // PCMOutputStream performs its own exact backpressure.
    mp3Copier.setCheckAvailable(
        true
    );


    uint32_t playStart =
        millis();

    uint32_t lastLog =
        millis();

    size_t lastFilePos = 0;


    bool eof = false;


    // --------------------------------------------------------
    // DECODE LOOP
    // --------------------------------------------------------

    while (
        millis() - playStart <
        PLAY_TIMEOUT_MS
    ) {

        if (
            !btConnected
        ) {

            Serial.println(
                "TARS: BT LOST"
            );

            break;
        }


        size_t copied =
            mp3Copier.copy();


        size_t filePos =
            mp3File.position();


        size_t pcmUsed =
            pcmRing.available();


        // ----------------------------------------------------
        // Periodic diagnostics.
        // ----------------------------------------------------

        if (
            millis() - lastLog >
            1000
        ) {

            Serial.printf(
                "AUDIO: MP3=%u/%u PCM=%u/%u BT=%d AUDIO=%d CB=%u\n",
                (unsigned)filePos,
                (unsigned)mp3Size,
                (unsigned)pcmUsed,
                (unsigned)PCM_BUFFER_SIZE,
                btConnected,
                btAudioStarted,
                (unsigned)btCallbackCalls
            );


            printHeap(
                "PLAY"
            );


            lastLog =
                millis();


            if (
                filePos !=
                lastFilePos
            ) {
                lastFilePos =
                    filePos;
            }
        }


        // ----------------------------------------------------
        // EOF
        // ----------------------------------------------------

        if (
            filePos >=
            mp3Size
        ) {

            eof = true;

            Serial.println(
                "TARS: MP3 EOF"
            );

            break;
        }


        // ----------------------------------------------------
        // No progress.
        // Give A2DP task CPU time.
        // ----------------------------------------------------

        if (
            copied == 0
        ) {

            delay(2);

            yield();
        }
        else {

            yield();
        }
    }


    // --------------------------------------------------------
    // Flush decoder before ending stream.
    // --------------------------------------------------------

    mp3Decoder.flush();


    delay(50);


    mp3Stream.end();


    mp3File.close();


    // --------------------------------------------------------
    // Drain PCM.
    // --------------------------------------------------------

    Serial.println(
        "TARS: PCM DRAIN"
    );


    uint32_t drainStart =
        millis();


    while (
        pcmRing.available() > 0 &&
        millis() - drainStart <
            10000
    ) {

        delay(10);

        yield();
    }


    size_t remaining =
        pcmRing.available();


    Serial.printf(
        "TARS: PCM REMAINING = %u\n",
        (unsigned)remaining
    );


    Serial.printf(
        "TARS: A2DP callbacks final = %u\n",
        (unsigned)btCallbackCalls
    );


    stopBluetooth();


    playbackRunning = false;


    LittleFS.remove(
        MP3_PATH
    );


    if (
        !eof
    ) {

        Serial.println(
            "TARS: PLAYBACK TIMEOUT/FAILED"
        );

        return false;
    }


    Serial.println(
        "TARS: PLAY FINISHED"
    );


    return true;
}


// ============================================================
// HANDLE QUESTION
// ============================================================

void handleQuestion(
    const String &question
)
{
    Serial.println();

    Serial.println(
        "TARS: COMMAND RECEIVED"
    );

    Serial.println(
        "QUESTION:"
    );

    Serial.println(
        question
    );


    // --------------------------------------------------------
    // WiFi + valid time.
    // --------------------------------------------------------

    if (
        !connectWiFi(true)
    ) {

        Serial.println(
            "TARS: WIFI/TIME FAILED"
        );

        return;
    }


    // --------------------------------------------------------
    // ASK
    // --------------------------------------------------------

    String answer;


    if (
        !askAI(
            question,
            answer
        )
    ) {

        Serial.println(
            "TARS: ASK FAILED"
        );

        return;
    }


    // --------------------------------------------------------
    // TTS
    // --------------------------------------------------------

    if (
        !downloadTTS(
            answer
        )
    ) {

        Serial.println(
            "TARS: TTS FAILED"
        );

        return;
    }


    // --------------------------------------------------------
    // WiFi OFF before Bluetooth.
    // --------------------------------------------------------

    disconnectWiFi();


    // --------------------------------------------------------
    // Bluetooth + MP3.
    // --------------------------------------------------------

    bool played =
        playMP3();


    if (
        !played
    ) {

        Serial.println(
            "TARS: PLAY FAILED"
        );
    }


    // --------------------------------------------------------
    // WiFi ON again.
    //
    // No blind NTP.
    // If system time is still valid,
    // simply reconnect.
    // --------------------------------------------------------

    if (
        !connectWiFi(false)
    ) {

        Serial.println(
            "TARS: WIFI RECONNECT FAILED"
        );

        return;
    }


    // If time somehow disappeared,
    // synchronize again.
    if (
        !isTimeValid()
    ) {

        Serial.println(
            "TARS: TIME LOST"
        );

        syncNTP();
    }


    Serial.println();

    Serial.println(
        "================================"
    );

    Serial.println(
        "TARS: READY FOR QUESTION"
    );

    Serial.println(
        "================================"
    );
}


// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(
        115200
    );


    delay(500);


    Serial.println();

    Serial.println(
        "================================"
    );

    Serial.println(
        "        TARS ESP32 START"
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
    ) {

        Serial.println(
            "OLED FAILED"
        );

        while (true) {
            delay(1000);
        }
    }


    oledText(
        "TARS BOOT"
    );


    // --------------------------------------------------------
    // LittleFS
    // --------------------------------------------------------

    if (
        !LittleFS.begin(
            true
        )
    ) {

        Serial.println(
            "LittleFS FAILED"
        );

        oledText(
            "LITTLEFS ERROR"
        );

        while (true) {
            delay(1000);
        }
    }


    Serial.println(
        "LittleFS OK"
    );


    // --------------------------------------------------------
    // PCM ring
    // --------------------------------------------------------

    if (
        !pcmRing.begin(
            PCM_BUFFER_SIZE
        )
    ) {

        Serial.println(
            "PCM BUFFER FAILED"
        );

        oledText(
            "PCM ERROR"
        );

        while (true) {
            delay(1000);
        }
    }


    Serial.printf(
        "PCM  : %uKB SAFE BUFFER\n",
        (unsigned)(
            PCM_BUFFER_SIZE /
            1024
        )
    );


    Serial.println(
        "PRIME: DISABLED"
    );


    Serial.println(
        "A2DP : BT BEFORE DECODER"
    );


    printHeap(
        "BOOT"
    );


    // --------------------------------------------------------
    // WiFi
    // --------------------------------------------------------

    oledText(
        "WIFI..."
    );


    if (
        !connectWiFi(
            true
        )
    ) {

        oledText(
            "WIFI/NTP ERROR"
        );

        while (true) {
            delay(1000);
        }
    }


    // --------------------------------------------------------
    // READY
    // --------------------------------------------------------

    oledText(
        "TARS READY"
    );


    Serial.println(
        "================================"
    );

    Serial.println(
        "TARS: READY FOR QUESTION"
    );

    Serial.println(
        "================================"
    );


    Serial.println(
        "WiFi : ON"
    );

    Serial.println(
        "NTP  : VALID"
    );

    Serial.println(
        "BT   : OFF"
    );


    Serial.printf(
        "PCM  : %uKB\n",
        (unsigned)(
            PCM_BUFFER_SIZE /
            1024
        )
    );


    Serial.println(
        "HELIX: PCM 4096"
    );

    Serial.println(
        "HELIX: FRAME 2048"
    );

    Serial.println(
        "AUDIO: 22050 MONO -> 44100 STEREO"
    );

    Serial.println(
        "VOLUME: 2X + LIMITER"
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
    if (
        Serial.available()
    ) {

        String question =
            Serial.readStringUntil(
                '\n'
            );


        question.trim();


        if (
            question.length() > 0
        ) {

            handleQuestion(
                question
            );
        }
    }


    delay(10);
}
