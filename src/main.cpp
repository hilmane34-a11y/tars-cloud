#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Wire.h>
#include <time.h>
#include <ArduinoJson.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <BluetoothA2DPSource.h>

#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"

#include "config.h"


// ============================================================
// TARS ESP32
// MP3 22050 Hz MONO
//        ↓
// Helix PCM 22050 Hz MONO
//        ↓
// 2x resample
//        ↓
// PCM 44100 Hz STEREO
//        ↓
// Ring Buffer
//        ↓
// ESP32 A2DP Source
//        ↓
// I7-TWS
// ============================================================


// -----------------------------
// OLED
// -----------------------------

#define OLED_WIDTH   128
#define OLED_HEIGHT   64
#define OLED_ADDR   0x3C

Adafruit_SSD1306 oled(
    OLED_WIDTH,
    OLED_HEIGHT,
    &Wire,
    -1
);


// -----------------------------
// NETWORK
// -----------------------------

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

static const long GMT_OFFSET_SEC = 7 * 3600;
static const int DAYLIGHT_OFFSET_SEC = 0;

static const uint32_t WIFI_TIMEOUT_MS = 15000;
static const uint32_t NTP_TIMEOUT_MS = 15000;
static const uint32_t BT_TIMEOUT_MS = 20000;
static const uint32_t PLAY_TIMEOUT_MS = 120000;


// -----------------------------
// AUDIO
// -----------------------------

static const uint32_t INPUT_SAMPLE_RATE  = 22050;
static const uint32_t OUTPUT_SAMPLE_RATE = 44100;

static const uint8_t INPUT_CHANNELS  = 1;
static const uint8_t OUTPUT_CHANNELS = 2;

static const uint8_t BITS_PER_SAMPLE = 16;


// 32 KB final PCM ring buffer.
// Karena output adalah 44100 stereo 16-bit:
//
// 44100 * 2 channel * 2 byte
// = 176400 byte/detik
//
// 32768 byte ≈ 186 ms audio.
//
// Cukup untuk smoothing antara decoder dan A2DP.
static const size_t PCM_RING_SIZE = 32768;


// MP3 copy buffer.
// Jangan terlalu besar karena ESP32 tanpa PSRAM.
static const size_t MP3_COPY_BUFFER = 1024;


// Decoder maksimum PCM satu frame.
// 1152 mono samples * 2 byte = 2304 byte input PCM.
//
// Setelah 2x + stereo:
// 1152 * 2 * 2 * 2 = 9216 byte output.
//
// Namun Helix dapat memberikan blok yang berbeda.
// Kita beri ruang ring secara bertahap.
static const size_t PCM_OUTPUT_CHUNK = 2048;


// Gain.
static const float PCM_GAIN = 2.0f;


// -----------------------------
// BLUETOOTH
// -----------------------------

static const char *BT_DEVICE_NAME =
    "I7-TWS";

BluetoothA2DPSource a2dpSource;


// -----------------------------
// STATE
// -----------------------------

volatile bool btConnected = false;
volatile bool btAudioStarted = false;
volatile bool playbackRunning = false;

volatile uint32_t btCallbackCalls = 0;

bool ntpSynced = false;


// ============================================================
// PCM RING BUFFER
// ============================================================

class PCMRingBuffer {

private:

    uint8_t *buffer = nullptr;

    size_t capacity = 0;

    volatile size_t readIndex = 0;
    volatile size_t writeIndex = 0;
    volatile size_t used = 0;

    portMUX_TYPE mux =
        portMUX_INITIALIZER_UNLOCKED;


public:

    bool begin(size_t size) {

        if (buffer != nullptr) {
            free(buffer);
            buffer = nullptr;
        }

        buffer = (uint8_t *)malloc(size);

        if (!buffer) {
            capacity = 0;
            return false;
        }

        capacity = size;

        clear();

        return true;
    }


    void end() {

        if (buffer) {
            free(buffer);
            buffer = nullptr;
        }

        capacity = 0;
        readIndex = 0;
        writeIndex = 0;
        used = 0;
    }


    void clear() {

        portENTER_CRITICAL(&mux);

        readIndex = 0;
        writeIndex = 0;
        used = 0;

        portEXIT_CRITICAL(&mux);
    }


    size_t available() {

        size_t value;

        portENTER_CRITICAL(&mux);

        value = used;

        portEXIT_CRITICAL(&mux);

        return value;
    }


    size_t freeSpace() {

        size_t value;

        portENTER_CRITICAL(&mux);

        value = capacity - used;

        portEXIT_CRITICAL(&mux);

        return value;
    }


    size_t write(
        const uint8_t *src,
        size_t len
    ) {

        if (!buffer || !src || len == 0) {
            return 0;
        }

        size_t written = 0;

        portENTER_CRITICAL(&mux);

        size_t freeBytes =
            capacity - used;

        if (len > freeBytes) {
            len = freeBytes;
        }

        if (len > 0) {

            size_t first =
                capacity - writeIndex;

            if (first > len) {
                first = len;
            }

            memcpy(
                buffer + writeIndex,
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

            writeIndex =
                (writeIndex + len) % capacity;

            used += len;

            written = len;
        }

        portEXIT_CRITICAL(&mux);

        return written;
    }


    size_t read(
        uint8_t *dst,
        size_t len
    ) {

        if (!buffer || !dst || len == 0) {
            return 0;
        }

        size_t result = 0;

        portENTER_CRITICAL(&mux);

        if (len > used) {
            len = used;
        }

        if (len > 0) {

            size_t first =
                capacity - readIndex;

            if (first > len) {
                first = len;
            }

            memcpy(
                dst,
                buffer + readIndex,
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

            readIndex =
                (readIndex + len) % capacity;

            used -= len;

            result = len;
        }

        portEXIT_CRITICAL(&mux);

        return result;
    }
};


PCMRingBuffer pcmRing;


// ============================================================
// PCM OUTPUT STREAM
//
// Input:
//   22050 Hz
//   mono
//   16 bit
//
// Output:
//   44100 Hz
//   stereo
//   16 bit
//
// 1 input sample:
//
//   L(sample)
//   R(sample)
//
// kemudian sample yang sama diulang:
//
//   frame 1 = L/R current
//   frame 2 = L/R current
//
// sehingga:
//   22050 → 44100
// ============================================================

class PCMOutputStream : public AudioStream {

private:

    AudioInfo currentInfo;

    uint8_t outputBuffer[PCM_OUTPUT_CHUNK];


    static int16_t applyGain(
        int16_t sample
    ) {

        int32_t value =
            (int32_t)(
                (float)sample *
                PCM_GAIN
            );

        if (value > 32767) {
            value = 32767;
        }

        if (value < -32768) {
            value = -32768;
        }

        return (int16_t)value;
    }


public:

    void setAudioInfo(
        AudioInfo info
    ) override {

        currentInfo = info;

        AudioStream::setAudioInfo(info);

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


        // Untuk 22050 mono → 44100 stereo:
        //
        // 1 input byte PCM
        // menjadi 4 output bytes.
        //
        // Kita sisakan sedikit margin.
        if (
            currentInfo.sample_rate == INPUT_SAMPLE_RATE &&
            currentInfo.channels == INPUT_CHANNELS &&
            currentInfo.bits_per_sample == BITS_PER_SAMPLE
        ) {

            size_t inputCapacity =
                freeBytes / 4;

            if (inputCapacity > 1024) {
                inputCapacity = 1024;
            }

            return (int)inputCapacity;
        }


        // Safety fallback.
        return (int)min(
            freeBytes,
            (size_t)1024
        );
    }


    size_t write(
        const uint8_t *data,
        size_t size
    ) override {

        if (!data || size == 0) {
            return 0;
        }


        // ----------------------------------------------------
        // Kita hanya menerima:
        //
        // 22050 Hz
        // mono
        // 16 bit
        // ----------------------------------------------------

        if (
            currentInfo.sample_rate != INPUT_SAMPLE_RATE ||
            currentInfo.channels != INPUT_CHANNELS ||
            currentInfo.bits_per_sample != BITS_PER_SAMPLE
        ) {

            Serial.printf(
                "PCM ERROR: unsupported %d Hz %d ch %d bit\n",
                currentInfo.sample_rate,
                currentInfo.channels,
                currentInfo.bits_per_sample
            );

            return 0;
        }


        size_t inputBytes =
            size;

        size_t inputOffset =
            0;


        // ----------------------------------------------------
        // Process PCM in small chunks.
        // ----------------------------------------------------

        while (inputOffset < inputBytes) {

            size_t remaining =
                inputBytes - inputOffset;

            size_t samples =
                remaining / 2;


            // output:
            //
            // 1 mono sample
            // -> 2 stereo frames
            //
            // 2 frames × 2 channels × 2 bytes
            // = 8 bytes
            //

            size_t maxSamples =
                sizeof(outputBuffer) / 8;


            if (samples > maxSamples) {
                samples = maxSamples;
            }


            if (samples == 0) {
                break;
            }


            size_t requiredOutput =
                samples * 8;


            // ------------------------------------------------
            // IMPORTANT:
            //
            // Jangan pernah return 0 hanya karena ring
            // sedang penuh.
            //
            // Tunggu sampai A2DP mengonsumsi data.
            // ------------------------------------------------

            uint32_t waitStart =
                millis();


            while (
                pcmRing.freeSpace() <
                requiredOutput
            ) {

                if (
                    millis() - waitStart >
                    5000
                ) {

                    Serial.println(
                        "PCM ERROR: ring buffer timeout"
                    );

                    return 0;
                }

                delay(2);
            }


            int16_t *inputSamples =
                (int16_t *)(data + inputOffset);

            int16_t *outputSamples =
                (int16_t *)outputBuffer;


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


                // 22050 → 44100
                //
                // duplicate temporal sample
                //

                // frame 1
                outputSamples[outSampleIndex++] =
                    sample;

                outputSamples[outSampleIndex++] =
                    sample;


                // frame 2
                outputSamples[outSampleIndex++] =
                    sample;

                outputSamples[outSampleIndex++] =
                    sample;
            }


            size_t written =
                pcmRing.write(
                    outputBuffer,
                    requiredOutput
                );


            if (written != requiredOutput) {

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


// ============================================================
// MP3 HELIX
// ============================================================

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

    if (WiFi.status() == WL_CONNECTED) {

        if (
            requireTime &&
            !ntpSynced
        ) {
            // continue below
        } else {
            return true;
        }
    }


    Serial.println(
        "TARS: WiFi ON"
    );


    WiFi.mode(WIFI_STA);

    WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
    );


    uint32_t start =
        millis();


    while (
        WiFi.status() != WL_CONNECTED &&
        millis() - start < WIFI_TIMEOUT_MS
    ) {

        delay(250);

        Serial.print(".");
    }


    Serial.println();


    if (WiFi.status() != WL_CONNECTED) {

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

    WiFi.disconnect(true);

    WiFi.mode(WIFI_OFF);

    delay(300);
}


// ============================================================
// NTP
// ============================================================

bool isTimeValid() {

    time_t now =
        time(nullptr);

    return (
        now > 1577836800
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

    int attempt = 0;


    while (
        !isTimeValid() &&
        millis() - start < NTP_TIMEOUT_MS
    ) {

        attempt++;

        Serial.printf(
            "TARS: NTP attempt %d\n",
            attempt
        );

        delay(1000);
    }


    if (!isTimeValid()) {

        Serial.println(
            "TARS: NTP FAILED"
        );

        ntpSynced = false;

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


    ntpSynced = true;

    return true;
}


bool ensureTimeValid() {

    if (
        ntpSynced &&
        isTimeValid()
    ) {
        return true;
    }


    if (isTimeValid()) {

        ntpSynced = true;

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

    if (!connectWiFi(true)) {
        return "";
    }


    if (!ensureTimeValid()) {
        return "";
    }


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
        http.POST(body);


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

    if (!connectWiFi(true)) {
        return false;
    }


    if (!ensureTimeValid()) {
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
        http.POST(body);


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


        if (available > 0) {

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


            if (read > 0) {

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


    return total > 0;
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

        btConnected = true;

        Serial.println(
            "TARS: A2DP CONNECTED"
        );
    }
    else {

        btConnected = false;
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

bool startBluetooth() {

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


    if (!btConnected) {

        Serial.println(
            "TARS: Bluetooth CONNECT FAILED"
        );

        return false;
    }


    Serial.println(
        "TARS: Bluetooth READY"
    );


    // Give A2DP stack a little time to
    // create/start its audio path.
    delay(300);


    return true;
}


// ============================================================
// STOP BLUETOOTH
// ============================================================

void stopBluetooth() {

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


    if (mp3Size == 0) {

        mp3File.close();

        return false;
    }


    Serial.println(
        "TARS: PLAY START"
    );


    playbackRunning = true;


    // --------------------------------------------------------
    // Clear old PCM data.
    // --------------------------------------------------------

    pcmRing.clear();


    // --------------------------------------------------------
    // Start Bluetooth FIRST.
    // --------------------------------------------------------

    if (!startBluetooth()) {

        mp3File.close();

        playbackRunning = false;

        return false;
    }


    // --------------------------------------------------------
    // Configure Helix memory.
    // --------------------------------------------------------

    mp3Decoder.setMaxPCMSize(
        4608
    );

    mp3Decoder.setMaxFrameSize(
        4608
    );


    // --------------------------------------------------------
    // Start decoder.
    // --------------------------------------------------------

    if (!mp3Stream.begin()) {

        Serial.println(
            "TARS: MP3 DECODER START FAILED"
        );

        mp3File.close();

        stopBluetooth();

        playbackRunning = false;

        return false;
    }


    Serial.println(
        "TARS: MP3 DECODER READY"
    );


    // --------------------------------------------------------
    // StreamCopy
    //
    // We DO NOT let StreamCopy immediately return zero
    // because the output ring is temporarily full.
    //
    // PCMOutputStream::write() itself waits for space.
    // --------------------------------------------------------

    StreamCopy mp3Copier(
        mp3Stream,
        mp3File,
        MP3_COPY_BUFFER
    );


    // We still enable availability checking so StreamCopy
    // doesn't blindly push unlimited input into the decoder.
    mp3Copier.setCheckAvailableForWrite(
        true
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
    // Decode loop
    // --------------------------------------------------------

    while (
        millis() - playStart <
        PLAY_TIMEOUT_MS
    ) {

        size_t position =
            mp3File.position();


        size_t availablePCM =
            pcmRing.available();


        size_t freePCM =
            pcmRing.freeSpace();


        // Debug progress every ~1 second.
        if (
            millis() -
            lastProgress >
            1000
        ) {

            Serial.printf(
                "AUDIO: MP3=%u/%u PCM=%u/%u BT=%d AUDIO=%d CB=%u\n",
                (unsigned)position,
                (unsigned)mp3Size,
                (unsigned)availablePCM,
                (unsigned)PCM_RING_SIZE,
                btConnected,
                btAudioStarted,
                (unsigned)btCallbackCalls
            );


            lastProgress =
                millis();


            if (
                position != lastPosition
            ) {
                lastPosition =
                    position;
            }
        }


        // ----------------------------------------------------
        // MP3 EOF
        // ----------------------------------------------------

        if (
            position >= mp3Size
        ) {

            decoderFinished = true;

            Serial.println(
                "TARS: MP3 EOF"
            );

            break;
        }


        // ----------------------------------------------------
        // If output buffer is nearly full,
        // allow A2DP to consume it.
        // ----------------------------------------------------

        if (
            freePCM < 4096
        ) {

            delay(2);

            continue;
        }


        size_t copied =
            mp3Copier.copy();


        if (copied == 0) {

            delay(2);
        }
        else {

            yield();
        }
    }


    // --------------------------------------------------------
    // Decoder finished reading MP3.
    // --------------------------------------------------------

    mp3Stream.end();


    mp3File.close();


    // --------------------------------------------------------
    // IMPORTANT:
    //
    // Don't stop Bluetooth immediately.
    //
    // Let A2DP consume the remaining PCM.
    // --------------------------------------------------------

    Serial.println(
        "TARS: PCM DRAIN"
    );


    uint32_t drainStart =
        millis();


    while (
        pcmRing.available() > 0 &&
        millis() - drainStart < 10000
    ) {

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


    Serial.printf(
        "TARS: A2DP callbacks final = %u\n",
        (unsigned)btCallbackCalls
    );


    stopBluetooth();


    playbackRunning = false;


    if (!decoderFinished) {

        Serial.println(
            "TARS: PLAYBACK TIMEOUT"
        );

        return false;
    }


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
    // WiFi ON
    // --------------------------------------------------------

    if (!connectWiFi(true)) {

        Serial.println(
            "TARS: WIFI ERROR"
        );

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
    // Bluetooth playback needs WiFi OFF.
    // --------------------------------------------------------

    disconnectWiFi();


    // --------------------------------------------------------
    // PLAY
    // --------------------------------------------------------

    playMP3();


    // --------------------------------------------------------
    // WiFi ON again.
    //
    // IMPORTANT:
    // No NTP here unless time became invalid.
    // --------------------------------------------------------

    if (connectWiFi(false)) {

        if (
            !isTimeValid()
        ) {

            Serial.println(
                "TARS: TIME LOST - NTP REQUIRED"
            );

            syncNTP();
        }
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
// OLED BASIC
// ============================================================

void oledReady() {

    oled.clearDisplay();

    oled.setTextColor(
        SSD1306_WHITE
    );

    oled.setTextSize(1);

    oled.setCursor(
        0,
        0
    );

    oled.println(
        "TARS ONLINE"
    );

    oled.println();

    oled.println(
        "Menunggu pertanyaan..."
    );

    oled.display();
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

        oledReady();
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
        "PCM  : 32KB SAFE BUFFER"
    );

    Serial.println(
        "PRIME: DISABLED"
    );

    Serial.println(
        "A2DP : BT BEFORE DECODER"
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
        WiFi.status() == WL_CONNECTED &&
        !ntpSynced
    ) {

        ensureTimeValid();
    }


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
