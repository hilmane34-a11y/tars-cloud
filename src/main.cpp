// ============================================================
// TARS CLOUD - FINAL BUILD READY
// INMP441 STT + OLED + WiFi Manager + NTP
// ElevenLabs STT via /stt
// TTS / SING MP3 + Helix + ESP32 DAC
// ESP32-WROOM-32 / NO PSRAM
//
// WiFi: ALWAYS ON
// Serial: MONITOR ONLY
// Bluetooth: DISABLED
// Boot animation: NONE
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Wire.h>
#include <time.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"

#include "config.h"
#include "wifi_manager.h"

// ============================================================
// PIN
// ============================================================

#define OLED_SDA 21
#define OLED_SCL 22

#define DAC_PORT I2S_NUM_0
#define DAC_L    25
#define DAC_R    26

#define MIC_PORT I2S_NUM_1
#define MIC_SCK  18
#define MIC_WS   19
#define MIC_SD   34

// ============================================================
// AUDIO
// ============================================================

static const uint32_t MIC_RATE = 16000;
static const uint32_t PLAY_RATE = 22050;

static const uint32_t RECORD_MS = 4000;

static const size_t MIC_BUF_SIZE = 2048;
static const size_t MP3_BUF_SIZE = 1024;
static const size_t DAC_BUF_SIZE = 1024;

static const float DAC_GAIN = 3.5f;

// ============================================================
// TIME
// ============================================================

static const char *NTP1 = "pool.ntp.org";
static const char *NTP2 = "time.nist.gov";

static const long GMT_OFFSET = 7 * 3600;
static const int DST_OFFSET = 0;

static const uint32_t NTP_RETRY_MS = 30000;

// ============================================================
// FILE
// ============================================================

static const char *STT_FILE = "/stt.wav";

// ============================================================
// URL
// ============================================================

static const char *ASK_URL  = TARS_CLOUD_URL "/ask";
static const char *STT_URL  = TARS_CLOUD_URL "/stt";
static const char *TTS_URL  = TARS_CLOUD_URL "/tts";
static const char *SING_URL = TARS_CLOUD_URL "/sing";

// ============================================================
// OLED
// ============================================================

Adafruit_SSD1306 oled(
  OLED_WIDTH,
  OLED_HEIGHT,
  &Wire,
  -1
);

static bool oledOK = false;

static String oledAnswer;
static size_t oledChars = 0;

static uint32_t oledTypeAt = 0;
static uint32_t oledAnimAt = 0;

static bool oledTyping = false;
static bool oledAudioSync = false;

static uint8_t oledFrame = 0;

static const uint32_t OLED_TYPE_MS = 44;
static const uint32_t OLED_ANIM_MS = 80;
static const uint32_t OLED_SYNC_MS = 50;

// ============================================================
// SYSTEM STATE
// ============================================================

static bool dacOK = false;
static bool micOK = false;
static bool ntpOK = false;

static volatile bool playing = false;
static volatile bool firstAudio = false;
static volatile uint32_t firstAudioAt = 0;
static volatile uint8_t singLevel = 0;

static bool singMode = false;

// ============================================================
// VOICE PIPELINE STATE
// ============================================================

enum VoiceState {
  VOICE_IDLE,
  VOICE_RECORD,
  VOICE_STT,
  VOICE_ASK,
  VOICE_AUDIO,
  VOICE_PLAY,
  VOICE_FINISH
};

static VoiceState voiceState = VOICE_IDLE;

static uint32_t stateAt = 0;
static uint32_t recordStart = 0;

static String voiceText;
static String aiAnswer;
static bool voiceSing = false;

// ============================================================
// RECORD STATE
// ============================================================

static File recordFile;

static uint32_t recordSamples = 0;

static uint8_t wavHeader[44];

static int32_t micRaw[MIC_BUF_SIZE / 4];
static int16_t micPCM[MIC_BUF_SIZE / 4];

// ============================================================
// HEAP
// ============================================================

static void heapInfo(const char *name) {
  Serial.printf(
    "HEAP[%s] free=%u largest=%u\n",
    name,
    (unsigned)ESP.getFreeHeap(),
    (unsigned)ESP.getMaxAllocHeap()
  );
}

// ============================================================
// OLED
// ============================================================

static void oledHeader(const char *title) {

  if (!oledOK)
    return;

  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);

  oled.setCursor(45, 0);
  oled.print("T A R S");

  oled.drawLine(0, 9, 27, 9, SSD1306_WHITE);
  oled.drawLine(34, 9, 61, 9, SSD1306_WHITE);
  oled.drawLine(67, 9, 94, 9, SSD1306_WHITE);
  oled.drawLine(101, 9, 127, 9, SSD1306_WHITE);

  oled.setCursor(3, 13);
  oled.print("> ");
  oled.print(title);
}

static void oledMechanical(bool speaking) {

  if (!oledOK)
    return;

  const int base = 62;

  oled.drawLine(
    2,
    base,
    125,
    base,
    SSD1306_WHITE
  );

  if (!speaking) {

    static const uint8_t p[24] = {
      2,2,5,5,5,2,
      2,4,4,2,2,5,
      5,5,2,2,4,4,
      2,2,5,5,2,2
    };

    for (int i = 0; i < 24; i++) {

      int x = 3 + i * 5;

      if (x > 123)
        break;

      uint8_t n =
        (i + oledFrame) % 24;

      int h = p[n];

      oled.drawLine(
        x,
        base - h,
        x + 3,
        base - h,
        SSD1306_WHITE
      );

      if (i < 23) {

        int nx = x + 5;

        oled.drawLine(
          x + 3,
          base - h,
          nx,
          base - p[(n + 1) % 24],
          SSD1306_WHITE
        );
      }
    }

    return;
  }

  for (int i = 0; i < 6; i++) {

    int x = 8 + i * 22;
    int h = 3;

    if (i == oledFrame % 6)
      h = 8;

    else if (
      i == (oledFrame + 5) % 6 ||
      i == (oledFrame + 1) % 6
    )
      h = 5;

    oled.drawLine(
      x,
      base - h,
      x,
      base,
      SSD1306_WHITE
    );

    oled.drawLine(
      x - 3,
      base - h,
      x,
      base,
      SSD1306_WHITE
    );

    oled.drawLine(
      x,
      base,
      x + 3,
      base - h,
      SSD1306_WHITE
    );
  }
}

static void oledReady() {

  if (!oledOK)
    return;

  oled.clearDisplay();

  oledHeader("READY");

  oled.setCursor(3, 27);
  oled.print("WAITING FOR");

  oled.setCursor(3, 36);
  oled.print("COMMAND...");

  oledMechanical(false);

  oled.display();
}

static void oledListening() {

  if (!oledOK)
    return;

  oled.clearDisplay();

  oledHeader("LISTENING");

  oled.setCursor(3, 27);
  oled.print("LISTENING...");

  oled.setCursor(3, 36);
  oled.print("SPEAK NOW");

  oledMechanical(false);

  oled.display();
}

static void oledProcessing() {

  if (!oledOK)
    return;

  oled.clearDisplay();

  oledHeader("PROCESSING");

  oled.setCursor(3, 27);
  oled.print("ANALYZING...");

  oled.setCursor(3, 36);
  oled.print("GENERATING");

  oledMechanical(false);

  oled.display();
}

static void oledTypingPrepare(
  const String &text
) {

  if (!oledOK)
    return;

  oledAnswer = text;
  oledChars = 0;

  oledTyping = false;
  oledAudioSync = true;

  oledTypeAt = millis();
  oledFrame = 0;
}

static void oledTypingStart() {

  oledTyping = true;
  oledAudioSync = false;

  oledChars = 0;
  oledTypeAt = millis();
  oledFrame = 0;
}

static void oledTypedDraw(
  bool speaking
) {

  if (!oledOK)
    return;

  oled.clearDisplay();

  oledHeader(
    speaking ? "SPEAKING" : "READY"
  );

  oled.setCursor(3, 23);
  oled.print("> ");

  int line = 0;
  int col = 2;

  for (
    size_t i = 0;
    i < oledChars;
    i++
  ) {

    char c = oledAnswer[i];

    if (c == '\r')
      continue;

    if (c == '\n') {

      line++;
      col = 0;

      if (line >= 4)
        break;

      oled.setCursor(
        3,
        23 + line * 9
      );

      continue;
    }

    if (col >= 20) {

      line++;
      col = 0;

      if (line >= 4)
        break;

      oled.setCursor(
        3,
        23 + line * 9
      );
    }

    oled.write(c);
    col++;
  }

  oledMechanical(speaking);

  oled.display();
}

static void oledSingDraw() {

  if (!oledOK)
    return;

  oled.clearDisplay();

  for (int i = 0; i < 6; i++) {

    int variation =
      (
        i * 13 +
        (millis() / 90) % 17
      ) % 18 - 9;

    int h =
      4 +
      (
        singLevel *
        (70 + variation)
        / 255
      );

    if (h < 4)
      h = 4;

    if (h > 55)
      h = 55;

    oled.fillRect(
      16 + i * 19,
      63 - h,
      8,
      h,
      SSD1306_WHITE
    );
  }

  oled.display();
}

static void oledUpdate() {

  if (!oledOK)
    return;

  uint32_t now = millis();

  if (singMode && playing) {

    if (
      now - oledAnimAt >=
      OLED_ANIM_MS
    ) {

      oledAnimAt = now;
      oledSingDraw();
    }

    return;
  }

  if (
    oledAudioSync &&
    firstAudio &&
    now - firstAudioAt >=
    OLED_SYNC_MS
  ) {

    oledTypingStart();
    oledTypedDraw(true);
  }

  bool redraw = false;

  if (
    oledTyping &&
    now - oledTypeAt >=
    OLED_TYPE_MS
  ) {

    oledTypeAt = now;

    if (
      oledChars <
      oledAnswer.length()
    )
      oledChars++;
    else
      oledTyping = false;

    redraw = true;
  }

  if (
    playing &&
    now - oledAnimAt >=
    OLED_ANIM_MS
  ) {

    oledAnimAt = now;
    oledFrame++;

    redraw = true;
  }

  if (redraw)
    oledTypedDraw(playing);
}

static void oledReadyAnimation() {

  if (!oledOK)
    return;

  uint32_t now = millis();

  if (
    now - oledAnimAt <
    OLED_ANIM_MS
  )
    return;

  oledAnimAt = now;
  oledFrame++;

  oledReady();
}

// ============================================================
// DAC
// ============================================================

static bool initDAC() {

  i2s_config_t cfg;

  memset(
    &cfg,
    0,
    sizeof(cfg)
  );

  cfg.mode =
    (i2s_mode_t)(
      I2S_MODE_MASTER |
      I2S_MODE_TX |
      I2S_MODE_DAC_BUILT_IN
    );

  cfg.sample_rate =
    PLAY_RATE;

  cfg.bits_per_sample =
    I2S_BITS_PER_SAMPLE_16BIT;

  cfg.channel_format =
    I2S_CHANNEL_FMT_RIGHT_LEFT;

  cfg.communication_format =
    I2S_COMM_FORMAT_I2S_MSB;

  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = 0;

  esp_err_t err =
    i2s_driver_install(
      DAC_PORT,
      &cfg,
      0,
      nullptr
    );

  if (err != ESP_OK) {

    Serial.printf(
      "TARS: DAC DRIVER ERROR %d\n",
      (int)err
    );

    return false;
  }

  err =
    i2s_set_dac_mode(
      I2S_DAC_CHANNEL_BOTH_EN
    );

  if (err != ESP_OK) {

    i2s_driver_uninstall(
      DAC_PORT
    );

    return false;
  }

  i2s_zero_dma_buffer(
    DAC_PORT
  );

  Serial.println(
    "TARS: DAC GPIO25/26 READY"
  );

  return true;
}

class DACOutput :
  public AudioStream {

  AudioInfo info;

  uint16_t buffer[
    DAC_BUF_SIZE / 2
  ];

  static uint8_t convert(
    int16_t sample
  ) {

    int32_t v =
      (int32_t)(
        (float)sample *
        DAC_GAIN
      );

    if (v > 32767)
      v = 32767;

    if (v < -32768)
      v = -32768;

    int32_t u =
      (v >> 8) + 128;

    if (u < 0)
      u = 0;

    if (u > 255)
      u = 255;

    return (uint8_t)u;
  }

public:

  void setAudioInfo(
    AudioInfo i
  ) override {

    info = i;

    AudioStream::setAudioInfo(i);

    if (dacOK) {

      i2s_set_clk(
        DAC_PORT,
        i.sample_rate,
        I2S_BITS_PER_SAMPLE_16BIT,
        I2S_CHANNEL_STEREO
      );
    }
  }

  int availableForWrite()
    override {

    return DAC_BUF_SIZE;
  }

  size_t write(
    const uint8_t *data,
    size_t size
  ) override {

    if (
      !data ||
      !size ||
      !dacOK
    )
      return 0;

    if (
      info.bits_per_sample != 16 ||
      (
        info.channels != 1 &&
        info.channels != 2
      )
    )
      return 0;

    size_t bytesPerFrame =
      info.channels * 2;

    size_t frames =
      size / bytesPerFrame;

    size_t maxFrames =
      (DAC_BUF_SIZE / 2) / 2;

    if (frames > maxFrames)
      frames = maxFrames;

    if (!frames)
      return 0;

    int peak = 0;

    for (
      size_t i = 0;
      i < frames;
      i++
    ) {

      int16_t sample;

      if (info.channels == 1) {

        sample =
          (int16_t)(
            data[i * 2] |
            (
              (uint16_t)
              data[i * 2 + 1]
              << 8
            )
          );

      } else {

        int16_t l =
          (int16_t)(
            data[i * 4] |
            (
              (uint16_t)
              data[i * 4 + 1]
              << 8
            )
          );

        int16_t r =
          (int16_t)(
            data[i * 4 + 2] |
            (
              (uint16_t)
              data[i * 4 + 3]
              << 8
            )
          );

        sample =
          (int16_t)(
            (
              (int32_t)l +
              (int32_t)r
            ) / 2
          );
      }

      int a =
        abs((int)sample);

      if (a > peak)
        peak = a;

      uint8_t d =
        convert(sample);

      buffer[i * 2] =
        (uint16_t)d << 8;

      buffer[i * 2 + 1] =
        (uint16_t)d << 8;
    }

    singLevel =
      (uint8_t)min(
        255,
        peak >> 7
      );

    size_t bytes =
      frames * 4;

    size_t written = 0;

    esp_err_t err =
      i2s_write(
        DAC_PORT,
        buffer,
        bytes,
        &written,
        portMAX_DELAY
      );

    if (err != ESP_OK)
      return 0;

    if (
      written &&
      !firstAudio
    ) {

      firstAudio = true;
      firstAudioAt = millis();

      Serial.println(
        "TARS: FIRST AUDIO"
      );
    }

    return frames *
      bytesPerFrame;
  }
};

DACOutput dacOutput;

MP3DecoderHelix mp3Decoder;

EncodedAudioStream mp3Stream(
  &dacOutput,
  &mp3Decoder
);

// ============================================================
// INMP441
// ============================================================

static bool initMic() {

  i2s_config_t cfg;

  memset(
    &cfg,
    0,
    sizeof(cfg)
  );

  cfg.mode =
    (i2s_mode_t)(
      I2S_MODE_MASTER |
      I2S_MODE_RX
    );

  cfg.sample_rate =
    MIC_RATE;

  cfg.bits_per_sample =
    I2S_BITS_PER_SAMPLE_32BIT;

  cfg.channel_format =
    I2S_CHANNEL_FMT_ONLY_LEFT;

  cfg.communication_format =
    I2S_COMM_FORMAT_I2S;

  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 2;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = false;
  cfg.fixed_mclk = 0;

  esp_err_t err =
    i2s_driver_install(
      MIC_PORT,
      &cfg,
      0,
      nullptr
    );

  if (err != ESP_OK) {

    Serial.printf(
      "TARS: MIC DRIVER ERROR %d\n",
      (int)err
    );

    return false;
  }

  i2s_pin_config_t pins;

  pins.bck_io_num = MIC_SCK;
  pins.ws_io_num = MIC_WS;
  pins.data_out_num =
    I2S_PIN_NO_CHANGE;
  pins.data_in_num = MIC_SD;

  err =
    i2s_set_pin(
      MIC_PORT,
      &pins
    );

  if (err != ESP_OK) {

    i2s_driver_uninstall(
      MIC_PORT
    );

    return false;
  }

  i2s_zero_dma_buffer(
    MIC_PORT
  );

  Serial.println(
    "TARS: INMP441 READY"
  );

  return true;
}

// ============================================================
// WAV
// ============================================================

static void put16(
  uint8_t *p,
  uint16_t v
) {

  p[0] = v & 0xff;
  p[1] = v >> 8;
}

static void put32(
  uint8_t *p,
  uint32_t v
) {

  p[0] = v & 0xff;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
}

static void writeWavHeader(
  File &file,
  uint32_t dataSize
) {

  uint8_t h[44];

  memset(
    h,
    0,
    sizeof(h)
  );

  memcpy(h, "RIFF", 4);

  put32(
    h + 4,
    36 + dataSize
  );

  memcpy(
    h + 8,
    "WAVE",
    4
  );

  memcpy(
    h + 12,
    "fmt ",
    4
  );

  put32(
    h + 16,
    16
  );

  put16(
    h + 20,
    1
  );

  put16(
    h + 22,
    1
  );

  put32(
    h + 24,
    MIC_RATE
  );

  put32(
    h + 28,
    MIC_RATE * 2
  );

  put16(
    h + 32,
    2
  );

  put16(
    h + 34,
    16
  );

  memcpy(
    h + 36,
    "data",
    4
  );

  put32(
    h + 40,
    dataSize
  );

  file.seek(0);

  file.write(
    h,
    sizeof(h)
  );
}

// ============================================================
// RECORD START
// ============================================================

static bool startRecording() {

  if (!micOK)
    return false;

  if (
    LittleFS.exists(
      STT_FILE
    )
  )
    LittleFS.remove(
      STT_FILE
    );

  recordFile =
    LittleFS.open(
      STT_FILE,
      FILE_WRITE
    );

  if (!recordFile) {

    Serial.println(
      "TARS: STT FILE FAILED"
    );

    return false;
  }

  memset(
    wavHeader,
    0,
    sizeof(wavHeader)
  );

  recordFile.write(
    wavHeader,
    sizeof(wavHeader)
  );

  recordSamples = 0;

  recordStart = millis();

  oledListening();

  Serial.println(
    "TARS: RECORDING..."
  );

  return true;
}

// ============================================================
// RECORD STEP
// ============================================================

static bool recordStep() {

  if (!recordFile)
    return false;

  size_t bytesRead = 0;

  esp_err_t err =
    i2s_read(
      MIC_PORT,
      micRaw,
      sizeof(micRaw),
      &bytesRead,
      0
    );

  if (
    err != ESP_OK ||
    bytesRead == 0
  )
    return true;

  size_t samples =
    bytesRead /
    sizeof(int32_t);

  for (
    size_t i = 0;
    i < samples;
    i++
  ) {

    int32_t v =
      micRaw[i] >> 14;

    if (v > 32767)
      v = 32767;

    if (v < -32768)
      v = -32768;

    micPCM[i] =
      (int16_t)v;
  }

  recordFile.write(
    (uint8_t *)micPCM,
    samples * 2
  );

  recordSamples += samples;

  if (
    millis() - recordStart >=
    RECORD_MS
  ) {

    uint32_t dataSize =
      recordSamples * 2;

    writeWavHeader(
      recordFile,
      dataSize
    );

    recordFile.flush();
    recordFile.close();

    Serial.printf(
      "TARS: RECORD DONE %u bytes\n",
      (unsigned)dataSize
    );

    return false;
  }

  return true;
}

// ============================================================
// NTP
// ============================================================

static bool timeValid() {

  return time(nullptr) >
    1577836800;
}

static void startNTP() {

  configTime(
    GMT_OFFSET,
    DST_OFFSET,
    NTP1,
    NTP2
  );

  ntpOK = false;

  Serial.println(
    "TARS: NTP START"
  );
}

static void updateNTP() {

  if (
    ntpOK &&
    timeValid()
  )
    return;

  static uint32_t lastTry = 0;

  uint32_t now = millis();

  if (
    lastTry != 0 &&
    now - lastTry <
    NTP_RETRY_MS
  )
    return;

  lastTry = now;

  if (!timeValid()) {
    Serial.println(
      "TARS: NTP WAITING"
    );
    return;
  }

  ntpOK = true;

  time_t tnow =
    time(nullptr);

  struct tm t;

  localtime_r(
    &tnow,
    &t
  );

  Serial.printf(
    "TARS: NTP OK %04d-%02d-%02d %02d:%02d:%02d\n",
    t.tm_year + 1900,
    t.tm_mon + 1,
    t.tm_mday,
    t.tm_hour,
    t.tm_min,
    t.tm_sec
  );
}

// ============================================================
// WIFI ALWAYS ON
// ============================================================

static bool ensureWiFi() {

  if (
    WiFi.status() ==
    WL_CONNECTED
  )
    return true;

  Serial.println(
    "TARS: WIFI RECONNECT"
  );

  return wifiManagerConnect(
    false
  );
}

static void maintainWiFi() {

  static uint32_t lastCheck = 0;

  uint32_t now = millis();

  if (
    now - lastCheck <
    3000
  )
    return;

  lastCheck = now;

  if (
    WiFi.status() !=
    WL_CONNECTED
  ) {

    Serial.println(
      "TARS: WIFI LOST"
    );

    wifiManagerConnect(
      false
    );

    return;
  }

  updateNTP();
}

// ============================================================
// ASK
// ============================================================

static String askAI(
  const String &question
) {

  if (!ensureWiFi())
    return "";

  oledProcessing();

  WiFiClientSecure client;

  client.setInsecure();

  HTTPClient http;

  if (
    !http.begin(
      client,
      ASK_URL
    )
  )
    return "";

  http.addHeader(
    "Content-Type",
    "application/json"
  );

  StaticJsonDocument<384> req;

  req["text"] =
    question;

  String body;

  serializeJson(
    req,
    body
  );

  int code =
    http.POST(body);

  Serial.printf(
    "ASK HTTP: %d\n",
    code
  );

  if (
    code < 200 ||
    code >= 300
  ) {

    http.end();

    return "";
  }

  String response =
    http.getString();

  http.end();

  StaticJsonDocument<768> json;

  if (
    deserializeJson(
      json,
      response
    )
  ) {

    Serial.println(
      "TARS: ASK JSON ERROR"
    );

    return "";
  }

  String answer =
    json["response"] | "";

  answer.trim();

  Serial.println(
    "TARS RESPONSE:"
  );

  Serial.println(
    answer
  );

  return answer;
}

// ============================================================
// STT
// ============================================================

static String speechToText() {

  if (!ensureWiFi())
    return "";

  if (
    !LittleFS.exists(
      STT_FILE
    )
  )
    return "";

  File file =
    LittleFS.open(
      STT_FILE,
      FILE_READ
    );

  if (!file)
    return "";

  const char *boundary =
    "----TARSSTTBoundary";

  String prefix;

  prefix =
    "--" +
    String(boundary) +
    "\r\n"
    "Content-Disposition: form-data; "
    "name=\"file\"; filename=\"stt.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n";

  String suffix =
    "\r\n--" +
    String(boundary) +
    "--\r\n";

  size_t fileSize =
    file.size();

  size_t contentLength =
    prefix.length() +
    fileSize +
    suffix.length();

  WiFiClientSecure client;

  client.setInsecure();

  String base =
    String(TARS_CLOUD_URL);

  int scheme =
    base.indexOf("://");

  if (scheme >= 0)
    base =
      base.substring(
        scheme + 3
      );

  int slash =
    base.indexOf('/');

  String host = base;

  String basePath = "";

  if (slash >= 0) {

    host =
      base.substring(
        0,
        slash
      );

    basePath =
      base.substring(
        slash
      );
  }

  String path =
    basePath + "/stt";

  if (
    !client.connect(
      host.c_str(),
      443
    )
  ) {

    file.close();

    Serial.println(
      "TARS: STT CONNECT FAILED"
    );

    return "";
  }

  client.printf(
    "POST %s HTTP/1.1\r\n",
    path.c_str()
  );

  client.printf(
    "Host: %s\r\n",
    host.c_str()
  );

  client.printf(
    "Content-Type: multipart/form-data; boundary=%s\r\n",
    boundary
  );

  client.printf(
    "Content-Length: %u\r\n",
    (unsigned)contentLength
  );

  client.println(
    "Connection: close"
  );

  client.println();

  client.print(prefix);

  static uint8_t buffer[
    MP3_BUF_SIZE
  ];

  while (
    file.available()
  ) {

    size_t n =
      file.read(
        buffer,
        sizeof(buffer)
      );

    if (!n)
      break;

    size_t sent = 0;

    while (
      sent < n
    ) {

      size_t w =
        client.write(
          buffer + sent,
          n - sent
        );

      if (!w) {

        file.close();
        client.stop();

        return "";
      }

      sent += w;

      yield();
    }
  }

  file.close();

  client.print(
    suffix
  );

  uint32_t start =
    millis();

  while (
    client.connected() &&
    !client.available() &&
    millis() - start < 15000
  ) {

    yield();
  }

  String response;

  while (
    client.available()
  ) {

    response +=
      client.readStringUntil(
        '\n'
      );

    response += "\n";
  }

  client.stop();

  int bodyPos =
    response.indexOf(
      "\r\n\r\n"
    );

  if (bodyPos >= 0) {

    response =
      response.substring(
        bodyPos + 4
      );
  }

  Serial.println(
    "STT RESPONSE:"
  );

  Serial.println(
    response
  );

  StaticJsonDocument<512> json;

  if (
    deserializeJson(
      json,
      response
    )
  ) {

    Serial.println(
      "TARS: STT JSON ERROR"
    );

    return "";
  }

  String text =
    json["text"] | "";

  if (!text.length())
    text =
      json["transcript"] | "";

  text.trim();

  return text;
}

// ============================================================
// MP3 DOWNLOAD
// ============================================================

static bool downloadMP3(
  const char *url,
  const String &text
) {

  if (!ensureWiFi())
    return false;

  WiFiClientSecure client;

  client.setInsecure();

  HTTPClient http;

  if (
    !http.begin(
      client,
      url
    )
  )
    return false;

  http.addHeader(
    "Content-Type",
    "application/json"
  );

  StaticJsonDocument<384> req;

  req["text"] =
    text;

  String body;

  serializeJson(
    req,
    body
  );

  int code =
    http.POST(body);

  Serial.printf(
    "AUDIO HTTP: %d\n",
    code
  );

  if (
    code < 200 ||
    code >= 300
  ) {

    http.end();

    return false;
  }

  int len =
    http.getSize();

  if (
    LittleFS.exists(
      MP3_FILE
    )
  )
    LittleFS.remove(
      MP3_FILE
    );

  File file =
    LittleFS.open(
      MP3_FILE,
      FILE_WRITE
    );

  if (!file) {

    http.end();

    return false;
  }

  WiFiClient *stream =
    http.getStreamPtr();

  static uint8_t buffer[
    MP3_BUF_SIZE
  ];

  size_t total = 0;

  uint32_t last =
    millis();

  while (
    http.connected() &&
    (len > 0 || len == -1)
  ) {

    size_t available =
      stream->available();

    if (available) {

      size_t n =
        min(
          available,
          sizeof(buffer)
        );

      int r =
        stream->readBytes(
          buffer,
          n
        );

      if (r > 0) {

        file.write(
          buffer,
          r
        );

        total += r;

        last = millis();

        if (len > 0)
          len -= r;
      }

    } else {

      if (
        millis() - last >
        5000
      )
        break;

      yield();
    }
  }

  file.flush();
  file.close();

  http.end();

  Serial.printf(
    "TARS: MP3 %u bytes\n",
    (unsigned)total
  );

  return total > 0;
}

// ============================================================
// PLAY MP3
// ============================================================

static bool playMP3() {

  if (
    !LittleFS.exists(
      MP3_FILE
    )
  )
    return false;

  File file =
    LittleFS.open(
      MP3_FILE,
      FILE_READ
    );

  if (!file)
    return false;

  size_t size =
    file.size();

  if (!size) {

    file.close();

    LittleFS.remove(
      MP3_FILE
    );

    return false;
  }

  playing = true;

  firstAudio = false;
  firstAudioAt = 0;
  singLevel = 0;

  mp3Decoder.setMaxPCMSize(
    2048
  );

  mp3Decoder.setMaxFrameSize(
    1024
  );

  if (
    !mp3Stream.begin()
  ) {

    file.close();

    playing = false;

    Serial.println(
      "TARS: HELIX FAILED"
    );

    return false;
  }

  StreamCopy copier(
    mp3Stream,
    file,
    MP3_BUF_SIZE
  );

  copier.setCheckAvailableForWrite(
    false
  );

  copier.setCheckAvailable(
    true
  );

  uint32_t start =
    millis();

  bool finished = false;

  while (
    millis() - start <
    120000
  ) {

    oledUpdate();

    if (
      file.position() >=
      size
    ) {

      finished = true;
      break;
    }

    size_t n =
      copier.copy();

    if (!n)
      yield();
    else
      yield();
  }

  mp3Stream.end();

  file.close();

  playing = false;

  singLevel = 0;

  LittleFS.remove(
    MP3_FILE
  );

  singMode = false;
  oledTyping = false;
  oledAudioSync = false;

  oledReady();

  return finished;
}

// ============================================================
// SING DETECTION
// ============================================================

static bool isSingRequest(
  String q
) {

  q.toLowerCase();

  return
    q.indexOf("nyanyi") >= 0 ||
    q.indexOf("bernyanyi") >= 0 ||
    q.indexOf("nyanyikan") >= 0 ||
    q.indexOf("nyanyiin") >= 0;
}

// ============================================================
// QUESTION PIPELINE
// ============================================================

static void handleQuestion(
  const String &question
) {

  if (!question.length())
    return;

  Serial.println();
  Serial.println(
    "=============================="
  );

  Serial.println(
    "TARS: QUESTION"
  );

  Serial.println(
    question
  );

  voiceSing =
    isSingRequest(
      question
    );

  aiAnswer =
    askAI(
      question
    );

  if (!aiAnswer.length()) {

    oledReady();

    return;
  }

  oledTypingPrepare(
    aiAnswer
  );

  // TTS = AI answer
  // SING = original question
  const String &audioText =
    voiceSing ?
    question :
    aiAnswer;

  if (
    !downloadMP3(
      voiceSing ?
      SING_URL :
      TTS_URL,
      audioText
    )
  ) {

    Serial.println(
      "TARS: AUDIO FAILED"
    );

    oledReady();

    return;
  }

  singMode =
    voiceSing;

  // IMPORTANT:
  // WiFi remains ON.
  // No wifiManagerDisconnect().
  playMP3();
}

// ============================================================
// VOICE START
// ============================================================

static void beginVoice() {

  if (
    voiceState !=
    VOICE_IDLE
  )
    return;

  if (
    !micOK ||
    playing
  )
    return;

  if (
    WiFi.status() !=
    WL_CONNECTED
  )
    return;

  if (
    !startRecording()
  )
    return;

  voiceState =
    VOICE_RECORD;

  stateAt =
    millis();
}

// ============================================================
// VOICE STATE MACHINE
// ============================================================

static void updateVoice() {

  switch (
    voiceState
  ) {

    case VOICE_IDLE:

      beginVoice();

      break;

    case VOICE_RECORD:

      if (
        !recordStep()
      ) {

        voiceState =
          VOICE_STT;

        stateAt =
          millis();
      }

      break;

    case VOICE_STT: {

      Serial.println(
        "TARS: STT START"
      );

      voiceText =
        speechToText();

      LittleFS.remove(
        STT_FILE
      );

      voiceText.trim();

      if (
        !voiceText.length()
      ) {

        Serial.println(
          "TARS: STT EMPTY"
        );

        oledReady();

        voiceState =
          VOICE_IDLE;

        break;
      }

      Serial.print(
        "TARS STT: "
      );

      Serial.println(
        voiceText
      );

      voiceState =
        VOICE_ASK;

      stateAt =
        millis();

      break;
    }

    case VOICE_ASK:

      handleQuestion(
        voiceText
      );

      voiceState =
        VOICE_FINISH;

      stateAt =
        millis();

      break;

    case VOICE_AUDIO:

      voiceState =
        VOICE_FINISH;

      break;

    case VOICE_PLAY:

      voiceState =
        VOICE_FINISH;

      break;

    case VOICE_FINISH:

      voiceText = "";
      aiAnswer = "";

      voiceSing = false;

      voiceState =
        VOICE_IDLE;

      break;
  }
}

// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(
    SERIAL_BAUD
  );

  // NO BOOT DELAY.
  // NO BOOT ANIMATION.

  Serial.println();
  Serial.println(
    "TARS: INIT"
  );

  heapInfo(
    "START"
  );

  // ----------------------------------------------------------
  // OLED
  // ----------------------------------------------------------

  Wire.begin(
    OLED_SDA,
    OLED_SCL
  );

  if (
    oled.begin(
      SSD1306_SWITCHCAPVCC,
      OLED_ADDR
    )
  ) {

    oledOK = true;

    oled.clearDisplay();
    oled.display();

    oledReady();
  }

  // ----------------------------------------------------------
  // LITTLEFS
  // ----------------------------------------------------------

  if (
    !LittleFS.begin(true)
  ) {

    Serial.println(
      "TARS: LITTLEFS FAILED"
    );

  } else {

    Serial.println(
      "TARS: LITTLEFS OK"
    );
  }

  // ----------------------------------------------------------
  // DAC
  // ----------------------------------------------------------

  dacOK =
    initDAC();

  // ----------------------------------------------------------
  // MIC
  // ----------------------------------------------------------

  micOK =
    initMic();

  // ----------------------------------------------------------
  // BLUETOOTH
  // ----------------------------------------------------------

  Serial.println(
    "TARS: BLUETOOTH DISABLED"
  );

  Serial.println(
    "TARS: SERIAL MONITOR ONLY"
  );

  // ----------------------------------------------------------
  // WIFI MANAGER
  // ----------------------------------------------------------

  if (
    wifiManagerBegin()
  ) {

    if (
      wifiManagerConnect(false)
    ) {

      Serial.println(
        "TARS: WIFI CONNECTED"
      );

      startNTP();

    } else {

      Serial.println(
        "TARS: WIFI NOT READY"
      );
    }

  } else {

    Serial.println(
      "TARS: WIFI SETUP REQUIRED"
    );
  }

  // ----------------------------------------------------------
  // READY
  // ----------------------------------------------------------

  heapInfo(
    "READY"
  );

  oledReady();

  Serial.println(
    "TARS: READY"
  );

  Serial.println(
    "TARS: SPEAK TO INMP441"
  );
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  // WiFi stays ON.
  maintainWiFi();

  // OLED animations continue independently.
  oledUpdate();

  // Voice pipeline.
  // No Serial input.
  if (
    !playing
  ) {

    updateVoice();
  }

  // Small cooperative yield.
  yield();
}
