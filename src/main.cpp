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

#include <AudioTools.h>
#include <CodecMP3Helix.h>

#include "config.h"
#include "wifi_manager.h"

// ============================================================
// TARS ESP32
// INMP441 + OLED + STT + ASK + TTS MP3
// Bluetooth OFF
// ============================================================

// -------------------- WIFI / CLOUD --------------------

#ifndef WIFI_SSID
#define WIFI_SSID "HomeManz"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "rumahanak4"
#endif

const char* WORKER_URL = "https://tars-cloud-v1.hilmane34.workers.dev";

// -------------------- OLED --------------------

#define OLED_ADDR 0x3C
#define OLED_SDA  21
#define OLED_SCL  22
#define OLED_W    128
#define OLED_H    64

Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);

// -------------------- INMP441 --------------------

#define I2S_PORT I2S_NUM_1

#define MIC_SCK 18
#define MIC_WS  19
#define MIC_SD  34

// -------------------- DAC --------------------

#define DAC_PIN 26

// -------------------- AUDIO --------------------

#define SAMPLE_RATE 16000
#define RECORD_SECONDS 4

#define MAX_SAMPLES (SAMPLE_RATE * RECORD_SECONDS)

static int16_t* micBuffer = nullptr;

#define ENV_POINTS 256

static uint8_t envelopeData[ENV_POINTS];

// ============================================================
// TEMPLATE SUARA
// ============================================================

// TARS
// JUMLAH = 64
const uint8_t TEMPLATE_TARS[64] = {
  0,18,105,209,248,253,255,255,255,253,242,229,233,228,225,228,
  214,219,233,230,232,231,215,215,226,224,225,228,224,230,237,226,
  216,212,209,221,235,222,209,211,192,163,146,102,33,9,29,25,
  4,4,4,0,0,5,9,9,9,8,10,11,9,6,5,4
};

// HIDUP JOKOWI
// JUMLAH = 64
const uint8_t TEMPLATE_JOKOWI[64] = {
  0,54,192,227,253,144,35,70,237,245,83,6,0,2,7,12,
  115,242,255,227,45,2,0,40,16,85,207,210,203,140,112,120,
  125,125,134,134,112,105,93,90,88,80,76,70,65,69,75,74,
  79,81,78,77,76,66,61,42,36,42,39,31,23,20,16,3
};

// CERITAKAN PENGALAMANMU
// JUMLAH = 64
const uint8_t TEMPLATE_EXP[64] = {
  3,17,186,237,255,229,59,150,120,106,41,1,0,2,175,255,
  229,58,4,1,30,39,206,249,227,84,57,41,10,0,0,47,
  180,73,92,101,197,213,219,215,211,248,219,201,157,69,72,157,
  184,162,67,51,65,70,75,95,132,139,137,119,83,40,30,12
};

// ============================================================
// CLASSIFIER
// ============================================================

enum InputType {
  INPUT_BLOCKED,
  INPUT_WAKE_TARS,
  INPUT_OFFLINE_JOKOWI,
  INPUT_OFFLINE_EXP
};

// Durasi aktif rekaman asli dalam frame envelope
const uint16_t DUR_TARS   = 70;
const uint16_t DUR_JOKOWI = 121;
const uint16_t DUR_EXP    = 179;

// Threshold classifier
const float WAKE_TH          = 0.24f;
const float OFFLINE_JOKOWI_TH = 0.27f;
const float OFFLINE_EXP_TH    = 0.30f;
const float MATCH_MARGIN      = 0.025f;

// ============================================================
// OLED
// ============================================================

void oledClear() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
}

void oledText(const String& text) {
  oledClear();
  display.println(text);
  display.display();
}

void oledMulti(const String& a, const String& b = "") {
  oledClear();
  display.println(a);
  if (b.length()) display.println(b);
  display.display();
}

// ============================================================
// I2S MIC
// ============================================================

void initMic() {

  i2s_config_t cfg;

  memset(&cfg, 0, sizeof(cfg));

  cfg.mode =
      (i2s_mode_t)(
        I2S_MODE_MASTER |
        I2S_MODE_RX
      );

  cfg.sample_rate = SAMPLE_RATE;

  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;

  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;

  cfg.communication_format = I2S_COMM_FORMAT_I2S;

  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;

  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;

  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = false;
  cfg.fixed_mclk = 0;

  i2s_pin_config_t pins;

  memset(&pins, 0, sizeof(pins));

  pins.bck_io_num = MIC_SCK;
  pins.ws_io_num = MIC_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = MIC_SD;

  i2s_driver_install(I2S_PORT, &cfg, 0, nullptr);
  i2s_set_pin(I2S_PORT, &pins);
  i2s_zero_dma_buffer(I2S_PORT);
}

// ============================================================
// DAC
// ============================================================

void initDAC() {
  pinMode(DAC_PIN, OUTPUT);
}

// ============================================================
// AUDIO ENVELOPE
// ============================================================

void buildEnvelope(
  const int16_t* samples,
  uint32_t count,
  uint8_t* env,
  uint16_t points
) {

  if (!samples || count == 0) {
    memset(env, 0, points);
    return;
  }

  uint32_t block = count / points;

  if (block < 1)
    block = 1;

  uint32_t maxVal = 1;

  for (uint16_t p = 0; p < points; p++) {

    uint32_t start = p * block;
    uint32_t end = start + block;

    if (start >= count)
      start = count - 1;

    if (end > count)
      end = count;

    uint32_t sum = 0;
    uint32_t n = 0;

    for (uint32_t i = start; i < end; i++) {

      int32_t v = samples[i];

      if (v < 0)
        v = -v;

      sum += (uint32_t)v;
      n++;
    }

    uint32_t avg = n ? sum / n : 0;

    if (avg > maxVal)
      maxVal = avg;

    env[p] = (uint8_t)min(avg / 128, (uint32_t)255);
  }

  // Normalisasi peak
  if (maxVal > 1) {

    for (uint16_t i = 0; i < points; i++) {

      uint32_t v = env[i];

      v = (v * 255UL) / min(maxVal / 128UL, 255UL);

      if (v > 255)
        v = 255;

      env[i] = (uint8_t)v;
    }
  }
}

// ============================================================
// WHOLE UTTERANCE TEMPLATE SCORE
// ============================================================

float wholeTemplateScore(
  const uint8_t* e,
  uint16_t n,
  const uint8_t* t,
  uint16_t expectedDur
) {

  if (!e || !t || n < 20)
    return 1.0f;

  uint8_t peak = 0;

  for (uint16_t i = 0; i < n; i++) {
    if (e[i] > peak)
      peak = e[i];
  }

  if (peak < 20)
    return 1.0f;

  // Hilangkan silence dengan adaptive gate
  uint8_t gate = max(
    (uint8_t)8,
    (uint8_t)(peak * 0.10f)
  );

  int first = -1;
  int last = -1;

  for (uint16_t i = 0; i < n; i++) {

    if (e[i] >= gate) {

      if (first < 0)
        first = i;

      last = i;
    }
  }

  if (first < 0 || last <= first)
    return 1.0f;

  uint16_t len = last - first + 1;

  if (len < 20)
    return 1.0f;

  // Normalisasi range amplitude
  uint8_t lo = 255;
  uint8_t hi = 0;

  for (uint16_t i = first; i <= last; i++) {

    lo = min(lo, e[i]);
    hi = max(hi, e[i]);
  }

  if (hi <= lo + 4)
    return 1.0f;

  // Bandingkan seluruh bentuk utterance
  float shape = 0.0f;

  for (uint8_t j = 0; j < 64; j++) {

    float q =
      (float)j *
      (float)(len - 1) /
      63.0f;

    uint16_t a = (uint16_t)q;

    uint16_t b =
      (a + 1 < len)
      ? a + 1
      : a;

    float v =
      e[first + a] +
      (e[first + b] - e[first + a]) *
      (q - a);

    v =
      constrain(
        (v - lo) * 255.0f /
        (hi - lo),
        0.0f,
        255.0f
      );

    shape += fabsf(
      v - t[j]
    ) / 255.0f;
  }

  shape /= 64.0f;

  // Penalti durasi
  float ratio =
    (float)len /
    (float)expectedDur;

  float duration =
    fabsf(
      logf(
        max(ratio, 0.05f)
      )
    );

  if (duration > 1.0f)
    duration = 1.0f;

  // 72% bentuk + 28% durasi
  return
    shape * 0.72f +
    duration * 0.28f;
}

// ============================================================
// CLASSIFY
// ============================================================

InputType classifyUtterance(
  const uint8_t* e,
  uint16_t n
) {

  float score[3];

  score[0] =
    wholeTemplateScore(
      e,
      n,
      TEMPLATE_TARS,
      DUR_TARS
    );

  score[1] =
    wholeTemplateScore(
      e,
      n,
      TEMPLATE_JOKOWI,
      DUR_JOKOWI
    );

  score[2] =
    wholeTemplateScore(
      e,
      n,
      TEMPLATE_EXP,
      DUR_EXP
    );

  int best = 0;

  if (score[1] < score[best])
    best = 1;

  if (score[2] < score[best])
    best = 2;

  float second = 1.0f;

  for (int i = 0; i < 3; i++) {

    if (i != best &&
        score[i] < second) {

      second = score[i];
    }
  }

  float margin =
    second - score[best];

  Serial.printf(
    "TARS: WAKE %.3f | JOKOWI %.3f | EXP %.3f\n",
    score[0],
    score[1],
    score[2]
  );

  Serial.printf(
    "TARS: BEST=%d MARGIN=%.3f\n",
    best,
    margin
  );

  // Hanya suara TARS yang boleh membuka STT
  if (
    best == 0 &&
    score[0] <= WAKE_TH &&
    margin >= MATCH_MARGIN
  ) {

    Serial.println(
      "TARS: WAKE -> STT ONLINE"
    );

    return INPUT_WAKE_TARS;
  }

  // Hidup Jokowi -> OFFLINE
  if (
    best == 1 &&
    score[1] <= OFFLINE_JOKOWI_TH &&
    margin >= MATCH_MARGIN
  ) {

    Serial.println(
      "TARS: OFFLINE -> HIDUP JOKOWI"
    );

    return INPUT_OFFLINE_JOKOWI;
  }

  // Ceritakan pengalamanmu -> OFFLINE
  if (
    best == 2 &&
    score[2] <= OFFLINE_EXP_TH &&
    margin >= MATCH_MARGIN
  ) {

    Serial.println(
      "TARS: OFFLINE -> CERITAKAN PENGALAMANMU"
    );

    return INPUT_OFFLINE_EXP;
  }

  Serial.printf(
    "TARS: UNKNOWN -> BLOCKED "
    "(BEST %.3f / MARGIN %.3f)\n",
    score[best],
    margin
  );

  return INPUT_BLOCKED;
}

// ============================================================
// RECORD MIC
// ============================================================

uint32_t recordMic() {

  if (!micBuffer)
    return 0;

  memset(
    micBuffer,
    0,
    MAX_SAMPLES * sizeof(int16_t)
  );

  oledText("TARS MENDENGAR...");

  size_t bytesRead = 0;

  uint32_t samplesWritten = 0;

  uint32_t start = millis();

  const uint32_t duration =
    RECORD_SECONDS * 1000UL;

  while (
    millis() - start < duration &&
    samplesWritten < MAX_SAMPLES
  ) {

    int32_t raw[256];

    size_t bytes = 0;

    esp_err_t err =
      i2s_read(
        I2S_PORT,
        raw,
        sizeof(raw),
        &bytes,
        100
      );

    if (
      err != ESP_OK ||
      bytes == 0
    ) {
      continue;
    }

    uint16_t count =
      bytes / sizeof(int32_t);

    for (
      uint16_t i = 0;
      i < count &&
      samplesWritten < MAX_SAMPLES;
      i++
    ) {

      int32_t s =
        raw[i] >> 14;

      if (s > 32767)
        s = 32767;

      if (s < -32768)
        s = -32768;

      micBuffer[
        samplesWritten++
      ] = (int16_t)s;
    }
  }

  Serial.printf(
    "TARS: REC %lu samples\n",
    (unsigned long)samplesWritten
  );

  return samplesWritten;
}

// ============================================================
// NTP
// ============================================================

bool timeValid() {

  time_t now = time(nullptr);

  return now > 1700000000;
}

bool syncTimeOnce() {

  if (timeValid()) {
    Serial.println(
      "TARS: NTP already valid"
    );
    return true;
  }

  configTime(
    7 * 3600,
    0,
    "pool.ntp.org",
    "time.nist.gov",
    "time.google.com"
  );

  for (int attempt = 1; attempt <= 4; attempt++) {

    Serial.printf(
      "TARS: NTP attempt %d/4\n",
      attempt
    );

    uint32_t start = millis();

    while (
      millis() - start < 5000
    ) {

      if (timeValid()) {

        Serial.println(
          "TARS: NTP VALID"
        );

        return true;
      }

      delay(250);
    }
  }

  Serial.println(
    "TARS: NTP FAILED"
  );

  return false;
}

// ============================================================
// WIFI
// ============================================================

bool ensureWiFi() {

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {
    return true;
  }

  Serial.println(
    "TARS: WIFI CONNECTING..."
  );

  WiFi.mode(WIFI_STA);

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );

  uint32_t start = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start < 15000
  ) {

    delay(250);
    Serial.print(".");
  }

  Serial.println();

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    Serial.println(
      "TARS: WIFI CONNECTED"
    );

    Serial.println(
      WiFi.localIP()
    );

    return true;
  }

  Serial.println(
    "TARS: WIFI FAILED"
  );

  return false;
}

// ============================================================
// HTTP BODY
// ============================================================

String httpBody(
  HTTPClient& http
) {

  String body =
    http.getString();

  body.trim();

  return body;
}

// ============================================================
// STT
// ============================================================

String sendSTT(
  const int16_t* samples,
  uint32_t count
) {

  if (!ensureWiFi())
    return "";

  if (!timeValid()) {

    Serial.println(
      "TARS: STT BLOCKED - TIME INVALID"
    );

    return "";
  }

  WiFiClientSecure client;

  client.setInsecure();

  HTTPClient http;

  String url =
    String(WORKER_URL) +
    "/stt";

  if (!http.begin(client, url)) {

    Serial.println(
      "TARS: STT HTTP BEGIN FAILED"
    );

    return "";
  }

  http.addHeader(
    "Content-Type",
    "audio/wav"
  );

  // WAV header
  const uint32_t dataSize =
    count * sizeof(int16_t);

  const uint32_t fileSize =
    44 + dataSize;

  uint8_t header[44];

  memset(header, 0, sizeof(header));

  memcpy(header, "RIFF", 4);

  uint32_t chunkSize =
    fileSize - 8;

  memcpy(
    header + 4,
    &chunkSize,
    4
  );

  memcpy(
    header + 8,
    "WAVE",
    4
  );

  memcpy(
    header + 12,
    "fmt ",
    4
  );

  uint32_t subchunk1 =
    16;

  uint16_t audioFormat =
    1;

  uint16_t channels =
    1;

  uint32_t sampleRate =
    SAMPLE_RATE;

  uint32_t byteRate =
    SAMPLE_RATE *
    channels *
    sizeof(int16_t);

  uint16_t blockAlign =
    channels *
    sizeof(int16_t);

  uint16_t bits =
    16;

  memcpy(header + 16, &subchunk1, 4);
  memcpy(header + 20, &audioFormat, 2);
  memcpy(header + 22, &channels, 2);
  memcpy(header + 24, &sampleRate, 4);
  memcpy(header + 28, &byteRate, 4);
  memcpy(header + 32, &blockAlign, 2);
  memcpy(header + 34, &bits, 2);

  memcpy(
    header + 36,
    "data",
    4
  );

  memcpy(
    header + 40,
    &dataSize,
    4
  );

  // Gabungkan header + audio
  uint8_t* wav =
    (uint8_t*)malloc(fileSize);

  if (!wav) {

    Serial.println(
      "TARS: STT WAV MALLOC FAILED"
    );

    http.end();

    return "";
  }

  memcpy(
    wav,
    header,
    44
  );

  memcpy(
    wav + 44,
    samples,
    dataSize
  );

  Serial.printf(
    "TARS: STT upload %lu bytes\n",
    (unsigned long)fileSize
  );

  int code =
    http.POST(
      wav,
      fileSize
    );

  free(wav);

  if (code <= 0) {

    Serial.printf(
      "TARS: STT HTTP ERROR %d\n",
      code
    );

    http.end();

    return "";
  }

  String body =
    httpBody(http);

  http.end();

  Serial.printf(
    "TARS: STT RESPONSE: %s\n",
    body.c_str()
  );

  JsonDocument doc;

  DeserializationError err =
    deserializeJson(
      doc,
      body
    );

  if (err) {

    Serial.printf(
      "TARS: STT JSON ERROR: %s\n",
      err.c_str()
    );

    return "";
  }

  String text =
    doc["text"] |
    doc["transcript"] |
    "";

  text.trim();

  return text;
}

// ============================================================
// ASK
// ============================================================

String askTARS(
  const String& question
) {

  if (!ensureWiFi())
    return "";

  if (!timeValid()) {

    Serial.println(
      "TARS: ASK BLOCKED - TIME INVALID"
    );

    return "";
  }

  WiFiClientSecure client;

  client.setInsecure();

  HTTPClient http;

  String url =
    String(WORKER_URL) +
    "/ask";

  if (!http.begin(client, url))
    return "";

  http.addHeader(
    "Content-Type",
    "application/json"
  );

  JsonDocument req;

  req["text"] = question;

  String payload;

  serializeJson(
    req,
    payload
  );

  int code =
    http.POST(payload);

  if (code <= 0) {

    Serial.printf(
      "TARS: ASK HTTP ERROR %d\n",
      code
    );

    http.end();

    return "";
  }

  String body =
    httpBody(http);

  http.end();

  Serial.printf(
    "TARS: ASK RESPONSE: %s\n",
    body.c_str()
  );

  JsonDocument doc;

  if (
    deserializeJson(
      doc,
      body
    )
  ) {
    return "";
  }

  String answer =
    doc["answer"] |
    doc["response"] |
    doc["text"] |
    "";

  answer.trim();

  return answer;
}

// ============================================================
// TTS MP3 DOWNLOAD
// ============================================================

bool downloadMP3(
  const String& text
) {

  if (!ensureWiFi())
    return false;

  if (!timeValid())
    return false;

  WiFiClientSecure client;

  client.setInsecure();

  HTTPClient http;

  String url =
    String(WORKER_URL) +
    "/tts";

  if (!http.begin(client, url))
    return false;

  http.addHeader(
    "Content-Type",
    "application/json"
  );

  JsonDocument req;

  req["text"] = text;

  String payload;

  serializeJson(
    req,
    payload
  );

  int code =
    http.POST(payload);

  if (code != 200) {

    Serial.printf(
      "TARS: TTS HTTP ERROR %d\n",
      code
    );

    http.end();

    return false;
  }

  File file =
    LittleFS.open(
      "/tars.mp3",
      "w"
    );

  if (!file) {

    Serial.println(
      "TARS: MP3 FILE OPEN FAILED"
    );

    http.end();

    return false;
  }

  WiFiClient* stream =
    http.getStreamPtr();

  uint8_t buffer[1024];

  int remaining =
    http.getSize();

  while (
    http.connected() &&
    (remaining > 0 || remaining == -1)
  ) {

    size_t available =
      stream->available();

    if (available) {

      size_t toRead =
        min(
          available,
          sizeof(buffer)
        );

      int n =
        stream->readBytes(
          buffer,
          toRead
        );

      if (n > 0) {

        file.write(
          buffer,
          n
        );

        if (remaining > 0)
          remaining -= n;
      }

    } else {

      delay(1);
    }
  }

  file.close();

  http.end();

  File check =
    LittleFS.open(
      "/tars.mp3",
      "r"
    );

  size_t size =
    check ? check.size() : 0;

  if (check)
    check.close();

  Serial.printf(
    "TARS: MP3 SIZE %u\n",
    (unsigned)size
  );

  return size > 128;
}

// ============================================================
// MP3 OUTPUT
// ============================================================

class DACOutput : public AudioStream {

public:

  DACOutput() {}

  virtual ~DACOutput() {}

  virtual size_t write(
    const uint8_t* data,
    size_t len
  ) override {

    if (!data || !len)
      return 0;

    const int16_t* pcm =
      (const int16_t*)data;

    size_t samples =
      len / 2;

    for (
      size_t i = 0;
      i < samples;
      i++
    ) {

      int32_t s =
        pcm[i];

      s =
        (s + 32768) >> 8;

      if (s < 0)
        s = 0;

      if (s > 255)
        s = 255;

      dacWrite(
        DAC_PIN,
        (uint8_t)s
      );
    }

    return len;
  }

  virtual int available() {
    return 1;
  }

  virtual void flush() {}
};

DACOutput dacOutput;

CodecMP3Helix decoder;

AudioInfo audioInfo(
  SAMPLE_RATE,
  1,
  16
);

StreamCopy copier(
  dacOutput,
  decoder
);

// ============================================================
// PLAY MP3
// ============================================================

bool playMP3() {

  File file =
    LittleFS.open(
      "/tars.mp3",
      "r"
    );

  if (!file) {

    Serial.println(
      "TARS: MP3 NOT FOUND"
    );

    return false;
  }

  oledText(
    "TARS BERBICARA..."
  );

  decoder.begin();

  Serial.println(
    "TARS: PLAY MP3"
  );

  uint8_t buffer[1024];

  while (file.available()) {

    size_t n =
      file.read(
        buffer,
        sizeof(buffer)
      );

    if (!n)
      break;

    decoder.write(
      buffer,
      n
    );

    delay(1);
  }

  file.close();

  decoder.end();

  dacWrite(
    DAC_PIN,
    128
  );

  Serial.println(
    "TARS: MP3 DONE"
  );

  return true;
}

// ============================================================
// CLOUD QUESTION
// ============================================================

void processQuestion(
  const String& question
) {

  if (!question.length())
    return;

  Serial.printf(
    "TARS: QUESTION = %s\n",
    question.c_str()
  );

  oledText(
    "TARS MENJAWAB..."
  );

  String answer =
    askTARS(question);

  if (!answer.length()) {

    Serial.println(
      "TARS: ASK EMPTY"
    );

    oledText(
      "TARS GAGAL MENJAWAB"
    );

    delay(1000);

    return;
  }

  Serial.printf(
    "TARS: ANSWER = %s\n",
    answer.c_str()
  );

  oledText(
    "TARS MENYIAPKAN SUARA..."
  );

  if (
    downloadMP3(answer)
  ) {

    // Setelah MP3 selesai di-download,
    // WiFi boleh dimatikan sebelum playback.
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    delay(100);

    playMP3();

    // WiFi hidup kembali setelah playback.
    WiFi.mode(WIFI_STA);
    WiFi.begin(
      WIFI_SSID,
      WIFI_PASSWORD
    );

    uint32_t start =
      millis();

    while (
      WiFi.status() != WL_CONNECTED &&
      millis() - start < 10000
    ) {
      delay(200);
    }

    // Tidak NTP lagi di sini.
    // NTP hanya sinkronisasi satu kali.
  }

  oledText(
    "TARS MENUNGGU TUAN"
  );
}

// ============================================================
// OFFLINE ANSWERS
// ============================================================

void offlineAnswer(
  InputType type
) {

  if (
    type ==
    INPUT_OFFLINE_JOKOWI
  ) {

    Serial.println(
      "TARS: OFFLINE HIDUP JOKOWI"
    );

    oledMulti(
      "TARS OFFLINE",
      "HIDUP JOKOWI"
    );

    // Tidak STT.
    // Tidak ASK.
    // Tidak internet.

    return;
  }

  if (
    type ==
    INPUT_OFFLINE_EXP
  ) {

    Serial.println(
      "TARS: OFFLINE PENGALAMAN"
    );

    oledMulti(
      "TARS OFFLINE",
      "PENGALAMAN TARS"
    );

    // Tidak STT.
    // Tidak ASK.
    // Tidak internet.

    return;
  }
}

// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  delay(500);

  Serial.println();
  Serial.println(
    "=============================="
  );
  Serial.println(
    "       TARS ESP32 V1"
  );
  Serial.println(
    "       AUDIO + STT"
  );
  Serial.println(
    "=============================="
  );

  // OLED
  Wire.begin(
    OLED_SDA,
    OLED_SCL
  );

  if (
    !display.begin(
      SSD1306_SWITCHCAPVCC,
      OLED_ADDR
    )
  ) {

    Serial.println(
      "TARS: OLED FAILED"
    );

  } else {

    oledText(
      "TARS BOOTING..."
    );
  }

  // LittleFS
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

  initDAC();
  initMic();

  // Buffer microphone
  micBuffer =
    (int16_t*)malloc(
      MAX_SAMPLES *
      sizeof(int16_t)
    );

  if (!micBuffer) {

    Serial.println(
      "TARS: MIC BUFFER FAILED"
    );

    oledText(
      "MIC BUFFER ERROR"
    );

    while (true)
      delay(1000);
  }

  // WiFi pertama kali
  ensureWiFi();

  // NTP hanya satu kali.
  // Kalau gagal, ulang maksimal 4 kali.
  if (WiFi.status() == WL_CONNECTED) {

    if (!syncTimeOnce()) {

      Serial.println(
        "TARS: TIME INVALID"
      );

      oledText(
        "NTP GAGAL"
      );
    }
  }

  oledText(
    "TARS MENUNGGU TUAN"
  );

  Serial.println(
    "TARS: READY"
  );
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  // Pastikan WiFi tersedia untuk proses berikutnya.
  // Reconnect TIDAK melakukan NTP ulang.
  if (
    WiFi.status() != WL_CONNECTED
  ) {

    ensureWiFi();
  }

  uint32_t count =
    recordMic();

  if (
    count < SAMPLE_RATE / 4
  ) {

    Serial.println(
      "TARS: RECORD TOO SHORT"
    );

    return;
  }

  // Buat envelope
  buildEnvelope(
    micBuffer,
    count,
    envelopeData,
    ENV_POINTS
  );

  // ========================================================
  // CLASSIFICATION LOCAL
  // ========================================================

  InputType type =
    classifyUtterance(
      envelopeData,
      ENV_POINTS
    );

  // ========================================================
  // UNKNOWN -> BLOCKED
  // WAJIB berhenti di sini.
  // Tidak boleh STT.
  // ========================================================

  if (
    type ==
    INPUT_BLOCKED
  ) {

    oledText(
      "TARS TIDAK MENGERTI"
    );

    delay(700);

    oledText(
      "TARS MENUNGGU TUAN"
    );

    return;
  }

  // ========================================================
  // HIDUP JOKOWI -> OFFLINE
  // ========================================================

  if (
    type ==
    INPUT_OFFLINE_JOKOWI
  ) {

    offlineAnswer(type);

    delay(700);

    oledText(
      "TARS MENUNGGU TUAN"
    );

    return;
  }

  // ========================================================
  // CERITAKAN PENGALAMANMU -> OFFLINE
  // ========================================================

  if (
    type ==
    INPUT_OFFLINE_EXP
  ) {

    offlineAnswer(type);

    delay(700);

    oledText(
      "TARS MENUNGGU TUAN"
    );

    return;
  }

  // ========================================================
  // HANYA TARS -> STT ONLINE
  // ========================================================

  if (
    type ==
    INPUT_WAKE_TARS
  ) {

    oledText(
      "TARS SIAP, TUAN"
    );

    delay(300);

    String text =
      sendSTT(
        micBuffer,
        count
      );

    text.trim();

    if (!text.length()) {

      Serial.println(
        "TARS: STT EMPTY"
      );

      oledText(
        "TARS TIDAK MENDENGAR"
      );

      delay(700);

      oledText(
        "TARS MENUNGGU TUAN"
      );

      return;
    }

    Serial.printf(
      "TARS: STT = %s\n",
      text.c_str()
    );

    // Pertanyaan hasil STT baru masuk ke ASK.
    processQuestion(text);

    oledText(
      "TARS MENUNGGU TUAN"
    );

    return;
  }

  delay(20);
}
