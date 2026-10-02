#include "env.h"

#define ENV_WIDTH 128
#define ENV_HEIGHT 64

static EnvState state = {};

void envBegin() {
  state = {};
}

bool envAnalyze(const uint8_t* image, EnvState &result) {
  result = {};

  if (!image) {
    state = result;
    return false;
  }

  uint16_t dark[3] = {};
  uint16_t total[3] = {};

  // Analisis area tengah gambar, bukan langit/lantai ekstrem.
  for (int y = 12; y < 58; y += 2) {
    for (int x = 0; x < ENV_WIDTH; x += 2) {
      uint8_t region = x < 42 ? 0 : (x < 86 ? 1 : 2);

      total[region]++;
      if (image[y * ENV_WIDTH + x])
        dark[region]++;
    }
  }

  uint8_t clear[3];

  for (int i = 0; i < 3; i++) {
    if (!total[i]) {
      state = result;
      return false;
    }

    uint8_t darkRate = (uint32_t)dark[i] * 100 / total[i];

    // Indikator awal kepadatan area gelap.
    clear[i] = darkRate < 45;
  }

  result.valid = true;
  result.leftClear = clear[0];
  result.centerClear = clear[1];
  result.rightClear = clear[2];
  result.obstacle = !clear[1];

  uint8_t clearCount = clear[0] + clear[1] + clear[2];
  result.confidence = clearCount * 100 / 3;

  state = result;
  return true;
}

EnvState envGet() {
  return state;
}
