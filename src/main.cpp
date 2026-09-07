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
// TARS CONFIG
// ============================================================

#define WORKER_URL "https://tars-cloud-v1.hilmane34.workers.dev"

#define BT_HEADSET_NAME "I7-TWS"

#define MP3_PATH "/tts.mp3"

#define WIFI_TIMEOUT_MS     15000
#define BT_TIMEOUT_MS       20000
#define PLAY_TIMEOUT_MS     120000
#define NTP_TIMEOUT_MS      15000

#define PCM_BUFFER_SIZE     32768
#define MP3_COPY_BUFFER     1024

#define PCM_GAIN            2.0f


// ============================================================
// OLED
// ============================================================

#define OLED_WIDTH  128
#define OLED_HEIGHT 64

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

volatile bool btConnected = false;
volatile bool btAudioStarted = false;
volatile bool playbackRunning = false;

volatile uint32_t btCallbackCalls = 0;


// ============================================================
// NTP
// ============================================================

bool ntpSynced = false;


// ============================================================
// PCM RING BUFFER
// ============================================================

class PCMRingBuffer {

private:

    uint8_t *buffer = nullptr;

    size_t capacity = 0;
    volatile size_t readPos = 0;
    volatile size_t writePos = 0;

    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;


public:

    bool begin(size_t size) {

        capacity = size;

        buffer = (uint8_t *)malloc(capacity);

        if (!buffer) {
            capacity = 0;
            return false;
        }

        readPos = 0;
        writePos = 0;

        memset(buffer, 0, capacity);

        return true;
    }


    void clear() {

        portENTER_CRITICAL(&mux);

        readPos = 0;
        writePos = 0;

        portEXIT_CRITICAL(&mux);
    }


    size_t available() {

        if (capacity == 0)
            return 0;

        portENTER_CRITICAL(&mux);

        size_t r = readPos;
        size_t w = writePos;

        size_t result;

        if (w >= r)
            result = w - r;
        else
            result = capacity - r + w;

        portEXIT_CRITICAL(&mux);

        return result;
    }


    size_t freeSpace() {

        if (capacity == 0)
            return 0;

        return capacity - available() - 1;
    }


    size_t write(
        const uint8_t *data,
        size_t len
    ) {

        if (!buffer || !data || len == 0)
            return 0;

        size_t written = 0;

        portENTER_CRITICAL(&mux);

        size_t r = readPos;
        size_t w = writePos;

        size_t used;

        if (w >= r)
            used = w - r;
        else
            used = capacity - r + w;

        size_t freeBytes =
            capacity - used - 1;

        size_t toWrite =
            min(len, freeBytes);


        while (written < toWrite) {

            buffer[w] =
                data[written];

            w++;

            if (w >= capacity)
                w = 0;

            written++;
        }


        writePos = w;

        portEXIT_CRITICAL(&mux);

        return written;
    }


    size_t read(
        uint8_t *data,
        size_t len
    ) {

        if (!buffer || !data || len == 0)
            return 0;

        size_t readCount = 0;

        portENTER_CRITICAL(&mux);

        size_t r = readPos;
        size_t w = writePos;

        size_t availableBytes;

        if (w >= r)
            availableBytes = w - r;
        else
            availableBytes = capacity - r + w;

        size_t toRead =
            min(len, availableBytes);


        while (readCount < toRead) {

            data[readCount] =
                buffer[r];

            r++;

            if (r >= capacity)
                r = 0;

            readCount++;
        }


        readPos = r;

        portEXIT_CRITICAL(&mux);

        return readCount;
    }
};


PCMRingBuffer pcmRing;


// ============================================================
// PCM OUTPUT STREAM
// ============================================================

class PCMOutputStream : public AudioStream {

private:

    AudioInfo currentInfo;

    static const size_t OUT_BUFFER_SIZE = 2048;

    uint8_t outBuffer[
        OUT_BUFFER_SIZE
    ];


public:

    AudioInfo audioInfo() override {
        return currentInfo;
    }


    AudioInfo audioInfoOut() override {
        return currentInfo;
    }


    bool begin() override {

        currentInfo =
            AudioInfo();

        return true;
    }


    void end() override {
    }


    void setAudioInfo(
        AudioInfo info
    ) override {

        currentInfo = info;

        Serial.printf(
            "PCM format: %d Hz, %d ch, %d bit\n",
            info.sample_rate,
            info.channels,
            info.bits_per_sample
        );
    }


    // --------------------------------------------------------
    // Conservative input/output ratio.
    //
    // 22050 mono -> 44100 stereo = 4x
    //
    // We intentionally report an even more conservative
    // factor to StreamCopy so the ring buffer cannot be
    // overrun by decoder expansion.
    // --------------------------------------------------------

    size_t safetyFactor() const {

        if (
            currentInfo.sample_rate == 22050 &&
            currentInfo.channels == 1
        )
            return 8;


        if (
            currentInfo.sample_rate == 22050 &&
            currentInfo.channels == 2
        )
            return 4;


        if (
            currentInfo.sample_rate == 44100 &&
            currentInfo.channels == 1
        )
            return 4;


        if (
            currentInfo.sample_rate == 44100 &&
            currentInfo.channels == 2
        )
            return 2;


        return 8;
    }


    int availableForWrite() override {

        size_t freeBytes =
            pcmRing.freeSpace();


        size_t safeInput =
            freeBytes / safetyFactor();


        if (safeInput > 16384)
            safeInput = 16384;


        return (int)safeInput;
    }


    size_t write(
        const uint8_t *data,
        size_t len
    ) override {

        if (!data || len == 0)
            return 0;


        if (
            currentInfo.bits_per_sample != 16
        )
            return 0;


        // ====================================================
        // 22050 MONO -> 44100 STEREO
        // ====================================================

        if (
            currentInfo.sample_rate == 22050 &&
            currentInfo.channels == 1
        ) {

            size_t samples =
                len / 2;


            size_t required =
                samples * 8;


            if (
                pcmRing.freeSpace() <
                required
            )
                return 0;


            size_t outPos = 0;

            const int16_t *samplesIn =
                (const int16_t *)data;


            for (
                size_t i = 0;
                i < samples;
                i++
            ) {

                int32_t s =
                    samplesIn[i];


                s =
                    (int32_t)(
                        (float)s *
                        PCM_GAIN
                    );


                if (s > 32767)
                    s = 32767;

                if (s < -32768)
                    s = -32768;


                int16_t sample =
                    (int16_t)s;


                // 22050 -> 44100
                // mono -> stereo

                for (
                    int repeat = 0;
                    repeat < 2;
                    repeat++
                ) {

                    outBuffer[outPos++] =
                        sample & 0xFF;

                    outBuffer[outPos++] =
                        (sample >> 8) & 0xFF;

                    outBuffer[outPos++] =
                        sample & 0xFF;

                    outBuffer[outPos++] =
                        (sample >> 8) & 0xFF;


                    if (
                        outPos >=
                        OUT_BUFFER_SIZE - 8
                    ) {

                        size_t written =
                            pcmRing.write(
                                outBuffer,
                                outPos
                            );


                        if (
                            written !=
                            outPos
                        )
                            return 0;


                        outPos = 0;
                    }
                }
            }


            if (outPos > 0) {

                size_t written =
                    pcmRing.write(
                        outBuffer,
                        outPos
                    );


                if (
                    written !=
                    outPos
                )
                    return 0;
            }


            return len;
        }


        // ====================================================
        // 22050 STEREO -> 44100 STEREO
        // ====================================================

        if (
            currentInfo.sample_rate == 22050 &&
            currentInfo.channels == 2
        ) {

            size_t frames =
                len / 4;


            size_t required =
                frames * 8;


            if (
                pcmRing.freeSpace() <
                required
            )
                return 0;


            size_t outPos = 0;

            const int16_t *samplesIn =
                (const int16_t *)data;


            for (
                size_t i = 0;
                i < frames;
                i++
            ) {

                int32_t left =
                    samplesIn[i * 2];

                int32_t right =
                    samplesIn[i * 2 + 1];


                left =
                    (int32_t)(
                        (float)left *
                        PCM_GAIN
                    );


                right =
                    (int32_t)(
                        (float)right *
                        PCM_GAIN
                    );


                if (left > 32767)
                    left = 32767;

                if (left < -32768)
                    left = -32768;


                if (right > 32767)
                    right = 32767;

                if (right < -32768)
                    right = -32768;


                int16_t l =
                    (int16_t)left;

                int16_t r =
                    (int16_t)right;


                for (
                    int repeat = 0;
                    repeat < 2;
                    repeat++
                ) {

                    outBuffer[outPos++] =
                        l & 0xFF;

                    outBuffer[outPos++] =
                        (l >> 8) & 0xFF;

                    outBuffer[outPos++] =
                        r & 0xFF;

                    outBuffer[outPos++] =
                        (r >> 8) & 0xFF;


                    if (
                        outPos >=
                        OUT_BUFFER_SIZE - 8
                    ) {

                        size_t written =
                            pcmRing.write(
                                outBuffer,
                                outPos
                            );


                        if (
                            written !=
                            outPos
                        )
                            return 0;


                        outPos = 0;
                    }
                }
            }


            if (outPos > 0) {

                size_t written =
                    pcmRing.write(
                        outBuffer,
                        outPos
                    );


                if (
                    written !=
                    outPos
                )
                    return 0;
            }


            return len;
        }


        // ====================================================
        // 44100 MONO -> 44100 STEREO
        // ====================================================

        if (
            currentInfo.sample_rate == 44100 &&
            currentInfo.channels == 1
        ) {

            size_t samples =
                len / 2;


            size_t required =
                samples * 4;


            if (
                pcmRing.freeSpace() <
                required
            )
                return 0;


            size_t outPos = 0;

            const int16_t *samplesIn =
                (const int16_t *)data;


            for (
                size_t i = 0;
                i < samples;
                i++
            ) {

                int32_t s =
                    samplesIn[i];


                s =
                    (int32_t)(
                        (float)s *
                        PCM_GAIN
                    );


                if (s > 32767)
                    s = 32767;

                if (s < -32768)
                    s = -32768;


                int16_t sample =
                    (int16_t)s;


                outBuffer[outPos++] =
                    sample & 0xFF;

                outBuffer[outPos++] =
                    (sample >> 8) & 0xFF;

                outBuffer[outPos++] =
                    sample & 0xFF;

                outBuffer[outPos++] =
                    (sample >> 8) & 0xFF;


                if (
                    outPos >=
                    OUT_BUFFER_SIZE - 4
                ) {

                    size_t written =
                        pcmRing.write(
                            outBuffer,
                            outPos
                        );


                    if (
                        written !=
                        outPos
                    )
                        return 0;


                    outPos = 0;
                }
            }


            if (outPos > 0) {

                size_t written =
                    pcmRing.write(
                        outBuffer,
                        outPos
                    );


                if (
                    written !=
                    outPos
                )
                    return 0;
            }


            return len;
        }


        // ====================================================
        // 44100 STEREO
        // ====================================================

        if (
            currentInfo.sample_rate == 44100 &&
            currentInfo.channels == 2
        ) {

            size_t required =
                len;


            if (
                pcmRing.freeSpace() <
                required
            )
                return 0;


            size_t processed = 0;


            while (
                processed < len
            ) {

                size_t chunk =
                    min(
                        len - processed,
                        OUT_BUFFER_SIZE
                    );


                memcpy(
                    outBuffer,
                    data + processed,
                    chunk
                );


                int16_t *samples =
                    (int16_t *)outBuffer;


                size_t sampleCount =
                    chunk / 2;


                for (
                    size_t i = 0;
                    i < sampleCount;
                    i++
                ) {

                    int32_t s =
                        (int32_t)(
                            (float)samples[i] *
                            PCM_GAIN
                        );


                    if (s > 32767)
                        s = 32767;

                    if (s < -32768)
                        s = -32768;


                    samples[i] =
                        (int16_t)s;
                }


                size_t written =
                    pcmRing.write(
                        outBuffer,
                        chunk
                    );


                if (
                    written !=
                    chunk
                )
                    return 0;


                processed += chunk;
            }


            return len;
        }


        return 0;
    }
};


PCMOutputStream pcmOutput;

MP3DecoderHelix mp3Decoder;

EncodedAudioStream mp3Stream(
    &pcmOutput,
    &mp3Decoder
);


// ============================================================
// BLUETOOTH DATA CALLBACK
// ============================================================

int32_t getAudioData(
    uint8_t *data,
    int32_t len
) {

    if (!data || len <= 0)
        return 0;


    btCallbackCalls++;


    size_t got =
        pcmRing.read(
            data,
            len
        );


    if (
        got < (size_t)len
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

void onBluetoothConnection(
    esp_a2d_connection_state_t state,
    void *
) {

    switch (state) {

        case ESP_A2D_CONNECTION_STATE_CONNECTING:

            Serial.println(
                "TARS: A2DP STATE = Connecting"
            );

            break;


        case ESP_A2D_CONNECTION_STATE_CONNECTED:

            btConnected = true;

            Serial.println(
                "TARS: A2DP STATE = Connected"
            );

            break;


        case ESP_A2D_CONNECTION_STATE_DISCONNECTED:

            btConnected = false;

            Serial.println(
                "TARS: A2DP STATE = Disconnected"
            );

            break;


        default:
            break;
    }
}


// ============================================================
// BLUETOOTH AUDIO STATE CALLBACK
// ============================================================

void onBluetoothAudioState(
    esp_a2d_audio_state_t state,
    void *
) {

    if (
        state ==
        ESP_A2D_AUDIO_STATE_STARTED
    ) {

        btAudioStarted = true;

        Serial.println(
            "TARS: A2DP AUDIO = Started"
        );

    } else {

        btAudioStarted = false;

        Serial.println(
            "TARS: A2DP AUDIO = Stopped"
        );
    }
}


// ============================================================
// CHECK SYSTEM TIME
//
// Tidak melakukan NTP otomatis setiap WiFi reconnect.
// Hanya mengecek apakah waktu sudah valid.
// ============================================================

bool isTimeValid() {

    time_t now =
        time(nullptr);


    // 2020-01-01
    return now > 1577836800;
}


// ============================================================
// NTP SYNC
//
// Bisa dipanggil jika waktu belum valid.
// ============================================================

bool syncNTP() {

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
        millis() - start <
        NTP_TIMEOUT_MS
    ) {

        attempt++;


        Serial.printf(
            "TARS: NTP attempt %d\n",
            attempt
        );


        struct tm timeinfo;


        if (
            getLocalTime(
                &timeinfo,
                2500
            )
        ) {

            if (
                timeinfo.tm_year >
                120
            ) {

                ntpSynced = true;


                char timeBuffer[32];


                strftime(
                    timeBuffer,
                    sizeof(timeBuffer),
                    "%Y-%m-%d %H:%M:%S",
                    &timeinfo
                );


                Serial.print(
                    "TARS: NTP OK "
                );


                Serial.println(
                    timeBuffer
                );


                return true;
            }
        }


        delay(100);
    }


    Serial.println(
        "TARS: NTP FAILED"
    );


    ntpSynced = false;


    return false;
}


// ============================================================
// ENSURE TIME
//
// HTTPS hanya boleh dijalankan kalau waktu valid.
//
// Kalau waktu masih valid setelah WiFi OFF/ON,
// TIDAK melakukan NTP lagi.
//
// Kalau waktu tidak valid, baru NTP.
// ============================================================

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

        ntpSynced = true;

        return true;
    }


    Serial.println(
        "TARS: TIME INVALID - NTP REQUIRED"
    );


    return syncNTP();
}


// ============================================================
// WIFI CONNECT
//
// requireTime = true:
//   pastikan waktu valid sebelum HTTPS.
//
// requireTime = false:
//   hanya konek WiFi.
// ============================================================

bool connectWiFi(
    bool requireTime = false
) {

    if (
        WiFi.status() ==
        WL_CONNECTED
    ) {

        if (
            requireTime
        ) {

            return ensureTimeValid();
        }


        return true;
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

        delay(100);
    }


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


    // --------------------------------------------------------
    // Hanya pastikan NTP kalau pemanggil memang membutuhkan
    // waktu valid untuk HTTPS.
    // --------------------------------------------------------

    if (
        requireTime
    ) {

        if (
            !ensureTimeValid()
        ) {

            Serial.println(
                "TARS: TIME SYNC FAILED"
            );

            return false;
        }
    }


    return true;
}


// ============================================================
// WIFI OFF
// ============================================================

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


    delay(200);
}


// ============================================================
// ASK AI
// ============================================================

bool askAI(
    const String &question,
    String &answer
) {

    if (
        !connectWiFi(true)
    )
        return false;


    WiFiClientSecure client;

    client.setInsecure();


    HTTPClient http;


    String url =
        String(WORKER_URL) +
        "/ask";


    Serial.println(
        "TARS: POST /ask"
    );


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


    JsonDocument doc;


    doc["text"] =
        question;


    String body;


    serializeJson(
        doc,
        body
    );


    int code =
        http.POST(
            body
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


    String response =
        http.getString();


    http.end();


    JsonDocument result;


    DeserializationError err =
        deserializeJson(
            result,
            response
        );


    if (err) {

        Serial.println(
            "ASK JSON ERROR"
        );

        return false;
    }


    answer =
        result["response"] |
        "";


    Serial.println(
        "TARS RESPONSE:"
    );


    Serial.println(
        answer
    );


    return answer.length() > 0;
}


// ============================================================
// DOWNLOAD TTS
// ============================================================

bool downloadTTS(
    const String &answer
) {

    // --------------------------------------------------------
    // TTS membutuhkan HTTPS.
    // Pastikan WiFi DAN waktu valid.
    // --------------------------------------------------------

    if (
        !connectWiFi(true)
    )
        return false;


    Serial.print(
        "TARS: TTS chars = "
    );


    Serial.println(
        answer.length()
    );


    WiFiClientSecure client;

    client.setInsecure();


    HTTPClient http;


    String url =
        String(WORKER_URL) +
        "/tts";


    Serial.println(
        "TARS: POST /tts"
    );


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


    JsonDocument doc;


    doc["text"] =
        answer;


    String body;


    serializeJson(
        doc,
        body
    );


    int code =
        http.POST(
            body
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


    File file =
        LittleFS.open(
            MP3_PATH,
            "w"
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


    uint8_t buffer[
        MP3_COPY_BUFFER
    ];


    size_t total = 0;


    uint32_t lastData =
        millis();


    while (
        http.connected() ||
        stream->available()
    ) {

        size_t available =
            stream->available();


        if (
            available > 0
        ) {

            size_t readSize =
                min(
                    available,
                    sizeof(buffer)
                );


            int count =
                stream->readBytes(
                    buffer,
                    readSize
                );


            if (
                count > 0
            ) {

                file.write(
                    buffer,
                    count
                );


                total += count;


                lastData =
                    millis();
            }

        } else {

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


    file.close();


    http.end();


    Serial.print(
        "TARS: MP3 bytes = "
    );


    Serial.println(
        total
    );


    return total > 100;
}


// ============================================================
// BLUETOOTH START
// ============================================================

bool startBluetooth() {

    btConnected = false;

    btAudioStarted = false;

    btCallbackCalls = 0;


    Serial.println(
        "TARS: Bluetooth START"
    );


    a2dpSource.set_auto_reconnect(
        false
    );


    a2dpSource.set_data_callback(
        getAudioData
    );


    a2dpSource.set_on_connection_state_changed(
        onBluetoothConnection
    );


    a2dpSource.set_on_audio_state_changed(
        onBluetoothAudioState
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

        delay(10);

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
        "TARS: A2DP CONNECTED"
    );


    Serial.println(
        "TARS: Bluetooth READY"
    );


    return true;
}


// ============================================================
// BLUETOOTH STOP
// ============================================================

void stopBluetooth() {

    Serial.println(
        "TARS: Bluetooth STOP"
    );


    playbackRunning =
        false;


    btAudioStarted =
        false;


    btConnected =
        false;


    a2dpSource.end(
        true
    );


    delay(300);
}


// ============================================================
// PLAY MP3
// ============================================================

bool playMP3() {

    Serial.println(
        "TARS: PLAY START"
    );


    pcmRing.clear();


    if (
        !startBluetooth()
    ) {

        Serial.println(
            "TARS: PLAY ABORT - BT"
        );


        return false;
    }


    File mp3File =
        LittleFS.open(
            MP3_PATH,
            "r"
        );


    if (!mp3File) {

        Serial.println(
            "TARS: MP3 OPEN FAILED"
        );


        stopBluetooth();


        return false;
    }


    Serial.print(
        "TARS: MP3 SIZE = "
    );


    Serial.println(
        mp3File.size()
    );


    // --------------------------------------------------------
    // Decoder reset
    // --------------------------------------------------------

    mp3Decoder.end();


    if (
        !mp3Stream.begin()
    ) {

        Serial.println(
            "TARS: MP3 DECODER BEGIN FAILED"
        );


        mp3File.close();

        stopBluetooth();


        return false;
    }


    Serial.println(
        "TARS: MP3 DECODER READY"
    );


    // --------------------------------------------------------
    // StreamCopy
    // --------------------------------------------------------

    StreamCopy mp3Copier(
        mp3Stream,
        mp3File,
        MP3_COPY_BUFFER
    );


    mp3Copier.setCheckAvailableForWrite(
        true
    );


    mp3Copier.setCheckAvailable(
        true
    );


    playbackRunning =
        true;


    uint32_t startTime =
        millis();


    uint32_t lastCallbackCheck =
        millis();


    uint32_t lastCallbackCount =
        btCallbackCalls;


    bool callbackReported =
        false;


    bool eof =
        false;


    // ========================================================
    // PLAY LOOP
    // ========================================================

    while (
        playbackRunning &&
        millis() - startTime <
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


        // ----------------------------------------------------
        // Monitor callback A2DP
        // ----------------------------------------------------

        if (
            !callbackReported &&
            millis() -
            lastCallbackCheck >=
            1000
        ) {

            uint32_t calls =
                btCallbackCalls;


            if (
                calls >
                lastCallbackCount
            ) {

                Serial.print(
                    "TARS: A2DP DATA ACTIVE = "
                );


                Serial.println(
                    calls
                );


                callbackReported =
                    true;

            } else {

                Serial.println(
                    "TARS: A2DP DATA CALLBACK NOT ACTIVE"
                );
            }


            lastCallbackCount =
                calls;


            lastCallbackCheck =
                millis();
        }


        // ----------------------------------------------------
        // Back-pressure
        // ----------------------------------------------------

        size_t freePCM =
            pcmRing.freeSpace();


        if (
            freePCM < 8192
        ) {

            delay(2);

            yield();

            continue;
        }


        // ----------------------------------------------------
        // MP3 -> decoder -> PCM
        // ----------------------------------------------------

        size_t copied =
            mp3Copier.copy();


        if (
            copied == 0
        ) {

            if (
                mp3File.position() >=
                mp3File.size()
            ) {

                eof =
                    true;


                Serial.println(
                    "TARS: MP3 EOF"
                );


                break;
            }


            delay(1);

            yield();


            continue;
        }


        yield();
    }


    // ========================================================
    // PCM DRAIN
    // ========================================================

    Serial.println(
        "TARS: PCM DRAIN"
    );


    uint32_t drainStart =
        millis();


    while (
        pcmRing.available() > 0 &&
        millis() -
        drainStart < 7000
    ) {

        delay(5);

        yield();
    }


    size_t remaining =
        pcmRing.available();


    Serial.print(
        "TARS: PCM REMAIN = "
    );


    Serial.println(
        remaining
    );


    // Beri waktu frame terakhir keluar
    delay(250);


    mp3File.close();


    mp3Stream.end();

    mp3Decoder.end();


    // ========================================================
    // BLUETOOTH OFF
    // ========================================================

    stopBluetooth();


    // ========================================================
    // DELETE MP3
    // ========================================================

    if (
        LittleFS.exists(
            MP3_PATH
        )
    ) {

        LittleFS.remove(
            MP3_PATH
        );
    }


    Serial.println(
        "TARS: PLAY END"
    );


    return eof;
}


// ============================================================
// HANDLE QUESTION
// ============================================================

void handleQuestion(
    const String &question
) {

    Serial.println(
        "TARS: COMMAND RECEIVED"
    );


    Serial.println(
        "QUESTION:"
    );


    Serial.println(
        question
    );


    String answer;


    // --------------------------------------------------------
    // WiFi + waktu valid
    // --------------------------------------------------------

    if (
        !connectWiFi(true)
    ) {

        Serial.println(
            "TARS: WIFI/TIME ERROR"
        );


        return;
    }


    // --------------------------------------------------------
    // ASK
    // --------------------------------------------------------

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
    // WiFi OFF sebelum Bluetooth
    // --------------------------------------------------------

    disconnectWiFi();


    // --------------------------------------------------------
    // PLAY
    // --------------------------------------------------------

    playMP3();


    // --------------------------------------------------------
    // WiFi ON lagi
    //
    // TIDAK NTP LAGI.
    // Waktu sistem tetap berjalan.
    // --------------------------------------------------------

    connectWiFi(false);


    Serial.println(
        "TARS: READY"
    );
}


// ============================================================
// SETUP
// ============================================================

void setup() {

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
        21,
        22
    );


    if (
        oled.begin(
            SSD1306_SWITCHCAPVCC,
            0x3C
        )
    ) {

        oled.clearDisplay();


        oled.setTextSize(
            1
        );


        oled.setTextColor(
            SSD1306_WHITE
        );


        oled.setCursor(
            0,
            0
        );


        oled.println(
            "TARS ONLINE"
        );


        oled.display();
    }


    // --------------------------------------------------------
    // LITTLEFS
    // --------------------------------------------------------

    if (
        !LittleFS.begin(
            true
        )
    ) {

        Serial.println(
            "LittleFS FAILED"
        );


        while (true) {
            delay(1000);
        }
    }


    Serial.println(
        "LittleFS OK"
    );


    // --------------------------------------------------------
    // PCM
    // --------------------------------------------------------

    if (
        !pcmRing.begin(
            PCM_BUFFER_SIZE
        )
    ) {

        Serial.println(
            "PCM BUFFER FAILED"
        );


        while (true) {
            delay(1000);
        }
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
    // WIFI + NTP BOOT
    // --------------------------------------------------------

    if (
        !connectWiFi(true)
    ) {

        Serial.println(
            "TARS: INITIAL WIFI/TIME FAILED"
        );
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
