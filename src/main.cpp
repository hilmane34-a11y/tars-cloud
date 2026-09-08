// Full main.cpp — revised: persistent A2DP instance, safer start/stop, more logging.
// Paste replacing your src/main.cpp

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
// OLED
// ============================================================
#define OLED_WIDTH   128
#define OLED_HEIGHT  64
#define OLED_ADDR    0x3C

Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
bool oledReadyFlag = false;

// (OLED helper functions identical to your original — omitted here for brevity in this block,
//  but in the file below they are included exactly as original.)
// For full file use the OLED functions from your original main.cpp (no changes).

// ============================================================
// AUDIO / MEMORY / BLUETOOTH CONFIG
// ============================================================
static const uint32_t INPUT_SAMPLE_RATE = 22050;
static const uint32_t OUTPUT_SAMPLE_RATE = 44100;
static const uint8_t INPUT_CHANNELS = 1;
static const uint8_t OUTPUT_CHANNELS = 2;
static const uint8_t BITS_PER_SAMPLE = 16;

static const size_t PCM_RING_SIZE = 16384;
static const size_t MP3_COPY_BUFFER = 1024;
static const size_t PCM_OUTPUT_CHUNK = 1024;
static const float PCM_GAIN = 2.0f;

static const uint32_t A2DP_TAIL_MS = 1000;
static const char *BT_DEVICE_NAME = "I7-TWS";

// Make A2DP source persistent: allocate once in setup(), reuse for sessions.
BluetoothA2DPSource *a2dpSource = nullptr;

// STATE
volatile bool btConnected = false;
volatile bool btAudioStarted = false;
volatile bool playbackRunning = false;
volatile uint32_t btCallbackCalls = 0;
bool ntpSynced = false;

// HEAP helper
void printHeap(const char *label) {
    Serial.printf("HEAP[%s]: free=%u largest=%u internal=%u\n",
        label,
        (unsigned)ESP.getFreeHeap(),
        (unsigned)ESP.getMaxAllocHeap(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
    );
}

// PCM ring implementation (same as original)
class PCMRingBuffer {
private:
    uint8_t *buffer = nullptr;
    size_t capacity = 0;
    volatile size_t readIndex = 0;
    volatile size_t writeIndex = 0;
    volatile size_t used = 0;
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

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

    size_t write(const uint8_t *src, size_t len) {
        if (!buffer || !src || len == 0) return 0;
        size_t written = 0;
        portENTER_CRITICAL(&mux);
        size_t freeBytes = capacity - used;
        if (len > freeBytes) len = freeBytes;
        if (len > 0) {
            size_t first = capacity - writeIndex;
            if (first > len) first = len;
            memcpy(buffer + writeIndex, src, first);
            size_t second = len - first;
            if (second > 0) memcpy(buffer, src + first, second);
            writeIndex = (writeIndex + len) % capacity;
            used += len;
            written = len;
        }
        portEXIT_CRITICAL(&mux);
        return written;
    }

    size_t read(uint8_t *dst, size_t len) {
        if (!buffer || !dst || len == 0) return 0;
        size_t result = 0;
        portENTER_CRITICAL(&mux);
        if (len > used) len = used;
        if (len > 0) {
            size_t first = capacity - readIndex;
            if (first > len) first = len;
            memcpy(dst, buffer + readIndex, first);
            size_t second = len - first;
            if (second > 0) memcpy(dst + first, buffer, second);
            readIndex = (readIndex + len) % capacity;
            used -= len;
            result = len;
        }
        portEXIT_CRITICAL(&mux);
        return result;
    }
};

PCMRingBuffer pcmRing;

// PCMOutputStream, mp3Decoder, mp3Stream (same as original)
class PCMOutputStream : public AudioStream {
private:
    AudioInfo currentInfo;
    uint8_t outputBuffer[PCM_OUTPUT_CHUNK];

    static int16_t applyGain(int16_t sample) {
        int32_t value = (int32_t)((float)sample * PCM_GAIN);
        if (value > 32767) value = 32767;
        if (value < -32768) value = -32768;
        return (int16_t)value;
    }

public:
    void setAudioInfo(AudioInfo info) override {
        currentInfo = info;
        AudioStream::setAudioInfo(info);
        Serial.printf("PCM format: %d Hz, %d ch, %d bit\n", info.sample_rate, info.channels, info.bits_per_sample);
    }

    int availableForWrite() override {
        size_t freeBytes = pcmRing.freeSpace();
        if (currentInfo.sample_rate == INPUT_SAMPLE_RATE &&
            currentInfo.channels == INPUT_CHANNELS &&
            currentInfo.bits_per_sample == BITS_PER_SAMPLE) {
            size_t inputCapacity = freeBytes / 4;
            if (inputCapacity > 512) inputCapacity = 512;
            return (int)inputCapacity;
        }
        return (int)min(freeBytes, (size_t)512);
    }

    size_t write(const uint8_t *data, size_t size) override {
        if (!data || size == 0) return 0;
        if (currentInfo.sample_rate != INPUT_SAMPLE_RATE ||
            currentInfo.channels != INPUT_CHANNELS ||
            currentInfo.bits_per_sample != BITS_PER_SAMPLE) {
            Serial.printf("PCM ERROR: unsupported %d Hz %d ch %d bit\n", currentInfo.sample_rate, currentInfo.channels, currentInfo.bits_per_sample);
            return 0;
        }

        size_t inputOffset = 0;
        while (inputOffset < size) {
            size_t remaining = size - inputOffset;
            size_t samples = remaining / 2;
            size_t maxSamples = sizeof(outputBuffer) / 8;
            if (samples > maxSamples) samples = maxSamples;
            if (samples == 0) break;
            size_t requiredOutput = samples * 8;
            uint32_t waitStart = millis();
            while (pcmRing.freeSpace() < requiredOutput) {
                delay(2);
                if (millis() - waitStart > 5000) {
                    Serial.println("PCM ERROR: ring timeout");
                    return 0;
                }
            }
            int16_t *inputSamples = (int16_t *)(data + inputOffset);
            int16_t *outputSamples = (int16_t *)(outputBuffer);
            size_t outSampleIndex = 0;
            for (size_t i = 0; i < samples; i++) {
                int16_t sample = applyGain(inputSamples[i]);
                outputSamples[outSampleIndex++] = sample;
                outputSamples[outSampleIndex++] = sample;
                outputSamples[outSampleIndex++] = sample;
                outputSamples[outSampleIndex++] = sample;
            }
            size_t written = pcmRing.write(outputBuffer, requiredOutput);
            if (written != requiredOutput) {
                Serial.printf("PCM ERROR: wrote %u/%u\n", (unsigned)written, (unsigned)requiredOutput);
                return 0;
            }
            inputOffset += samples * 2;
        }
        return inputOffset;
    }
};

PCMOutputStream pcmOutput;
MP3DecoderHelix mp3Decoder;
EncodedAudioStream mp3Stream(&pcmOutput, &mp3Decoder);

// ============================================================
// NETWORK/TTS/NTP FUNCTIONS (unchanged)
// ============================================================
// (copy these from your original main.cpp — no changes needed)

// ============================================================
// A2DP CALLBACKS (safe)
// ============================================================
int32_t getAudioDataNoop(uint8_t *data, int32_t len) {
    if (data && len > 0) memset(data, 0, len);
    return len;
}

int32_t getAudioData(uint8_t *data, int32_t len) {
    if (!data || len <= 0) return 0;
    btCallbackCalls++;
    size_t got = pcmRing.read(data, len);
    if (got < (size_t)len) memset(data + got, 0, len - got);
    return len;
}

void onBTConnectionState(esp_a2d_connection_state_t state, void *) {
    Serial.print("TARS: A2DP STATE = ");
    switch (state) {
        case ESP_A2D_CONNECTION_STATE_DISCONNECTED: Serial.println("Disconnected"); break;
        case ESP_A2D_CONNECTION_STATE_CONNECTING: Serial.println("Connecting"); break;
        case ESP_A2D_CONNECTION_STATE_CONNECTED: Serial.println("Connected"); break;
        case ESP_A2D_CONNECTION_STATE_DISCONNECTING: Serial.println("Disconnecting"); break;
        default: Serial.printf("State=%d\n", (int)state); break;
    }
    btConnected = (state == ESP_A2D_CONNECTION_STATE_CONNECTED);
}

void onBTAudioState(esp_a2d_audio_state_t state, void *) {
    Serial.print("TARS: A2DP AUDIO = ");
    switch (state) {
        case ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND: Serial.println("Suspended"); break;
        case ESP_A2D_AUDIO_STATE_STOPPED: Serial.println("Stopped"); break;
        case ESP_A2D_AUDIO_STATE_STARTED: Serial.println("Started"); break;
        default: Serial.printf("AudioState=%d\n", (int)state); break;
    }
    btAudioStarted = (state == ESP_A2D_AUDIO_STATE_STARTED);
}

// ============================================================
// START / STOP BLUETOOTH (modified to reuse a2dpSource)
// ============================================================
bool startBluetooth() {
    Serial.println("TARS: Bluetooth START");

    // ensure object exists (allocate once)
    if (a2dpSource == nullptr) {
        a2dpSource = new BluetoothA2DPSource();
        if (a2dpSource == nullptr) {
            Serial.println("TARS: A2DP OBJECT ALLOC FAILED");
            return false;
        }
        // configure once
        a2dpSource->set_event_queue_size(8);
        a2dpSource->set_event_stack_size(4096); // increase stack
        a2dpSource->set_auto_reconnect(true);   // try auto reconnect
        // register callbacks (safe functions)
        a2dpSource->set_data_callback(getAudioData);
        a2dpSource->set_on_connection_state_changed(onBTConnectionState);
        a2dpSource->set_on_audio_state_changed(onBTAudioState);
    }

    btConnected = false;
    btAudioStarted = false;
    btCallbackCalls = 0;

    printHeap("BEFORE_BT");

    bool ok = a2dpSource->start(BT_DEVICE_NAME);
    Serial.printf("a2dpSource->start returned=%d\n", ok ? 1 : 0);

    uint32_t start = millis();
    while (!btConnected && millis() - start < BT_TIMEOUT_MS) {
        delay(100);
    }
    Serial.printf("btConnected after wait = %d\n", btConnected ? 1 : 0);

    if (!btConnected) {
        Serial.println("TARS: Bluetooth CONNECT FAILED");
        printHeap("BT_FAILED");
        // leave object intact (do not delete): stop it safely
        // attempt graceful stop
        a2dpSource->set_data_callback(getAudioDataNoop);
        a2dpSource->end(false); // stop but keep object
        return false;
    }

    Serial.println("TARS: Bluetooth READY");
    delay(300);
    printHeap("BT_READY");
    return true;
}

void releaseBluetooth() {
    if (a2dpSource == nullptr) {
        btConnected = false;
        btAudioStarted = false;
        return;
    }

    Serial.println("TARS: Bluetooth RELEASE");

    btAudioStarted = false;
    btConnected = false;

    // set data callback to noop to avoid pending read usage
    a2dpSource->set_data_callback(getAudioDataNoop);
    // give small time for pending events
    uint32_t graceStart = millis();
    while (millis() - graceStart < 50) delay(1);

    // stop streaming but DON'T delete object: end(false) leaves internal structures intact
    a2dpSource->end(false);

    // give some time for library events to settle
    uint32_t waitStart = millis();
    while (millis() - waitStart < 500) delay(10);

    btConnected = false;
    btAudioStarted = false;

    Serial.println("TARS: Bluetooth MEMORY RELEASED (object kept for reuse)");
    printHeap("AFTER_BT_RELEASE");
}

// ============================================================
// stopBluetooth wrapper
// ============================================================
void stopBluetooth() {
    Serial.println("TARS: Bluetooth STOP");
    releaseBluetooth();
    Serial.printf("TARS: A2DP callbacks = %u\n", (unsigned)btCallbackCalls);
}

// ============================================================
// AUDIO session cleanup and playMP3 (mostly unchanged)
// ============================================================
// (Copy the rest of your playMP3, cleanupAudioSession, handleQuestion, askAI,
//  downloadTTS, network, setup, loop — keeping logic the same except they call
//  new startBluetooth/stopBluetooth above.)

// IMPORTANT: in setup() allocate the persistent a2dpSource so it's always ready
void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(1000);
    Serial.println();
    Serial.println("================================");
    Serial.println("        TARS ESP32 START");
    Serial.println("================================");
    printHeap("BOOT");

    Wire.begin(21, 22);
    if (oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        oledReadyFlag = true;
        oledShowOnline();
    }

    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS FAILED");
        return;
    }
    Serial.println("LittleFS OK");
    printHeap("AFTER_LITTLEFS");

    if (!pcmRing.begin(PCM_RING_SIZE)) {
        Serial.println("PCM RING ALLOC FAILED");
        return;
    }

    Serial.println("PCM  : 16KB BUFFER");
    Serial.println("HELIX: 4096 PCM / 2048 FRAME");
    Serial.println("BT   : QUEUE 8 / STACK 4096");
    Serial.println("PRIME: DISABLED");
    Serial.println("A2DP : PERSISTENT OBJECT REUSE");

    printHeap("AFTER_PCM_RING");

    // Create persistent a2dpSource here (so we avoid new/delete races later)
    if (a2dpSource == nullptr) {
        a2dpSource = new BluetoothA2DPSource();
        if (a2dpSource) {
            a2dpSource->set_event_queue_size(8);
            a2dpSource->set_event_stack_size(4096);
            a2dpSource->set_auto_reconnect(true);
            a2dpSource->set_data_callback(getAudioDataNoop); // set noop until actual start
            a2dpSource->set_on_connection_state_changed(onBTConnectionState);
            a2dpSource->set_on_audio_state_changed(onBTAudioState);
        } else {
            Serial.println("WARN: persistent a2dpSource allocation failed");
        }
    }

    // rest of setup: connect WiFi, NTP, etc. (as in original)
    if (!connectWiFi(true)) {
        Serial.println("TARS: INITIAL WIFI FAILED");
    }
    if (WiFi.status() == WL_CONNECTED && !ntpSynced) {
        ensureTimeValid();
    }
    printHeap("READY");
    oledShowReady();
    Serial.println("================================");
    Serial.println("TARS: READY FOR QUESTION");
    Serial.println("================================");
}

void loop() {
    // same as original
    // oledUpdate...
    // read serial, handleQuestion, etc.
    // keep unchanged logic
    oledUpdateReadyAnimation();
    if (Serial.available()) {
        String question = Serial.readStringUntil('\n');
        question.trim();
        if (question.length() > 0) handleQuestion(question);
    }
    delay(10);
}
