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

#include "BluetoothA2DPSource.h"
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"

#include "config.h"

// ============================================================
// TARS - MAIN
// ESP32 + WiFi + ElevenLabs MP3 + MP3 Helix + A2DP
// ============================================================

// ---------------- OLED ----------------

Adafruit_SSD1306 oled(
    OLED_WIDTH,
    OLED_HEIGHT,
    &Wire,
    -1
);

// ---------------- BLUETOOTH ----------------

BluetoothA2DPSource a2dpSource;

// ---------------- AUDIO ----------------

static const size_t PCM_BUFFER_SIZE = 32768;
static const size_t MP3_COPY_BUFFER = 1024;

static const uint32_t WIFI_TIMEOUT_MS = 15000;
static const uint32_t BT_TIMEOUT_MS = 20000;
static const uint32_t PLAY_TIMEOUT_MS = 120000;

static const float PCM_GAIN = 2.0f;

static const char *MP3_PATH = "/tts.mp3";

// ---------------- STATE ----------------

volatile bool btConnected = false;
volatile bool btAudioStarted = false;
volatile bool playbackRunning = false;

bool ntpSynced = false;

// ============================================================
// PCM RING BUFFER
// ============================================================

class PCMRingBuffer {

private:

    uint8_t *buffer;

    size_t capacity;
    volatile size_t readPos;
    volatile size_t writePos;
    volatile size_t used;

    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

public:

    PCMRingBuffer(size_t size)
    {
        capacity = size;
        buffer = (uint8_t *)malloc(capacity);

        readPos = 0;
        writePos = 0;
        used = 0;
    }

    ~PCMRingBuffer()
    {
        if (buffer) {
            free(buffer);
            buffer = nullptr;
        }
    }

    bool begin()
    {
        return buffer != nullptr;
    }

    void clear()
    {
        portENTER_CRITICAL(&mux);

        readPos = 0;
        writePos = 0;
        used = 0;

        portEXIT_CRITICAL(&mux);
    }

    size_t available()
    {
        size_t value;

        portENTER_CRITICAL(&mux);
        value = used;
        portEXIT_CRITICAL(&mux);

        return value;
    }

    size_t freeSpace()
    {
        size_t value;

        portENTER_CRITICAL(&mux);
        value = capacity - used;
        portEXIT_CRITICAL(&mux);

        return value;
    }

    size_t write(const uint8_t *src, size_t len)
    {
        if (!src || len == 0) {
            return 0;
        }

        portENTER_CRITICAL(&mux);

        size_t freeBytes = capacity - used;

        if (len > freeBytes) {
            len = freeBytes;
        }

        size_t first = capacity - writePos;

        if (first > len) {
            first = len;
        }

        memcpy(buffer + writePos, src, first);

        if (len > first) {
            memcpy(buffer, src + first, len - first);
        }

        writePos += len;

        if (writePos >= capacity) {
            writePos -= capacity;
        }

        used += len;

        portEXIT_CRITICAL(&mux);

        return len;
    }

    size_t read(uint8_t *dst, size_t len)
    {
        if (!dst || len == 0) {
            return 0;
        }

        portENTER_CRITICAL(&mux);

        if (len > used) {
            len = used;
        }

        size_t first = capacity - readPos;

        if (first > len) {
            first = len;
        }

        memcpy(dst, buffer + readPos, first);

        if (len > first) {
            memcpy(dst + first, buffer, len - first);
        }

        readPos += len;

        if (readPos >= capacity) {
            readPos -= capacity;
        }

        used -= len;

        portEXIT_CRITICAL(&mux);

        return len;
    }
};

PCMRingBuffer pcmRing(PCM_BUFFER_SIZE);

// ============================================================
// PCM OUTPUT STREAM
// MP3 decoder -> 44.1kHz stereo 16-bit PCM
// ============================================================

class PCMOutputStream : public AudioStream {

private:

    PCMRingBuffer &ring;

    AudioInfo currentInfo;

public:

    PCMOutputStream(PCMRingBuffer &r)
        : ring(r)
    {
    }

    void setAudioInfo(AudioInfo info)
    {
        currentInfo = info;

        Serial.print("PCM format: ");
        Serial.print(info.sample_rate);
        Serial.print(" Hz, ");
        Serial.print(info.channels);
        Serial.print(" ch, ");
        Serial.print(info.bits_per_sample);
        Serial.println(" bit");
    }

    AudioInfo audioInfo() override
    {
        return currentInfo;
    }

    size_t write(const uint8_t *data, size_t len) override
    {
        if (!data || len == 0) {
            return 0;
        }

        const int16_t *samples = (const int16_t *)data;

        size_t inputSamples = len / 2;

        if (inputSamples == 0) {
            return 0;
        }

        uint8_t outputBuffer[2048];

        size_t outputBytes = 0;

        int channels = currentInfo.channels;
        int sampleRate = currentInfo.sample_rate;

        if (channels != 1 && channels != 2) {
            return 0;
        }

        for (size_t i = 0; i < inputSamples; ) {

            int16_t left;
            int16_t right;

            if (channels == 1) {

                int32_t s = samples[i++];

                s = (int32_t)(s * PCM_GAIN);

                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;

                left = (int16_t)s;
                right = (int16_t)s;

            } else {

                int32_t l = samples[i++];
                int32_t r = 0;

                if (i < inputSamples) {
                    r = samples[i++];
                }

                l = (int32_t)(l * PCM_GAIN);
                r = (int32_t)(r * PCM_GAIN);

                if (l > 32767) l = 32767;
                if (l < -32768) l = -32768;

                if (r > 32767) r = 32767;
                if (r < -32768) r = -32768;

                left = (int16_t)l;
                right = (int16_t)r;
            }

            if (sampleRate == 22050) {

                // 22050 -> 44100
                // duplicate sample once

                for (int repeat = 0; repeat < 2; repeat++) {

                    if (outputBytes + 4 > sizeof(outputBuffer)) {

                        size_t freeBytes = ring.freeSpace();

                        if (freeBytes < outputBytes) {
                            return 0;
                        }

                        ring.write(outputBuffer, outputBytes);

                        outputBytes = 0;
                    }

                    memcpy(
                        outputBuffer + outputBytes,
                        &left,
                        2
                    );

                    outputBytes += 2;

                    memcpy(
                        outputBuffer + outputBytes,
                        &right,
                        2
                    );

                    outputBytes += 2;
                }

            } else {

                if (outputBytes + 4 > sizeof(outputBuffer)) {

                    size_t freeBytes = ring.freeSpace();

                    if (freeBytes < outputBytes) {
                        return 0;
                    }

                    ring.write(outputBuffer, outputBytes);

                    outputBytes = 0;
                }

                memcpy(
                    outputBuffer + outputBytes,
                    &left,
                    2
                );

                outputBytes += 2;

                memcpy(
                    outputBuffer + outputBytes,
                    &right,
                    2
                );

                outputBytes += 2;
            }
        }

        if (outputBytes > 0) {

            size_t freeBytes = ring.freeSpace();

            if (freeBytes < outputBytes) {
                return 0;
            }

            ring.write(outputBuffer, outputBytes);
        }

        return len;
    }

    int availableForWrite() override
    {
        return ring.freeSpace();
    }
};

PCMOutputStream pcmOutput(pcmRing);

// ============================================================
// MP3 DECODER
// ============================================================

MP3DecoderHelix mp3Decoder;

EncodedAudioStream mp3Stream(
    &pcmOutput,
    &mp3Decoder
);

StreamCopy mp3Copier(
    mp3Stream,
    MP3_COPY_BUFFER
);

// ============================================================
// OLED
// ============================================================

void oledText(const char *text)
{
    oled.clearDisplay();

    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    oled.setCursor(0, 0);
    oled.println(text);

    oled.display();
}

// ============================================================
// WIFI
// ============================================================

bool connectWiFi()
{
    Serial.println("TARS: WiFi ON");

    WiFi.mode(WIFI_STA);

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("TARS: IP = ");
        Serial.println(WiFi.localIP());
        return true;
    }

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t start = millis();

    while (
        WiFi.status() != WL_CONNECTED &&
        millis() - start < WIFI_TIMEOUT_MS
    ) {

        delay(250);
        Serial.print(".");
    }

    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {

        Serial.println("TARS: WiFi FAILED");

        return false;
    }

    Serial.print("TARS: IP = ");
    Serial.println(WiFi.localIP());

    return true;
}

void disconnectWiFi()
{
    Serial.println("TARS: WiFi OFF");

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    delay(200);
}

// ============================================================
// NTP
// SYNC ONLY ONCE
// ============================================================

bool syncNTPOnce()
{
    if (ntpSynced) {
        return true;
    }

    Serial.println("TARS: NTP START");

    configTime(
        7 * 3600,
        0,
        "pool.ntp.org",
        "time.nist.gov",
        "time.google.com"
    );

    for (int attempt = 1; attempt <= 4; attempt++) {

        Serial.print("TARS: NTP attempt ");
        Serial.print(attempt);
        Serial.println("/4");

        time_t now = time(nullptr);

        if (now > 1700000000) {

            ntpSynced = true;

            struct tm timeinfo;

            localtime_r(&now, &timeinfo);

            Serial.printf(
                "TARS: NTP OK %04d-%02d-%02d %02d:%02d:%02d\n",
                timeinfo.tm_year + 1900,
                timeinfo.tm_mon + 1,
                timeinfo.tm_mday,
                timeinfo.tm_hour,
                timeinfo.tm_min,
                timeinfo.tm_sec
            );

            return true;
        }

        delay(1500);
    }

    Serial.println("TARS: NTP FAILED");

    return false;
}

// ============================================================
// ASK AI
// ============================================================

bool askAI(
    const String &question,
    String &answer
)
{
    WiFiClientSecure client;

    client.setInsecure();

    HTTPClient http;

    String url = String(WORKER_URL) + "/ask";

    Serial.println("TARS: POST /ask");

    if (!http.begin(client, url)) {

        Serial.println("ASK HTTP BEGIN FAILED");

        return false;
    }

    http.addHeader(
        "Content-Type",
        "application/json"
    );

    JsonDocument requestDoc;

    requestDoc["text"] = question;

    String requestBody;

    serializeJson(
        requestDoc,
        requestBody
    );

    int code = http.POST(requestBody);

    Serial.print("ASK HTTP: ");
    Serial.println(code);

    if (code != 200) {

        http.end();

        return false;
    }

    String payload = http.getString();

    http.end();

    JsonDocument responseDoc;

    DeserializationError err =
        deserializeJson(
            responseDoc,
            payload
        );

    if (err) {

        Serial.print("ASK JSON ERROR: ");
        Serial.println(err.c_str());

        return false;
    }

    answer =
        responseDoc["response"] |
        "";

    Serial.println("TARS RESPONSE:");
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
    String ttsText = text;

    if (ttsText.length() > 450) {
        ttsText = ttsText.substring(0, 450);
    }

    Serial.print("TARS: TTS chars = ");
    Serial.println(ttsText.length());

    WiFiClientSecure client;

    client.setInsecure();

    HTTPClient http;

    String url = String(WORKER_URL) + "/tts";

    Serial.println("TARS: POST /tts");

    if (!http.begin(client, url)) {

        Serial.println("TTS HTTP BEGIN FAILED");

        return false;
    }

    http.addHeader(
        "Content-Type",
        "application/json"
    );

    JsonDocument requestDoc;

    requestDoc["text"] = ttsText;

    String requestBody;

    serializeJson(
        requestDoc,
        requestBody
    );

    int code = http.POST(requestBody);

    Serial.print("TTS HTTP: ");
    Serial.println(code);

    if (code != 200) {

        http.end();

        return false;
    }

    int totalLength =
        http.getSize();

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

    uint8_t buffer[1024];

    size_t total = 0;

    uint32_t start = millis();

    while (
        http.connected() &&
        (totalLength > 0 ||
         totalLength == -1)
    ) {

        size_t available =
            stream->available();

        if (available) {

            size_t readSize =
                available;

            if (readSize > sizeof(buffer)) {
                readSize = sizeof(buffer);
            }

            int len =
                stream->readBytes(
                    buffer,
                    readSize
                );

            if (len > 0) {

                file.write(
                    buffer,
                    len
                );

                total += len;

                if (totalLength > 0) {
                    totalLength -= len;
                }
            }
        }

        if (millis() - start > 30000) {
            break;
        }

        delay(1);
    }

    file.close();

    http.end();

    Serial.print("TARS: MP3 bytes = ");
    Serial.println(total);

    return total > 100;
}

// ============================================================
// A2DP CALLBACK
// ============================================================

int32_t getAudioData(
    uint8_t *data,
    int32_t len
)
{
    if (!data || len <= 0) {
        return 0;
    }

    size_t got =
        pcmRing.read(
            data,
            len
        );

    // A2DP must always receive requested
    // PCM bytes. Missing audio becomes silence.

    if (got < (size_t)len) {

        memset(
            data + got,
            0,
            len - got
        );
    }

    return len;
}

// ============================================================
// A2DP CONNECTION CALLBACK
// ============================================================

void onBTConnectionState(
    esp_a2d_connection_state_t state,
    void *obj
)
{
    Serial.print("TARS: A2DP STATE = ");

    Serial.println(
        a2dpSource.to_str(state)
    );

    if (
        state ==
        ESP_A2D_CONNECTION_STATE_CONNECTED
    ) {

        btConnected = true;

        Serial.println(
            "TARS: A2DP CONNECTED"
        );

    } else {

        btConnected = false;
    }
}

// ============================================================
// A2DP AUDIO STATE
// ============================================================

void onBTAudioState(
    esp_a2d_audio_state_t state,
    void *obj
)
{
    Serial.print("TARS: A2DP AUDIO = ");

    Serial.println(
        a2dpSource.to_str(state)
    );

    if (
        state ==
        ESP_A2D_AUDIO_STATE_STARTED
    ) {

        btAudioStarted = true;

    } else {

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
        millis() - start < BT_TIMEOUT_MS
    ) {

        delay(50);
        yield();
    }

    if (!btConnected) {

        Serial.println(
            "TARS: Bluetooth TIMEOUT"
        );

        return false;
    }

    Serial.println(
        "TARS: Bluetooth READY"
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

    btConnected = false;
    btAudioStarted = false;

    a2dpSource.end(true);

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
    File file =
        LittleFS.open(
            MP3_PATH,
            FILE_READ
        );

    if (!file) {

        Serial.println(
            "TARS: MP3 OPEN FAILED"
        );

        return false;
    }

    Serial.println(
        "TARS: MP3 DECODER READY"
    );

    pcmRing.clear();

    mp3Stream.setNotifyAudioChange(
        false
    );

    if (!mp3Stream.begin()) {

        Serial.println(
            "TARS: MP3 STREAM BEGIN FAILED"
        );

        file.close();

        return false;
    }

    file.close();

    return true;
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
    // IMPORTANT:
    // Bluetooth starts BEFORE MP3 decoding.
    // --------------------------------------------------------

    if (!startBluetooth()) {

        playbackRunning = false;

        stopBluetooth();

        return false;
    }

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

    Serial.println(
        "TARS: MP3 DECODER START"
    );

    if (!mp3Stream.begin()) {

        Serial.println(
            "TARS: MP3 STREAM BEGIN FAILED"
        );

        mp3File.close();

        playbackRunning = false;

        stopBluetooth();

        return false;
    }

    uint32_t start =
        millis();

    size_t lastPCM = 0;

    while (
        millis() - start < PLAY_TIMEOUT_MS
    ) {

        // ----------------------------------------------------
        // Feed MP3 decoder only while BT is alive.
        // ----------------------------------------------------

        if (!btConnected) {

            Serial.println(
                "TARS: BT LOST"
            );

            break;
        }

        size_t copied =
            mp3Copier.copy();

        size_t pcmNow =
            pcmRing.available();

        if (pcmNow != lastPCM) {

            lastPCM = pcmNow;
        }

        // ----------------------------------------------------
        // EOF
        // ----------------------------------------------------

        if (
            mp3File.position() >=
            mp3File.size()
        ) {

            // Give A2DP time to drain
            // the remaining PCM.

            uint32_t drainStart =
                millis();

            while (
                pcmRing.available() > 0 &&
                millis() - drainStart < 5000
            ) {

                delay(10);
                yield();
            }

            break;
        }

        // ----------------------------------------------------
        // If decoder could not write because buffer is full,
        // allow A2DP callback to consume it.
        // ----------------------------------------------------

        if (copied == 0) {
            delay(2);
        }

        yield();
    }

    mp3File.close();

    Serial.println(
        "TARS: MP3 DECODER STOP"
    );

    playbackRunning = false;

    delay(300);

    stopBluetooth();

    LittleFS.remove(
        MP3_PATH
    );

    Serial.println(
        "TARS: PLAY FINISHED"
    );

    return true;
}

// ============================================================
// COMMAND HANDLER
// ============================================================

void handleQuestion(
    const String &question
)
{
    Serial.println(
        "TARS: COMMAND RECEIVED"
    );

    Serial.println(
        "QUESTION:"
    );

    Serial.println(
        question
    );

    if (WiFi.status() != WL_CONNECTED) {

        if (!connectWiFi()) {
            return;
        }
    }

    String answer;

    if (!askAI(
        question,
        answer
    )) {

        Serial.println(
            "TARS: ASK FAILED"
        );

        return;
    }

    if (!downloadTTS(
        answer
    )) {

        Serial.println(
            "TARS: TTS FAILED"
        );

        return;
    }

    // --------------------------------------------------------
    // WiFi OFF before Bluetooth
    // --------------------------------------------------------

    disconnectWiFi();

    // --------------------------------------------------------
    // Bluetooth + MP3 playback
    // --------------------------------------------------------

    bool played =
        playMP3();

    if (!played) {

        Serial.println(
            "TARS: PLAY FAILED"
        );
    }

    // --------------------------------------------------------
    // WiFi back ON
    // --------------------------------------------------------

    if (!connectWiFi()) {

        Serial.println(
            "TARS: WiFi RECONNECT FAILED"
        );

        return;
    }

    Serial.println(
        "TARS READY"
    );

    Serial.println(
        "Ketik pertanyaan:"
    );
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(115200);

    delay(500);

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

    if (!oled.begin(
        SSD1306_SWITCHCAPVCC,
        OLED_ADDR
    )) {

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
    // LITTLEFS
    // --------------------------------------------------------

    if (!LittleFS.begin(true)) {

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
    // PCM BUFFER
    // --------------------------------------------------------

    if (!pcmRing.begin()) {

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

    Serial.println(
        "PCM : 32KB SAFE BUFFER"
    );

    Serial.println(
        "PRIME: DISABLED"
    );

    Serial.println(
        "A2DP : BT BEFORE DECODER"
    );

    // --------------------------------------------------------
    // WIFI
    // --------------------------------------------------------

    oledText(
        "WIFI..."
    );

    if (!connectWiFi()) {

        oledText(
            "WIFI ERROR"
        );

        while (true) {
            delay(1000);
        }
    }

    // --------------------------------------------------------
    // NTP ONLY ONCE
    // --------------------------------------------------------

    Serial.println(
        "TARS: NTP START"
    );

    syncNTPOnce();

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
        "PCM  : 32KB SAFE BUFFER"
    );

    Serial.println(
        "PRIME: DISABLED"
    );

    Serial.println(
        "A2DP : BT BEFORE DECODER"
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
            Serial.readStringUntil('\n');

        question.trim();

        if (question.length() == 0) {
            return;
        }

        handleQuestion(
            question
        );
    }

    delay(10);
}
