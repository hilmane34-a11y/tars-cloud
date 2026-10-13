#include "oled.h"
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <string.h>
#include "config.h"
#include <sleep_oled.h>
#include <I2SCamera.h>

extern bool cameraLive, playing, sleepPreparing;
extern bool visionLiveEnabled();
extern SemaphoreHandle_t previewMux;
extern uint8_t cameraPreview[128 * 64];
extern bool previewReady;

Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

bool oledOK = false;
volatile uint8_t oledSpecial = 0;
volatile uint32_t oledDeadUntil = 0, oledDoorStart = 0;

String oledText, oledStatus = "READY";

uint32_t oledTypePos = 0;
uint32_t oledLastType = 0;
uint32_t oledLastWave = 0;
uint32_t oledPage = 0;
uint32_t oledLastPage = 0;

void oledSetStatus(const String &s) {
  oledStatus = s;
  oledText = "";
  oledTypePos = 0;
  oledPage = 0;
  oledLastPage = millis();
}

void oledSetListening() {
  oledSetStatus("LISTENING");
}

void oledStartSpeak(const String &s) {
  oledStatus = "SPEAKING";
  oledText = s;
  oledTypePos = 0;
  oledPage = 0;
  oledLastType = millis();
  oledLastPage = millis();
}

void oledShowText(const String &s, const String &status) {
  oledStatus = status;
  oledText = s;
  oledTypePos = s.length();
  oledPage = 0;
  oledLastType = millis();
  oledLastPage = millis();
}

void drawSpecialOLED(uint8_t m) {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  oled.drawLine(15, 55, 8, 37, 1);
  oled.drawLine(8, 37, 8, 22, 1);
  oled.drawLine(8, 22, 4, 17, 1);
  oled.drawLine(8, 22, 8, 14, 1);
  oled.drawLine(8, 22, 12, 15, 1);

  oled.drawLine(113, 55, 120, 37, 1);
  oled.drawLine(120, 37, 120, 22, 1);
  oled.drawLine(120, 22, 124, 17, 1);
  oled.drawLine(120, 22, 120, 14, 1);
  oled.drawLine(120, 22, 116, 15, 1);

  if (m == 1) {
    oled.fillCircle(42, 25, 8, 1);
    oled.fillCircle(86, 25, 8, 1);
    oled.drawLine(45, 44, 83, 44, 1);
  } else {
    oled.drawLine(34, 18, 49, 32, 1);
    oled.drawLine(49, 18, 34, 32, 1);
    oled.drawLine(79, 18, 94, 32, 1);
    oled.drawLine(94, 18, 79, 32, 1);

    oled.drawCircle(64, 45, 7, 1);
    oled.fillRect(61, 49, 6, 4, 0);
    oled.drawLine(64, 52, 64, 57, 1);
    oled.drawLine(64, 57, 69, 57, 1);

    uint32_t e = millis() - oledDoorStart;
    int bx = 5 + (int)((e / 35U > 48U) ? 48U : e / 35U);

    oled.drawLine(bx - 10, 27, bx - 2, 27, 1);
    oled.drawLine(bx - 8, 30, bx - 2, 30, 1);
    oled.fillCircle(bx, 27, 3, 1);

    if (e > 1700) {
      oled.drawLine(53, 23, 58, 28, 1);
      oled.drawLine(58, 23, 53, 28, 1);
    }
  }

  oled.display();
}

void drawCameraOLED() {
  if (!oledOK || !previewMux) return;

  if (xSemaphoreTake(previewMux, pdMS_TO_TICKS(100)) != pdTRUE) {
    return;
  }

  if (!previewReady) {
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(24, 28);
    oled.print("CAMERA WAIT");
    oled.display();

    xSemaphoreGive(previewMux);
    return;
  }

  oled.clearDisplay();

  for (int y = 0; y < 64; y++) {
    for (int x = 0; x < 128; x++) {
      if (cameraPreview[y * 128 + x]) {
        oled.drawPixel(x, y, SSD1306_WHITE);
      }
    }
  }

  oled.display();
  xSemaphoreGive(previewMux);
}

// Renderer teks memakai buffer tetap.
// Tidak membuat substring atau String baru untuk setiap karakter/baris.
static bool drawTextPage(uint32_t visibleLen, uint32_t targetPage) {
  char line[21];
  uint8_t lineLen = 0;
  uint32_t lineNo = 0;
  uint8_t shown = 0;
  bool next = false;

  line[0] = '\0';

  for (uint32_t i = 0; i <= visibleLen; i++) {
    char c = (i < visibleLen) ? oledText[i] : '\0';

    if (c == '\r') {
      continue;
    }

    if (c == '\n' || c == '\0') {
      line[lineLen] = '\0';

      if (lineNo >= targetPage && shown < 4) {
        oled.setCursor(3, 29 + shown * 8);
        oled.print(line);
        shown++;
      }

      lineLen = 0;
      line[0] = '\0';
      lineNo++;

      if (shown >= 4) {
        next = (i < visibleLen);
        break;
      }

      continue;
    }

    if (lineLen < 20) {
      line[lineLen++] = c;
      line[lineLen] = '\0';
      continue;
    }

    // Baris sudah mencapai 20 karakter.
    // Jika ada spasi, pindahkan kata terakhir ke baris berikutnya.
    int cut = -1;

    for (int k = (int)lineLen - 1; k >= 0; k--) {
      if (line[k] == ' ') {
        cut = k;
        break;
      }
    }

    if (cut > 0) {
      char rest[21];
      uint8_t restLen = lineLen - cut - 1;

      if (restLen > 20) restLen = 20;

      memcpy(rest, line + cut + 1, restLen);
      rest[restLen] = '\0';

      line[cut] = '\0';

      if (lineNo >= targetPage && shown < 4) {
        oled.setCursor(3, 29 + shown * 8);
        oled.print(line);
        shown++;
      }

      lineNo++;

      if (shown >= 4) {
        next = true;
        break;
      }

      memcpy(line, rest, restLen + 1);
      lineLen = restLen;

      // Karakter yang sedang diproses menjadi bagian baris baru.
      if (lineLen < 20) {
        line[lineLen++] = c;
        line[lineLen] = '\0';
      }
    } else {
      // Kata panjang tanpa spasi: pecah agar buffer tetap aman.
      line[lineLen] = '\0';

      if (lineNo >= targetPage && shown < 4) {
        oled.setCursor(3, 29 + shown * 8);
        oled.print(line);
        shown++;
      }

      lineNo++;

      if (shown >= 4) {
        next = true;
        break;
      }

      lineLen = 0;
      line[0] = '\0';
      line[lineLen++] = c;
      line[lineLen] = '\0';
    }

    if ((i & 63U) == 63U) {
      vTaskDelay(1);
    }
  }

  return next;
}

void oledTask(void *) {
  for (;;) {
    if (!oledOK) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    uint32_t now = millis();

    if (oledSpecial) {
      if (oledSpecial == 2 && now >= oledDeadUntil) {
        oledSpecial = 0;
        oledSetStatus(visionLiveEnabled() ? "LISTENING" : "READY");
      } else {
        drawSpecialOLED(oledSpecial);
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
    }

    if (sleepPreparing) {
      sleepOLEDUpdate(oled);
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    if (cameraLive && !playing) {
      drawCameraOLED();
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    uint32_t textLen = oledText.length();

    if (textLen &&
        oledTypePos < textLen &&
        now - oledLastType >= OLED_TYPE_MS) {
      oledTypePos++;
      oledLastType = now;
    }

    if (now - oledLastWave >= OLED_WAVE_MS) {
      oledLastWave = now;

      oled.clearDisplay();
      oled.setTextColor(SSD1306_WHITE);
      oled.setTextSize(2);
      oled.setCursor(36, 0);
      oled.print("TARS");

      oled.setTextSize(1);
      oled.setCursor(3, 17);
      oled.print(oledStatus);

      textLen = oledText.length();

      if (textLen) {
        uint32_t visibleLen =
          (oledTypePos < textLen) ? oledTypePos : textLen;

        uint32_t target = oledPage * 4;
        bool next = drawTextPage(visibleLen, target);

        if (oledStatus == "SPEAKING" &&
            now - oledLastPage >= OLED_PAGE_MS) {
          if (next) {
            oledPage++;
          } else if (oledPage > 0) {
            // Teks yang masih diketik dapat membuat halaman
            // berikutnya belum tersedia. Jangan memaksa reset.
          }

          oledLastPage = now;
        }
      }

      if (oledStatus == "LISTENING") {
        int x = 64 + (int)(sin(now / 120.0) * 25);
        oled.drawCircle(x, 56, 4, SSD1306_WHITE);
      } else if (oledStatus == "SPEAKING") {
        int w = 8 + (now / 40) % 18;
        oled.fillRect(64 - w / 2, 51, w, 6, SSD1306_WHITE);
      }

      oled.display();
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
