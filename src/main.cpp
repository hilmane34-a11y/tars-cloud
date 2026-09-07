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

#include <BluetoothA2DPSource.h>

#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"

#include "config.h"


// ============================================================
// TARS ESP32
//
// MP3 22050 Hz MONO
//        ↓
// Helix PCM 22050 Hz MONO
//        ↓
// 2x sample duplication
//        ↓
// PCM 44100 Hz STEREO
//        ↓
// Ring Buffer
//        ↓
// ESP32 A2DP Source
//        ↓
// I7-TWS
// ============================================================


// ============================================================
// OLED
// ============================================================

#define OLED_WIDTH   128
#define OLED_HEIGHT  64
#define OLED_ADDR    0x3C

Adafruit_SSD1306 oled(
    OLED_WIDTH,
    OLED_HEIGHT,
    &Wire,
    -1
);

bool oledReadyFlag = false;


// ============================================================
// OLED TYPING
// ============================================================

static String oledAnswer = "";

static size_t oledTypedChars = 0;

static uint32_t oledLastType = 0;

// 55 ms = kecepatan lama
// 44 ms = 80% dari 55 ms
static const uint32_t OLED_TYPE_INTERVAL = 44;

static bool oledTyping = false;


// ============================================================
// OLED MECHANICAL ANIMATION
// ============================================================

static uint32_t oledLastAnim = 0;

static uint8_t oledMechanicalFrame = 0;

// Kecepatan gerakan struktur
static const uint32_t OLED_ANIM_INTERVAL = 80;


// ============================================================
// OLED HEADER
// ============================================================

void oledHeader(
    const char *status
) {

    if (!oledReadyFlag) {
        return;
    }

    oled.setTextColor(
        SSD1306_WHITE
    );

    oled.setTextSize(1);


    // --------------------------------------------------------
    // T A R S
    // --------------------------------------------------------

    oled.setCursor(
        45,
        0
    );

    oled.print(
        "T A R S"
    );


    // --------------------------------------------------------
    // Garis mekanikal header
    // --------------------------------------------------------

    oled.drawLine(
        0,
        9,
        27,
        9,
        SSD1306_WHITE
    );

    oled.drawLine(
        34,
        9,
        61,
        9,
        SSD1306_WHITE
    );

    oled.drawLine(
        67,
        9,
        94,
        9,
        SSD1306_WHITE
    );

    oled.drawLine(
        101,
        9,
        127,
        9,
        SSD1306_WHITE
    );


    // --------------------------------------------------------
    // Status
    // --------------------------------------------------------

    oled.setCursor(
        3,
        13
    );

    oled.print(
        "> "
    );

    oled.print(
        status
    );
}


// ============================================================
// OLED MECHANICAL STRUCTURE
// ============================================================
//
// Tidak ada wajah.
// Tidak ada kotak sudut.
//
// NORMAL:
//
// --___--__---___--__--___
//
// Struktur terus bergeser agar terlihat seperti TARS
// tetap hidup walaupun sedang diam.
//
// SPEAKING:
//
// Struktur bergerak lebih aktif.
// ============================================================

void oledDrawMechanical(
    bool speaking
) {

    if (!oledReadyFlag) {
        return;
    }


    const int baseY = 62;


    // --------------------------------------------------------
    // Garis dasar mekanikal
    // --------------------------------------------------------

    oled.drawLine(
        2,
        baseY,
        125,
        baseY,
        SSD1306_WHITE
    );


    // ========================================================
    // NORMAL / STANDBY
    // ========================================================

    if (!speaking) {

        static const uint8_t normalPattern[24] = {

            2, 2, 5, 5, 5, 2,

            2, 4, 4, 2, 2, 5,

            5, 5, 2, 2, 4, 4,

            2, 2, 5, 5, 2, 2
        };


        for (
            int i = 0;
            i < 24;
            i++
        ) {

            int x =
                3 +
                (i * 5);


            if (
                x > 123
            ) {
                break;
            }


            uint8_t index =
                (
                    i +
                    oledMechanicalFrame
                ) % 24;


            int height =
                normalPattern[index];


            oled.drawLine(
                x,
                baseY - height,
                x + 3,
                baseY - height,
                SSD1306_WHITE
            );


            if (
                i < 23
            ) {

                int nextX =
                    x + 5;


                if (
                    nextX <= 125
                ) {

                    oled.drawLine(
                        x + 3,
                        baseY - height,
                        nextX,
                        baseY -
                        normalPattern[
                            (index + 1) % 24
                        ],
                        SSD1306_WHITE
                    );
                }
            }
        }


        // ----------------------------------------------------
        // Pulse mekanikal kecil
        // ----------------------------------------------------

        int pulseX =
            45 +
            (
                oledMechanicalFrame %
                17
            );


        oled.drawLine(
            pulseX,
            57,
            pulseX + 3,
            57,
            SSD1306_WHITE
        );


        return;
    }


    // ========================================================
    // SPEAKING
    // ========================================================

    for (
        int i = 0;
        i < 6;
        i++
    ) {

        int x =
            8 +
            (i * 22);


        int height =
            3;


        if (
            i ==
            (
                oledMechanicalFrame %
                6
            )
        ) {

            height =
                8;

        }
        else if (
            i ==
            (
                (
                    oledMechanicalFrame +
                    5
                ) % 6
            ) ||

            i ==
            (
                (
                    oledMechanicalFrame +
                    1
                ) % 6
            )
        ) {

            height =
                5;
        }


        oled.drawLine(
            x,
            baseY - height,
            x,
            baseY,
            SSD1306_WHITE
        );


        oled.drawLine(
            x - 3,
            baseY - height,
            x,
            baseY,
            SSD1306_WHITE
        );


        oled.drawLine(
            x,
            baseY,
            x + 3,
            baseY - height,
            SSD1306_WHITE
        );
    }


    // --------------------------------------------------------
    // Pulse tengah
    // --------------------------------------------------------

    int x =
        61 +
        (
            (
                oledMechanicalFrame %
                3
            ) - 1
        ) * 3;


    oled.drawLine(
        x,
        56,
        x,
        59,
        SSD1306_WHITE
    );
}


// ============================================================
// OLED READY
// ============================================================

void oledShowReady() {

    if (!oledReadyFlag) {
        return;
    }


    oled.clearDisplay();


    oledHeader(
        "READY"
    );


    oled.setCursor(
        3,
        27
    );

    oled.print(
        "WAITING FOR"
    );


    oled.setCursor(
        3,
        36
    );

    oled.print(
        "COMMAND..."
    );


    oledDrawMechanical(
        false
    );


    oled.display();


    oledLastAnim =
        millis();
}


// ============================================================
// OLED LISTENING
// ============================================================

void oledShowListening() {

    if (!oledReadyFlag) {
        return;
    }


    oled.clearDisplay();


    oledHeader(
        "LISTENING"
    );


    oled.setCursor(
        3,
        27
    );

    oled.print(
        "INPUT RECEIVED"
    );


    oled.setCursor(
        3,
        36
    );

    oled.print(
        "AWAITING QUERY"
    );


    oledDrawMechanical(
        false
    );


    oled.display();
}


// ============================================================
// OLED PROCESSING
// ============================================================

void oledShowProcessing() {

    if (!oledReadyFlag) {
        return;
    }


    oled.clearDisplay();


    oledHeader(
        "PROCESSING"
    );


    oled.setCursor(
        3,
        27
    );

    oled.print(
        "ANALYZING..."
    );


    oled.setCursor(
        3,
        36
    );

    oled.print(
        "GENERATING RESPONSE"
    );


    oledDrawMechanical(
        false
    );


    oled.display();
}


// ============================================================
// OLED ONLINE
// ============================================================

void oledShowOnline() {

    if (!oledReadyFlag) {
        return;
    }


    oled.clearDisplay();


    oledHeader(
        "ONLINE"
    );


    oled.setCursor(
        3,
        27
    );

    oled.print(
        "SYSTEM INITIALIZED"
    );


    oled.setCursor(
        3,
        36
    );

    oled.print(
        "A2DP : STANDBY"
    );


    oled.setCursor(
        3,
        45
    );

    oled.print(
        "VOICE: READY"
    );


    oledDrawMechanical(
        false
    );


    oled.display();
}


// ============================================================
// OLED READY ANIMATION
// ============================================================
//
// Struktur bawah terus bergerak saat TARS idle.
// ============================================================

void oledUpdateReadyAnimation() {

    if (!oledReadyFlag) {
        return;
    }


    // Tidak memakai playbackRunning di sini.
    // Variabel tersebut dideklarasikan setelah bagian OLED,
    // sehingga tidak diperlukan untuk animasi OLED.


    uint32_t now =
        millis();


    if (
        now -
        oledLastAnim <
        OLED_ANIM_INTERVAL
    ) {

        return;
    }


    oledLastAnim =
        now;


    oledMechanicalFrame++;


    oled.clearDisplay();


    oledHeader(
        "READY"
    );


    oled.setCursor(
        3,
        27
    );

    oled.print(
        "WAITING FOR"
    );


    oled.setCursor(
        3,
        36
    );

    oled.print(
        "COMMAND..."
    );


    oledDrawMechanical(
        false
    );


    oled.display();
}


// ============================================================
// OLED TYPING START
// ============================================================
//
// Teks hanya disiapkan.
// Belum ditampilkan.
//
// Tampilan jawaban dimulai ketika A2DP benar-benar
// sudah masuk AUDIO STARTED.
// ============================================================

void oledStartTyping(
    const String &text
) {

    if (!oledReadyFlag) {
        return;
    }


    oledAnswer =
        text;


    oledTypedChars =
        0;


    oledLastType =
        millis();


    oledMechanicalFrame =
        0;


    oledTyping =
        true;
}


// ============================================================
// OLED DRAW TYPED TEXT
// ============================================================

void oledDrawTypedText(
    bool speaking
) {

    if (!oledReadyFlag) {
        return;
    }


    oled.clearDisplay();


    oledHeader(
        speaking ?
        "SPEAKING" :
        "READY"
    );


    oled.setTextColor(
        SSD1306_WHITE
    );

    oled.setTextSize(
        1
    );


    String visible =
        oledAnswer.substring(
            0,
            oledTypedChars
        );


    // --------------------------------------------------------
    // Area jawaban
    //
    // 20 karakter × 4 baris.
    // --------------------------------------------------------

    const int startX =
        3;

    const int startY =
        23;

    const int maxChars =
        20;

    const int maxLines =
        4;


    int line =
        0;

    int column =
        0;


    oled.setCursor(
        startX,
        startY
    );


    oled.print(
        "> "
    );


    column =
        2;


    for (
        size_t i = 0;
        i < visible.length();
        i++
    ) {

        char c =
            visible[i];


        if (
            c == '\r'
        ) {
            continue;
        }


        if (
            c == '\n'
        ) {

            line++;

            column =
                0;


            if (
                line >=
                maxLines
            ) {
                break;
            }


            oled.setCursor(
                startX,
                startY +
                line * 9
            );


            continue;
        }


        if (
            column >=
            maxChars
        ) {

            line++;

            column =
                0;


            if (
                line >=
                maxLines
            ) {
                break;
            }


            oled.setCursor(
                startX,
                startY +
                line * 9
            );
        }


        oled.write(
            c
        );


        column++;
    }


    // --------------------------------------------------------
    // Struktur mekanikal
    // --------------------------------------------------------

    oledDrawMechanical(
        speaking
    );


    oled.display();
}


// ============================================================
// OLED TYPING UPDATE
// ============================================================
//
// Typing berjalan bersamaan dengan A2DP.
// ============================================================

void oledUpdateTyping(
    bool speaking
) {

    if (!oledReadyFlag) {
        return;
    }


    uint32_t now =
        millis();


    bool redraw =
        false;


    // --------------------------------------------------------
    // Typing
    // --------------------------------------------------------

    if (
        oledTyping &&

        now -
        oledLastType >=
        OLED_TYPE_INTERVAL
    ) {

        oledLastType =
            now;


        if (
            oledTypedChars <
            oledAnswer.length()
        ) {

            oledTypedChars++;

            redraw =
                true;

        }
        else {

            oledTyping =
                false;

            redraw =
                true;
        }
    }


    // --------------------------------------------------------
    // Struktur mekanikal saat berbicara
    // --------------------------------------------------------

    if (
        speaking &&

        now -
        oledLastAnim >=
        OLED_ANIM_INTERVAL
    ) {

        oledLastAnim =
            now;


        oledMechanicalFrame++;


        redraw =
            true;
    }


    if (redraw) {

        oledDrawTypedText(
            speaking
        );
    }
}


// ============================================================
// NETWORK
// ============================================================

static const char *WORKER_URL =
    "https://tars-cloud-v1.hilmane34.workers.dev";

static const char *ASK_URL =
    "https://tars-cloud-v1.hilmane34.workers.dev/ask";

static const char *TTS_URL =
    "https://tars-cloud-v1.hilmane34.workers.dev/tts";

static const char *MP3_PATH =
    "/tts.mp3";

static const char *NTP_SERVER_1 =
    "pool.ntp.org";

static const char *NTP_SERVER_2 =
    "time.nist.gov";

static const long GMT_OFFSET_SEC =
    7 * 3600;

static const int DAYLIGHT_OFFSET_SEC =
    0;

static const uint32_t WIFI_TIMEOUT_MS =
    15000;

static const uint32_t NTP_TIMEOUT_MS =
    15000;

static const uint32_t BT_TIMEOUT_MS =
    20000;

static const uint32_t PLAY_TIMEOUT_MS =
    120000;


// ============================================================
// AUDIO
// ============================================================

static const uint32_t INPUT_SAMPLE_RATE =
    22050;

static const uint32_t OUTPUT_SAMPLE_RATE =
    44100;

static const uint8_t INPUT_CHANNELS =
    1;

static const uint8_t OUTPUT_CHANNELS =
    2;

static const uint8_t BITS_PER_SAMPLE =
    16;


// ============================================================
// MEMORY
// ============================================================

static const size_t PCM_RING_SIZE =
    16384;

static const size_t MP3_COPY_BUFFER =
    1024;

static const size_t PCM_OUTPUT_CHUNK =
    1024;

static const float PCM_GAIN =
    2.0f;


// ============================================================
// A2DP TAIL
// ============================================================

static const uint32_t A2DP_TAIL_MS =
    1000;


// ============================================================
// BLUETOOTH
// ============================================================

static const char *BT_DEVICE_NAME =
    "I7-TWS";

BluetoothA2DPSource a2dpSource;


// ============================================================
// STATE
// ============================================================

volatile bool btConnected =
    false;

volatile bool btAudioStarted =
    false;

volatile bool playbackRunning =
    false;

volatile uint32_t btCallbackCalls =
    0;

bool ntpSynced =
    false;


// ============================================================
// HEAP
// ============================================================

void printHeap(
    const char *label
) {

    Serial.printf(
        "HEAP[%s]: free=%u largest=%u internal=%u\n",
        label,
        (unsigned)ESP.getFreeHeap(),
        (unsigned)ESP.getMaxAllocHeap(),
        (unsigned)heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL
        )
    );
}


// ============================================================
// PCM RING BUFFER
// ============================================================

class PCMRingBuffer {

private:

    uint8_t *buffer =
        nullptr;

    size_t capacity =
        0;

    volatile size_t readIndex =
        0;

    volatile size_t writeIndex =
        0;

    volatile size_t used =
        0;

    portMUX_TYPE mux =
        portMUX_INITIALIZER_UNLOCKED;


public:

    bool begin(
        size_t size
    ) {

        if (buffer != nullptr) {

            free(buffer);

            buffer =
                nullptr;
        }


        buffer =
            (uint8_t *)malloc(
                size
            );


        if (!buffer) {

            capacity =
                0;

            return false;
        }


        capacity =
            size;


        clear();


        return true;
    }


    void end() {

        if (buffer) {

            free(buffer);

            buffer =
                nullptr;
        }


        capacity =
            0;

        readIndex =
            0;

        writeIndex =
            0;

        used =
            0;
    }


    void clear() {

        portENTER_CRITICAL(
            &mux
        );


        readIndex =
            0;

        writeIndex =
            0;

        used =
            0;


        portEXIT_CRITICAL(
            &mux
        );
    }


    size_t available() {

        size_t value;


        portENTER_CRITICAL(
            &mux
        );


        value =
            used;


        portEXIT_CRITICAL(
            &mux
        );


        return value;
    }


    size_t freeSpace() {

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
    ) {

        if (
            !buffer ||
            !src ||
            len == 0
        ) {
            return 0;
        }


        size_t written =
            0;


        portENTER_CRITICAL(
            &mux
        );


        size_t freeBytes =
            capacity - used;


        if (
            len >
            freeBytes
        ) {
            len =
                freeBytes;
        }


        if (len > 0) {

            size_t first =
                capacity - writeIndex;


            if (
                first >
                len
            ) {
                first =
                    len;
            }


            memcpy(
                buffer + writeIndex,
                src,
                first
            );


            size_t second =
                len - first;


            if (
                second > 0
            ) {

                memcpy(
                    buffer,
                    src + first,
                    second
                );
            }


            writeIndex =
                (
                    writeIndex +
                    len
                ) % capacity;


            used +=
                len;


            written =
                len;
        }


        portEXIT_CRITICAL(
            &mux
        );


        return written;
    }


    size_t read(
        uint8_t *dst,
        size_t len
    ) {

        if (
            !buffer ||
            !dst ||
            len == 0
        ) {
            return 0;
        }


        size_t result =
            0;


        portENTER_CRITICAL(
            &mux
        );


        if (
            len >
            used
        ) {
            len =
                used;
        }


        if (len > 0) {

            size_t first =
                capacity - readIndex;


            if (
                first >
                len
            ) {
                first =
                    len;
            }


            memcpy(
                dst,
                buffer + readIndex,
                first
            );


            size_t second =
                len - first;


            if (
                second > 0
            ) {

                memcpy(
                    dst + first,
                    buffer,
                    second
                );
            }


            readIndex =
                (
                    readIndex +
                    len
                ) % capacity;


            used -=
                len;


            result =
                len;
        }


        portEXIT_CRITICAL(
            &mux
        );


        return result;
    }
};


PCMRingBuffer pcmRing;


// ============================================================
// PCM OUTPUT STREAM
// ============================================================

class PCMOutputStream : public AudioStream {

private:

    AudioInfo currentInfo;

    uint8_t outputBuffer[
        PCM_OUTPUT_CHUNK
    ];


    static int16_t applyGain(
        int16_t sample
    ) {

        int32_t value =
            (int32_t)(
                (float)sample *
                PCM_GAIN
            );


        if (
            value >
            32767
        ) {
            value =
                32767;
        }


        if (
            value <
            -32768
        ) {
            value =
                -32768;
        }


        return (
            int16_t)value
            ;
    }


public:

    void setAudioInfo(
        AudioInfo info
    ) override {

        currentInfo =
            info;


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


    int availableForWrite() override {

        size_t freeBytes =
            pcmRing.freeSpace();


        if (
            currentInfo.sample_rate ==
                INPUT_SAMPLE_RATE &&

            currentInfo.channels ==
                INPUT_CHANNELS &&

            currentInfo.bits_per_sample ==
                BITS_PER_SAMPLE
        ) {

            size_t inputCapacity =
                freeBytes / 4;


            if (
                inputCapacity >
                512
            ) {
                inputCapacity =
                    512;
            }


            return (
                int)inputCapacity;
        }


        return (
            int)min(
                freeBytes,
                (size_t)512
            );
    }


    size_t write(
        const uint8_t *data,
        size_t size
    ) override {

        if (
            !data ||
            size == 0
        ) {
            return 0;
        }


        if (
            currentInfo.sample_rate !=
                INPUT_SAMPLE_RATE ||

            currentInfo.channels !=
                INPUT_CHANNELS ||

            currentInfo.bits_per_sample !=
                BITS_PER_SAMPLE
        ) {

            Serial.printf(
                "PCM ERROR: unsupported %d Hz %d ch %d bit\n",
                currentInfo.sample_rate,
                currentInfo.channels,
                currentInfo.bits_per_sample
            );


            return 0;
        }


        size_t inputOffset =
            0;


        while (
            inputOffset <
            size
        ) {

            size_t remaining =
                size - inputOffset;


            size_t samples =
                remaining / 2;


            size_t maxSamples =
                sizeof(outputBuffer) / 8;


            if (
                samples >
                maxSamples
            ) {
                samples =
                    maxSamples;
            }


            if (
                samples == 0
            ) {
                break;
            }


            size_t requiredOutput =
                samples * 8;


            uint32_t waitStart =
                millis();


            while (
                pcmRing.freeSpace() <
                requiredOutput
            ) {

                oledUpdateTyping(
                    true
                );


                if (
                    millis() -
                    waitStart >
                    5000
                ) {

                    Serial.println(
                        "PCM ERROR: ring timeout"
                    );


                    return 0;
                }


                delay(2);
            }


            int16_t *inputSamples =
                (int16_t *)(
                    data +
                    inputOffset
                );


            int16_t *outputSamples =
                (int16_t *)(
                    outputBuffer
                );


            size_t outSampleIndex =
                0;


            for (
                size_t i = 0;
                i < samples;
                i++
            ) {

                int16_t sample =
                    applyGain(
                        inputSamples[i]
                    );


                outputSamples[
                    outSampleIndex++
                ] =
                    sample;

                outputSamples[
                    outSampleIndex++
                ] =
                    sample;


                outputSamples[
                    outSampleIndex++
                ] =
                    sample;

                outputSamples[
                    outSampleIndex++
                ] =
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


            inputOffset +=
                samples * 2;
        }


        return inputOffset;
    }
};


PCMOutputStream pcmOutput;


MP3DecoderHelix mp3Decoder;


EncodedAudioStream mp3Stream(
    &pcmOutput,
    &mp3Decoder
);


// ============================================================
// WIFI
// ============================================================

bool connectWiFi(
    bool requireTime = false
) {

    if (
        WiFi.status() ==
        WL_CONNECTED
    ) {

        if (
            requireTime &&
            !ntpSynced
        ) {

        }
        else {

            return true;
        }
    }


    Serial.println(
        "TARS: WiFi ON"
    );


    WiFi.mode(
        WIFI_STA
    );


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


    return true;
}


void disconnectWiFi() {

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
// NTP
// ============================================================

bool isTimeValid() {

    time_t now =
        time(nullptr);


    return (
        now >
        1577836800
    );
}


bool syncNTP() {

    Serial.println(
        "TARS: TIME INVALID - NTP REQUIRED"
    );


    Serial.println(
        "TARS: NTP START"
    );


    configTime(
        GMT_OFFSET_SEC,
        DAYLIGHT_OFFSET_SEC,
        NTP_SERVER_1,
        NTP_SERVER_2
    );


    uint32_t start =
        millis();


    int attempt =
        0;


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


        ntpSynced =
            false;


        return false;
    }


    time_t now =
        time(nullptr);


    struct tm timeInfo;


    localtime_r(
        &now,
        &timeInfo
    );


    Serial.printf(
        "TARS: NTP OK %04d-%02d-%02d %02d:%02d:%02d\n",

        timeInfo.tm_year + 1900,

        timeInfo.tm_mon + 1,

        timeInfo.tm_mday,

        timeInfo.tm_hour,

        timeInfo.tm_min,

        timeInfo.tm_sec
    );


    ntpSynced =
        true;


    return true;
}


bool ensureTimeValid() {

    if (
        ntpSynced &&
        isTimeValid()
    ) {

        return true;
    }


    if (
        isTimeValid()
    ) {

        ntpSynced =
            true;


        return true;
    }


    return syncNTP();
}


// ============================================================
// ASK AI
// ============================================================

String askAI(
    const String &question
) {

    if (
        !connectWiFi(true)
    ) {
        return "";
    }


    if (
        !ensureTimeValid()
    ) {
        return "";
    }


    oledShowProcessing();


    Serial.println(
        "TARS: POST /ask"
    );


    WiFiClientSecure client;


    client.setInsecure();


    HTTPClient http;


    if (
        !http.begin(
            client,
            ASK_URL
        )
    ) {

        Serial.println(
            "ASK HTTP: begin FAILED"
        );


        return "";
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


    int httpCode =
        http.POST(
            body
        );


    Serial.printf(
        "ASK HTTP: %d\n",
        httpCode
    );


    if (
        httpCode < 200 ||
        httpCode >= 300
    ) {

        http.end();


        return "";
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


    if (error) {

        Serial.println(
            "TARS: JSON ERROR"
        );


        return "";
    }


    String answer =
        json["response"] |
        "";


    Serial.println(
        "TARS RESPONSE:"
    );


    Serial.println(
        answer
    );


    return answer;
}


// ============================================================
// DOWNLOAD TTS
// ============================================================

bool downloadTTS(
    const String &text
) {

    if (
        !connectWiFi(true)
    ) {
        return false;
    }


    if (
        !ensureTimeValid()
    ) {
        return false;
    }


    Serial.printf(
        "TARS: TTS chars = %u\n",
        (unsigned)text.length()
    );


    Serial.println(
        "TARS: POST /tts"
    );


    WiFiClientSecure client;


    client.setInsecure();


    HTTPClient http;


    if (
        !http.begin(
            client,
            TTS_URL
        )
    ) {

        Serial.println(
            "TTS HTTP: begin FAILED"
        );


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


    int httpCode =
        http.POST(
            body
        );


    Serial.printf(
        "TTS HTTP: %d\n",
        httpCode
    );


    if (
        httpCode < 200 ||
        httpCode >= 300
    ) {

        http.end();


        return false;
    }


    int contentLength =
        http.getSize();


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
            "TARS: MP3 OPEN FAILED"
        );


        http.end();


        return false;
    }


    WiFiClient *stream =
        http.getStreamPtr();


    uint8_t buffer[
        1024
    ];


    size_t total =
        0;


    uint32_t lastData =
        millis();


    while (
        http.connected() &&
        (
            contentLength > 0 ||
            contentLength == -1
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


            int read =
                stream->readBytes(
                    buffer,
                    readSize
                );


            if (
                read > 0
            ) {

                file.write(
                    buffer,
                    read
                );


                total +=
                    read;


                lastData =
                    millis();


                if (
                    contentLength > 0
                ) {

                    contentLength -=
                        read;
                }
            }
        }
        else {

            if (
                millis() -
                lastData >
                5000
            ) {

                break;
            }


            delay(1);
        }
    }


    file.flush();

    file.close();

    http.end();


    Serial.printf(
        "TARS: MP3 bytes = %u\n",
        (unsigned)total
    );


    return (
        total > 0
    );
}


// ============================================================
// A2DP CALLBACK
// ============================================================

int32_t getAudioData(
    uint8_t *data,
    int32_t len
) {

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
// BLUETOOTH CALLBACKS
// ============================================================

void onBTConnectionState(
    esp_a2d_connection_state_t state,
    void *
) {

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

        btConnected =
            true;


        Serial.println(
            "TARS: A2DP CONNECTED"
        );
    }
    else {

        btConnected =
            false;
    }
}


void onBTAudioState(
    esp_a2d_audio_state_t state,
    void *
) {

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

        btAudioStarted =
            true;


        Serial.println(
            "TARS: A2DP AUDIO STARTED"
        );
    }
    else {

        btAudioStarted =
            false;
    }
}


// ============================================================
// START BLUETOOTH
// ============================================================

bool startBluetooth() {

    Serial.println(
        "TARS: Bluetooth START"
    );


    btConnected =
        false;


    btAudioStarted =
        false;


    btCallbackCalls =
        0;


    printHeap(
        "BEFORE_BT"
    );


    a2dpSource.set_event_queue_size(
        8
    );


    a2dpSource.set_event_stack_size(
        2048
    );


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


    Serial.println(
        "BT: event queue = 8"
    );


    Serial.println(
        "BT: event stack = 2048"
    );


    Serial.println(
        "BT: auto reconnect = OFF"
    );


    a2dpSource.start(
        BT_DEVICE_NAME
    );


    uint32_t start =
        millis();


    while (
        !btConnected &&

        millis() - start <
            BT_TIMEOUT_MS
    ) {

        delay(100);
    }


    if (
        !btConnected
    ) {

        Serial.println(
            "TARS: Bluetooth CONNECT FAILED"
        );


        printHeap(
            "BT_FAILED"
        );


        return false;
    }


    Serial.println(
        "TARS: Bluetooth READY"
    );


    delay(300);


    printHeap(
        "BT_READY"
    );


    return true;
}


// ============================================================
// STOP BLUETOOTH
// ============================================================

void stopBluetooth() {

    Serial.println(
        "TARS: Bluetooth STOP"
    );


    btAudioStarted =
        false;


    delay(100);


    a2dpSource.end(
        true
    );


    btConnected =
        false;


    delay(500);


    Serial.printf(
        "TARS: A2DP callbacks = %u\n",
        (unsigned)btCallbackCalls
    );


    printHeap(
        "AFTER_BT_STOP"
    );
}


// ============================================================
// PLAY MP3
// ============================================================

bool playMP3() {

    if (
        !LittleFS.exists(
            MP3_PATH
        )
    ) {

        Serial.println(
            "TARS: MP3 NOT FOUND"
        );


        return false;
    }


    File mp3File =
        LittleFS.open(
            MP3_PATH,
            FILE_READ
        );


    if (!mp3File) {

        Serial.println(
            "TARS: MP3 OPEN FAILED"
        );


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


        return false;
    }


    Serial.println(
        "TARS: PLAY START"
    );


    playbackRunning =
        true;


    pcmRing.clear();


    if (
        !startBluetooth()
    ) {

        mp3File.close();


        playbackRunning =
            false;


        return false;
    }


    printHeap(
        "AFTER_BT_BEFORE_HELIX"
    );


    mp3Decoder.setMaxPCMSize(
        4096
    );


    mp3Decoder.setMaxFrameSize(
        2048
    );


    Serial.println(
        "HELIX: max PCM = 4096"
    );


    Serial.println(
        "HELIX: max frame = 2048"
    );


    printHeap(
        "BEFORE_HELIX_BEGIN"
    );


    if (
        !mp3Stream.begin()
    ) {

        Serial.println(
            "TARS: MP3 DECODER START FAILED"
        );


        mp3File.close();


        stopBluetooth();


        playbackRunning =
            false;


        return false;
    }


    Serial.println(
        "TARS: MP3 DECODER READY"
    );


    printHeap(
        "AFTER_HELIX_BEGIN"
    );


    StreamCopy mp3Copier(
        mp3Stream,
        mp3File,
        MP3_COPY_BUFFER
    );


    mp3Copier.setCheckAvailableForWrite(
        false
    );


    mp3Copier.setCheckAvailable(
        true
    );


    uint32_t playStart =
        millis();


    uint32_t lastProgress =
        millis();


    size_t lastPosition =
        0;


    bool decoderFinished =
        false;


    // --------------------------------------------------------
    // Tunggu sampai A2DP benar-benar STARTED.
    //
    // Jawaban OLED belum ditampilkan sebelum audio
    // benar-benar mulai dikirim.
    // --------------------------------------------------------

    uint32_t oledAudioWaitStart =
        millis();


    while (
        !btAudioStarted &&
        btConnected &&
        millis() -
        oledAudioWaitStart <
        5000
    ) {

        delay(5);
    }


    // --------------------------------------------------------
    // A2DP benar-benar sudah STARTED.
    // Sekarang TARS mulai berbicara di OLED.
    // --------------------------------------------------------

    if (
        btAudioStarted
    ) {

        oledDrawTypedText(
            true
        );
    }


    // --------------------------------------------------------
    // Decode loop
    // --------------------------------------------------------

    while (
        millis() -
        playStart <
        PLAY_TIMEOUT_MS
    ) {

        // ----------------------------------------------------
        // OLED typing berjalan bersamaan dengan audio.
        // ----------------------------------------------------

        oledUpdateTyping(
            true
        );


        size_t position =
            mp3File.position();


        size_t availablePCM =
            pcmRing.available();


        size_t freePCM =
            pcmRing.freeSpace();


        if (
            millis() -
            lastProgress >
            1000
        ) {

            Serial.printf(
                "AUDIO: MP3=%u/%u PCM=%u/%u FREE=%u BT=%d AUDIO=%d CB=%u\n",

                (unsigned)position,

                (unsigned)mp3Size,

                (unsigned)availablePCM,

                (unsigned)PCM_RING_SIZE,

                (unsigned)freePCM,

                btConnected,

                btAudioStarted,

                (unsigned)btCallbackCalls
            );


            lastProgress =
                millis();


            if (
                position !=
                lastPosition
            ) {

                lastPosition =
                    position;
            }
        }


        if (
            !btConnected
        ) {

            Serial.println(
                "TARS: Bluetooth LOST"
            );


            break;
        }


        if (
            position >=
            mp3Size
        ) {

            decoderFinished =
                true;


            Serial.println(
                "TARS: MP3 EOF"
            );


            break;
        }


        if (
            freePCM <
            4096
        ) {

            delay(2);

            yield();

            continue;
        }


        size_t copied =
            mp3Copier.copy();


        if (
            copied == 0
        ) {

            delay(2);
        }
        else {

            yield();
        }
    }


    // --------------------------------------------------------
    // Stop decoder.
    // --------------------------------------------------------

    mp3Stream.end();


    mp3File.close();


    // --------------------------------------------------------
    // PCM DRAIN
    // --------------------------------------------------------

    Serial.println(
        "TARS: PCM DRAIN"
    );


    uint32_t drainStart =
        millis();


    while (
        pcmRing.available() > 0 &&

        millis() -
        drainStart <
        10000
    ) {

        oledUpdateTyping(
            true
        );


        delay(10);
    }


    if (
        pcmRing.available() > 0
    ) {

        Serial.printf(
            "TARS: PCM DRAIN TIMEOUT, remaining=%u\n",
            (unsigned)pcmRing.available()
        );
    }
    else {

        Serial.println(
            "TARS: PCM EMPTY"
        );
    }


    // --------------------------------------------------------
    // A2DP FINAL TAIL
    // --------------------------------------------------------

    Serial.printf(
        "TARS: A2DP TAIL %ums\n",
        (unsigned)A2DP_TAIL_MS
    );


    uint32_t tailStart =
        millis();


    uint32_t lastCallback =
        btCallbackCalls;


    while (
        millis() -
        tailStart <
        A2DP_TAIL_MS
    ) {

        oledUpdateTyping(
            true
        );


        delay(10);


        if (
            !btConnected
        ) {
            break;
        }


        if (
            btCallbackCalls !=
            lastCallback
        ) {

            lastCallback =
                btCallbackCalls;
        }
    }


    Serial.printf(
        "TARS: A2DP callbacks final = %u\n",
        (unsigned)btCallbackCalls
    );


    // --------------------------------------------------------
    // Baru sekarang Bluetooth dimatikan.
    // --------------------------------------------------------

    stopBluetooth();


    playbackRunning =
        false;


    if (
        !decoderFinished
    ) {

        Serial.println(
            "TARS: PLAYBACK TIMEOUT"
        );


        oledShowReady();


        return false;
    }


    // --------------------------------------------------------
    // Pastikan seluruh teks selesai.
    // --------------------------------------------------------

    while (
        oledTyping
    ) {

        oledUpdateTyping(
            false
        );

        delay(5);
    }


    oledShowReady();


    Serial.println(
        "TARS: PLAY DONE"
    );


    return true;
}


// ============================================================
// HANDLE QUESTION
// ============================================================

void handleQuestion(
    const String &question
) {

    if (
        question.length() == 0
    ) {
        return;
    }


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
    // OLED LISTENING
    // --------------------------------------------------------

    oledShowListening();


    // --------------------------------------------------------
    // WiFi
    // --------------------------------------------------------

    if (
        !connectWiFi(true)
    ) {

        Serial.println(
            "TARS: WIFI ERROR"
        );


        oledShowReady();


        return;
    }


    // --------------------------------------------------------
    // AI
    // --------------------------------------------------------

    String answer =
        askAI(
            question
        );


    if (
        answer.length() == 0
    ) {

        Serial.println(
            "TARS: AI EMPTY"
        );


        oledShowReady();


        return;
    }


    // --------------------------------------------------------
    // Siapkan teks untuk typing.
    // Belum ditampilkan.
    // --------------------------------------------------------

    oledStartTyping(
        answer
    );


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


        oledShowReady();


        return;
    }


    // --------------------------------------------------------
    // WiFi OFF sebelum Bluetooth.
    // --------------------------------------------------------

    disconnectWiFi();


    // --------------------------------------------------------
    // PLAY
    // --------------------------------------------------------

    playMP3();


    // --------------------------------------------------------
    // WiFi ON kembali.
    // --------------------------------------------------------

    if (
        connectWiFi(false)
    ) {

        if (
            !isTimeValid()
        ) {

            Serial.println(
                "TARS: TIME LOST - NTP REQUIRED"
            );


            syncNTP();
        }
    }


    oledShowReady();


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

void setup() {

    Serial.begin(
        115200
    );


    delay(1000);


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


    printHeap(
        "BOOT"
    );


    // --------------------------------------------------------
    // OLED
    // --------------------------------------------------------

    Wire.begin(
        21,
        22
    );


    if (
        oled.begin(
            SSD1306_SWITCHCAPVCC,
            OLED_ADDR
        )
    ) {

        oledReadyFlag =
            true;


        oledShowOnline();
    }


    // --------------------------------------------------------
    // LittleFS
    // --------------------------------------------------------

    if (
        !LittleFS.begin(true)
    ) {

        Serial.println(
            "LittleFS FAILED"
        );


        return;
    }


    Serial.println(
        "LittleFS OK"
    );


    printHeap(
        "AFTER_LITTLEFS"
    );


    // --------------------------------------------------------
    // PCM ring
    // --------------------------------------------------------

    if (
        !pcmRing.begin(
            PCM_RING_SIZE
        )
    ) {

        Serial.println(
            "PCM RING ALLOC FAILED"
        );


        return;
    }


    Serial.println(
        "PCM  : 16KB BUFFER"
    );


    Serial.println(
        "HELIX: 4096 PCM / 2048 FRAME"
    );


    Serial.println(
        "BT   : QUEUE 8 / STACK 2048"
    );


    Serial.println(
        "PRIME: DISABLED"
    );


    Serial.println(
        "A2DP : BT BEFORE DECODER"
    );


    printHeap(
        "AFTER_PCM_RING"
    );


    // --------------------------------------------------------
    // WiFi + NTP
    // --------------------------------------------------------

    if (
        !connectWiFi(true)
    ) {

        Serial.println(
            "TARS: INITIAL WIFI FAILED"
        );
    }


    if (
        WiFi.status() ==
            WL_CONNECTED &&

        !ntpSynced
    ) {

        ensureTimeValid();
    }


    printHeap(
        "READY"
    );


    oledShowReady();


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
// LOOP
// ============================================================

void loop() {

    // --------------------------------------------------------
    // Struktur OLED terus hidup saat TARS standby.
    // --------------------------------------------------------

    oledUpdateReadyAnimation();


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
