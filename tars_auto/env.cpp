#include "env.h"

static EnvState envState = {};

void envBegin() {
  envState = {};
}

bool envAnalyze(const uint8_t* frame, uint16_t width,
                uint16_t height, EnvState &result) {
  result = {};

  if (!frame || width < 12 || height < 12) {
    envState = result;
    return false;
  }

  uint32_t sum[3] = {};
  uint32_t edges[3] = {};
  uint32_t samples[3] = {};

  for (uint16_t y = height / 4; y < height * 3 / 4; y += 2) {
    for (uint16_t x = 2; x < width - 2; x += 2) {
      uint8_t region = (x < width / 3) ? 0 :
                       (x < (width * 2 / 3)) ? 1 : 2;

      uint16_t i = (y * width + x) * 2;

      // Frame RGB565
      uint16_t p = ((uint16_t)frame[i] << 8) | frame[i + 1];

      uint8_t r = (p >> 11) & 0x1F;
      uint8_t g = (p >> 5) & 0x3F;
      uint8_t b = p & 0x1F;

      uint8_t gray = (r * 255 / 31 * 30 / 100) +
                     (g * 255 / 63 * 59 / 100) +
                     (b * 255 / 31 * 11 / 100);

      sum[region] += gray;
      samples[region]++;

      uint16_t next = (y * width + x + 2) * 2;
      uint16_t q = ((uint16_t)frame[next] << 8) | frame[next + 1];

      uint8_t qr = (q >> 11) & 0x1F;
      uint8_t qg = (q >> 5) & 0x3F;
      uint8_t qb = q & 0x1F;

      uint8_t nextGray = (qr * 255 / 31 * 30 / 100) +
                         (qg * 255 / 63 * 59 / 100) +
                         (qb * 255 / 31 * 11 / 100);

      if (abs((int)gray - (int)nextGray) > 35)
        edges[region]++;
    }
  }

  bool clear[3];

  for (uint8_t i = 0; i < 3; i++) {
    if (!samples[i]) {
      envState = result;
      return false;
    }

    uint32_t edgeRate = edges[i] * 100 / samples[i];

    // Indikator tekstur/tepi, bukan pengukur jarak.
    clear[i] = edgeRate < 45;
  }

  result.valid = true;
  result.leftClear = clear[0];
  result.centerClear = clear[1];
  result.rightClear = clear[2];

  result.obstacle = !clear[1];

  uint8_t clearCount = clear[0] + clear[1] + clear[2];
  result.confidence = clearCount * 100 / 3;

  envState = result;
  return true;
}

EnvState envGet() {
  return envState;
}
