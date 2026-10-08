#include "oled.h"

#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "sleep_oled.h"

extern bool cameraLive;
extern bool playing;
extern bool sleepPreparing;

extern void drawCameraOLED();

Adafruit_SSD1306 oled(
  OLED_WIDTH,
  OLED_HEIGHT,
  &Wire,
  -1
);

bool oledOK = false;

volatile uint8_t oledSpecial = 0;
volatile uint32_t oledDeadUntil = 0;
volatile uint32_t oledDoorStart = 0;

String oledText;
String oledStatus = "READY";

uint32_t oledTypePos = 0;
uint32_t oledLastType = 0;
uint32_t oledLastWave = 0;
uint32_t oledPage = 0;
uint32_t oledLastPage = 0;

void oledSetStatus(const String& s) {

  oledStatus = s;
  oledText = "";
  oledTypePos = 0;
  oledPage = 0;
  oledLastPage = millis();
}

void oledSetListening() {

  oledSetStatus("LISTENING");
}

void oledStartSpeak(const String& s) {

  oledStatus = "SPEAKING";
  oledText = s;

  oledTypePos = 0;
  oledPage = 0;

  oledLastType = millis();
  oledLastPage = millis();
}

void oledShowText(
  const String& s,
  const String& status
) {

  oledStatus = status;
  oledText = s;

  oledTypePos = s.length();
  oledPage = 0;

  oledLastType = millis();
  oledLastPage = millis();
}

void drawSpecialOLED(uint8_t m) {

  if (!oledOK)
    return;

  oled.clearDisplay();

  /*
   * Bagian gambar emosi/special OLED
   * tetap dipertahankan dari implementasi lama.
   *
   * Untuk sementara mode 0 = normal.
   */
  if (m == 0) {

    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);

    oled.println("TARS");

  } else {

    oled.setTextSize(2);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 20);

    if (m == 1)
      oled.println("...");
    else if (m == 2)
      oled.println("!");
    else
      oled.println("TARS");
  }

  oled.display();
}

void oledTask(void*) {

  for (;;) {

    if (!oledOK) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (sleepPreparing) {

      sleepOLEDUpdate(oled);

      vTaskDelay(pdMS_TO_TICKS(40));
      continue;
    }

    uint8_t special =
      oledSpecial;

    if (special) {

      drawSpecialOLED(special);

      vTaskDelay(pdMS_TO_TICKS(80));
      continue;
    }

    if (cameraLive) {

      drawCameraOLED();

      vTaskDelay(pdMS_TO_TICKS(40));
      continue;
    }

    oled.clearDisplay();

    oled.setTextColor(
      SSD1306_WHITE
    );

    oled.setTextSize(1);

    oled.setCursor(0, 0);

    oled.println(
      oledStatus
    );

    if (playing) {

      oled.setCursor(0, 16);

      oled.println(
        "TTS..."
      );

    } else if (oledText.length()) {

      uint32_t now =
        millis();

      if (oledText.length() &&
          now - oledLastType >= 39) {

        oledLastType = now;

        if (oledTypePos < oledText.length())
          oledTypePos++;
      }

      String shown =
        oledText.substring(
          0,
          oledTypePos
        );

      oled.setCursor(0, 16);

      oled.setTextSize(1);

      oled.println(
        shown
      );

    } else {

      oled.setCursor(0, 16);

      oled.println(
        "READY"
      );
    }

    oled.display();

    vTaskDelay(
      pdMS_TO_TICKS(40)
    );
  }
}
