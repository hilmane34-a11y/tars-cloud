
#include "env.h"

#define ENV_WIDTH 128
#define ENV_HEIGHT 64

#define GRID_W 16
#define GRID_H 8
#define GRID_SIZE (GRID_W * GRID_H)

#define SAMPLE_STEP 4
#define MOTION_THRESHOLD 25
#define MOTION_MIN_CELLS 3

static EnvState state = {};
static uint8_t previousGrid[GRID_SIZE];
static bool previousValid = false;

void envBegin() {
  state = {};
  memset(previousGrid, 0, sizeof(previousGrid));
  previousValid = false;
}

bool envAnalyze(const uint8_t* image, EnvState &result) {
  result = {};

  if (!image) {
    state = result;
    previousValid = false;
    return false;
  }

  uint8_t currentGrid[GRID_SIZE] = {};
  uint16_t bright[3] = {};
  uint16_t total[3] = {};

  // Ambil sampel kecil dari setiap area 8x8.
  for (int gy = 0; gy < GRID_H; gy++) {
    for (int gx = 0; gx < GRID_W; gx++) {
      uint16_t sum = 0;
      uint8_t count = 0;

      for (int y = 0; y < 8; y += SAMPLE_STEP) {
        for (int x = 0; x < 8; x += SAMPLE_STEP) {
          int px = gx * 8 + x;
          int py = gy * 8 + y;

          if (py < 12 || py >= 58) continue;

          // Asumsi: nilai bukan nol = piksel terang OLED.
          if (image[py * ENV_WIDTH + px])
            sum += 100;

          count++;
        }
      }

      uint8_t index = gy * GRID_W + gx;

      currentGrid[index] =
        count ? sum / count : 0;
    }
  }

  // Hitung kecerahan kiri, tengah, kanan.
  for (int gy = 0; gy < GRID_H; gy++) {
    for (int gx = 0; gx < GRID_W; gx++) {
      if (gy < 2 || gy > 6) continue;

      uint8_t region =
        gx < 5 ? 0 : (gx < 11 ? 1 : 2);

      bright[region] += currentGrid[gy * GRID_W + gx];
      total[region] += 100;
    }
  }

  uint8_t clear[3];
  uint8_t brightness[3];

  for (int i = 0; i < 3; i++) {
    if (!total[i]) {
      state = result;
      return false;
    }

    brightness[i] =
      (uint32_t)bright[i] * 100 / total[i];

    // Area terang bukan bukti bahwa ada manusia/hewan.
    clear[i] = brightness[i] >= 55;
  }

  result.valid = true;
  result.leftBright = brightness[0];
  result.centerBright = brightness[1];
  result.rightBright = brightness[2];

  result.leftClear = clear[0];
  result.centerClear = clear[1];
  result.rightClear = clear[2];

  // Indikator kepadatan visual gelap, bukan deteksi
  // rintangan fisik yang sudah terkonfirmasi.
  result.obstacle = !clear[1];

  uint8_t clearCount =
    clear[0] + clear[1] + clear[2];

  result.confidence = clearCount * 100 / 3;

  // Frame pertama hanya menjadi referensi.
  if (!previousValid) {
    memcpy(previousGrid, currentGrid, GRID_SIZE);
    previousValid = true;
    state = result;
    return true;
  }

  uint8_t changed[3] = {};
  uint8_t globalChanged = 0;
  uint8_t globalDirection = 0;

  for (int gy = 1; gy < 7; gy++) {
    for (int gx = 0; gx < GRID_W; gx++) {
      uint8_t index = gy * GRID_W + gx;

      int diff =
        (int)currentGrid[index] -
        (int)previousGrid[index];

      if (abs(diff) >= MOTION_THRESHOLD) {
        uint8_t region =
          gx < 5 ? 0 : (gx < 11 ? 1 : 2);

        changed[region]++;
        globalChanged++;

        if (diff > 0) globalDirection++;
      }
    }
  }

  uint8_t changedTotal =
    changed[0] + changed[1] + changed[2];

  // Perubahan global biasanya disebabkan perubahan cahaya.
  bool lightingChange = globalChanged >= 30;

  if (!lightingChange &&
      changedTotal >= MOTION_MIN_CELLS) {

    result.motion = true;
    result.motionLevel = changedTotal;

    if (changed[0] >= changed[1] &&
        changed[0] >= changed[2]) {
      result.event = ENV_MOTION_LEFT;
    }
    else if (changed[2] >= changed[0] &&
             changed[2] >= changed[1]) {
      result.event = ENV_MOTION_RIGHT;
    }
    else {
      result.event = ENV_MOTION_CENTER;
    }
  }
  else if (lightingChange) {
    result.event = ENV_SCENE_CHANGED;
  }

  memcpy(previousGrid, currentGrid, GRID_SIZE);
  state = result;

  return true;
}

EnvState envGet() {
  return state;
}
