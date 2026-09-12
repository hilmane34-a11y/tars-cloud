#include <Arduino.h>
#include "driver/i2s.h"

#define I2S_PORT I2S_NUM_0
#define DAC_GPIO 26
#define SAMPLE_RATE 22050

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println(" TARS I2S INTERNAL DAC TEST");
  Serial.println(" GPIO26 / DAC2 ONLY");
  Serial.println("================================");

  i2s_config_t config = {};
  config.mode = (i2s_mode_t)(
    I2S_MODE_MASTER |
    I2S_MODE_TX |
    I2S_MODE_DAC_BUILT_IN
  );
  config.sample_rate = SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT;
  config.communication_format = I2S_COMM_FORMAT_I2S_MSB;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 4;
  config.dma_buf_len = 256;
  config.use_apll = false;
  config.tx_desc_auto_clear = true;
  config.fixed_mclk = 0;

  esp_err_t err = i2s_driver_install(
    I2S_PORT,
    &config,
    0,
    NULL
  );

  if (err != ESP_OK) {
    Serial.printf(
      "I2S INSTALL ERROR = %d\r\n",
      (int)err
    );
    while (true) {
      delay(1000);
    }
  }

  err = i2s_set_dac_mode(
    I2S_DAC_CHANNEL_RIGHT_EN
  );

  if (err != ESP_OK) {
    Serial.printf(
      "DAC MODE ERROR = %d\r\n",
      (int)err
    );
    while (true) {
      delay(1000);
    }
  }

  i2s_zero_dma_buffer(I2S_PORT);

  Serial.println("TARS: I2S READY");
  Serial.println("TARS: INTERNAL DAC = GPIO26");
  Serial.println("TARS: DAC RIGHT ENABLED");
  Serial.println("TARS: STARTING 1kHz TEST");
}

void loop() {

  const uint32_t duration = SAMPLE_RATE * 5;
  const float frequency = 1000.0f;

  static int16_t buffer[256];

  uint32_t generated = 0;

  Serial.println("TARS: BEEP START");

  while (generated < duration) {

    uint32_t count = duration - generated;

    if (count > 256) {
      count = 256;
    }

    for (uint32_t i = 0; i < count; i++) {

      float t =
        (float)(generated + i) /
        (float)SAMPLE_RATE;

      float s =
        sinf(2.0f * PI * frequency * t);

      /*
       * Internal DAC ESP32:
       *
       * DAC value:
       * 0   = minimum
       * 128 = midpoint
       * 255 = maximum
       *
       * Put DAC value in upper 8 bits
       * of the 16-bit I2S sample.
       */

      uint8_t dacValue =
        (uint8_t)(128.0f + s * 100.0f);

      buffer[i] =
        (int16_t)((uint16_t)dacValue << 8);
    }

    size_t written = 0;

    esp_err_t err = i2s_write(
      I2S_PORT,
      buffer,
      count * sizeof(int16_t),
      &written,
      portMAX_DELAY
    );

    if (err != ESP_OK) {
      Serial.printf(
        "TARS: I2S WRITE ERROR = %d\r\n",
        (int)err
      );
      break;
    }

    generated += count;
  }

  i2s_zero_dma_buffer(I2S_PORT);

  Serial.println("TARS: BEEP END");
  delay(1000);
}
