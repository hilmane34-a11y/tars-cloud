
#include "env.h"
#include <Arduino.h>
#include <string.h>
#include <stdlib.h>

#define ENV_WIDTH 96
#define ENV_HEIGHT 32
#define GRID_W 16
#define GRID_H 8

#define MOTION_THRESHOLD 30
#define MIN_MOTION_CELLS 10
#define GLOBAL_CHANGE_CELLS 50

#define CLEAR_CONFIRM_FRAMES 3
#define CLEAR_LOST_FRAMES 2

static EnvState state = {};
static uint8_t previousGrid[GRID_W * GRID_H] = {};
static bool previousValid = false;
static uint8_t clearStable[3] = {};
static uint8_t blockedStable[3] = {};

struct ColorInfo {
  uint32_t count[11];
  uint32_t samples;
  uint32_t brightness;
};

struct VisualInfo {
  uint32_t brightness;
  uint32_t contrast;
  uint16_t samples;
  uint16_t edges;
  uint16_t texture;
  uint16_t colorChanges;
};

static uint8_t getBrightness(uint16_t p) {
  uint8_t r = ((p >> 11) & 31) * 255 / 31;
  uint8_t g = ((p >> 5) & 63) * 255 / 63;
  uint8_t b = (p & 31) * 255 / 31;
  return (77 * r + 150 * g + 29 * b) >> 8;
}

static EnvColor getColor(uint16_t p) {
  uint8_t r = ((p >> 11) & 31) * 255 / 31;
  uint8_t g = ((p >> 5) & 63) * 255 / 63;
  uint8_t b = (p & 31) * 255 / 31;

  uint8_t mx = max(r, max(g, b));
  uint8_t mn = min(r, min(g, b));
  uint8_t d = mx - mn;

  if (mx < 35) return ENV_COLOR_BLACK;
  if (mn > 220 && d < 35) return ENV_COLOR_WHITE;
  if (d < 25) return ENV_COLOR_GRAY;

  float h;
  if (mx == r) h = 60.0f * ((float)g - b) / d;
  else if (mx == g) h = 60.0f * ((float)b - r) / d + 120;
  else h = 60.0f * ((float)r - g) / d + 240;

  if (h < 0) h += 360;

  if (h < 15 || h >= 345) return ENV_COLOR_RED;
  if (h < 40) return ENV_COLOR_ORANGE;
  if (h < 70) return ENV_COLOR_YELLOW;
  if (h < 165) return ENV_COLOR_GREEN;
  if (h < 200) return ENV_COLOR_CYAN;
  if (h < 255) return ENV_COLOR_BLUE;
  return ENV_COLOR_PURPLE;
}

static void addPixel(ColorInfo &info, uint16_t p) {
  EnvColor c = getColor(p);
  info.count[c]++;
  info.samples++;
  info.brightness += getBrightness(p);
}

static EnvColor bestColor(const ColorInfo &info, uint8_t &confidence) {
  confidence = 0;
  if (!info.samples) return ENV_COLOR_UNKNOWN;

  uint8_t best = ENV_COLOR_UNKNOWN;
  uint32_t count = 0;

  for (uint8_t i = 1; i <= ENV_COLOR_PURPLE; i++) {
    if (info.count[i] > count) {
      count = info.count[i];
      best = i;
    }
  }

  confidence = (uint32_t)count * 100 / info.samples;
  return (EnvColor)best;
}

static void addVisual(
  VisualInfo &v,
  uint16_t p,
  uint16_t right,
  uint16_t down
) {
  int b = getBrightness(p);
  int br = getBrightness(right);
  int bd = getBrightness(down);
  int diff = max(abs(b - br), abs(b - bd));

  v.brightness += b;
  v.contrast += diff;
  v.samples++;

  if (diff >= 22) v.edges++;
  if (diff >= 10) v.texture++;
  if (getColor(p) != getColor(right)) v.colorChanges++;
}

static uint8_t obstacleScore(const VisualInfo &v) {
  if (!v.samples) return 0;

  uint32_t edgeRate = (uint32_t)v.edges * 100 / v.samples;
  uint32_t textureRate = (uint32_t)v.texture * 100 / v.samples;
  uint32_t contrast = v.contrast / v.samples;

  uint32_t score =
    edgeRate * 2 +
    textureRate / 3 +
    contrast / 2;

  return score > 100 ? 100 : score;
}

void envBegin() {
  state = {};
  memset(previousGrid, 0, sizeof(previousGrid));
  previousValid = false;
  memset(clearStable, 0, sizeof(clearStable));
  memset(blockedStable, 0, sizeof(blockedStable));
}

void envResetMotion() {
  memset(previousGrid, 0, sizeof(previousGrid));
  previousValid = false;
}

bool envAnalyze(const uint16_t *image, EnvState &result) {
  return envAnalyze(image, ENV_COLOR_UNKNOWN, 0, result);
}

bool envAnalyze(
  const uint16_t *image,
  uint8_t dominantColor,
  uint8_t colorConfidence,
  EnvState &result
) {
  result = {};

  if (!image) {
    state = result;
    previousValid = false;
    memset(clearStable, 0, sizeof(clearStable));
    memset(blockedStable, 0, sizeof(blockedStable));
    return false;
  }

  ColorInfo sector[3] = {};
  ColorInfo global = {};
  VisualInfo zone[3][3] = {};

  uint8_t currentGrid[GRID_W * GRID_H] = {};
  uint8_t changedRegion[3] = {};
  uint8_t changedCells = 0;

  // Bagi gambar menjadi 3 sektor horizontal dan 3 zona vertikal.
  // Zona 0=atas, 1=tengah, 2=bawah.
  for (int y = 0; y < ENV_HEIGHT; y += 2) {
    int band = y < 10 ? 0 : (y < 21 ? 1 : 2);

    for (int x = 0; x < ENV_WIDTH; x += 2) {
      int s = x < 32 ? 0 : (x < 64 ? 1 : 2);
      uint16_t p = image[y * ENV_WIDTH + x];

      addPixel(sector[s], p);
      addPixel(global, p);

      int xr = min(x + 2, ENV_WIDTH - 1);
      int yd = min(y + 2, ENV_HEIGHT - 1);

      addVisual(
        zone[s][band],
        p,
        image[y * ENV_WIDTH + xr],
        image[yd * ENV_WIDTH + x]
      );
    }
  }

  for (int s = 0; s < 3; s++) {
    if (!sector[s].samples) {
      state = result;
      previousValid = false;
      return false;
    }
  }

  result.leftBright =
    sector[0].brightness / sector[0].samples;
  result.centerBright =
    sector[1].brightness / sector[1].samples;
  result.rightBright =
    sector[2].brightness / sector[2].samples;

  result.leftColor =
    bestColor(sector[0], result.leftColorConfidence);
  result.centerColor =
    bestColor(sector[1], result.centerColorConfidence);
  result.rightColor =
    bestColor(sector[2], result.rightColorConfidence);

  uint8_t globalBrightness =
    global.brightness / max((uint32_t)1, global.samples);

  // Analisis ciri setiap zona.
  uint8_t brightness[3][3] = {};
  uint8_t scores[3][3] = {};

  for (int s = 0; s < 3; s++) {
    for (int b = 0; b < 3; b++) {
      VisualInfo &v = zone[s][b];
      if (!v.samples) continue;

      brightness[s][b] = v.brightness / v.samples;
      scores[s][b] = obstacleScore(v);
    }
  }

  for (int s = 0; s < 3; s++) {
    for (int b = 0; b < 3; b++) {
      if (!zone[s][b].samples) continue;

      uint8_t bright = brightness[s][b];
      uint8_t score = scores[s][b];

      // Bandingkan kecerahan dengan zona sekitar.
      uint16_t neighborSum = 0;
      uint8_t neighborCount = 0;

      if (s > 0) {
        neighborSum += brightness[s - 1][b];
        neighborCount++;
      }
      if (s < 2) {
        neighborSum += brightness[s + 1][b];
        neighborCount++;
      }
      if (b > 0) {
        neighborSum += brightness[s][b - 1];
        neighborCount++;
      }
      if (b < 2) {
        neighborSum += brightness[s][b + 1];
        neighborCount++;
      }

      uint8_t neighborBright = neighborCount
        ? neighborSum / neighborCount
        : globalBrightness;

      bool darkerThanNeighbors =
        (uint16_t)bright + 18 < neighborBright;

      bool darkerThanScene =
        (uint16_t)bright + 25 < globalBrightness;

      // Bayangan hanya dugaan visual:
      // lebih gelap daripada sekitar dan memiliki
      // ciri tepi relatif rendah.
      bool likelyShadow =
        (darkerThanNeighbors || darkerThanScene) &&
        score < 48;

      EnvSurface surface = ENV_SURFACE_UNKNOWN;

      if (likelyShadow) {
        surface = ENV_SURFACE_SHADOW;
        result.shadow[s] = true;
      } else if (b == 2 && score < 68) {
        // Bagian bawah gambar diperkirakan sebagai lantai.
        surface = ENV_SURFACE_FLOOR;
      } else if (b == 0 && score < 55) {
        // Bagian atas yang relatif seragam bisa berupa dinding.
        surface = ENV_SURFACE_WALL;
      } else if (score >= 68) {
        // Tepi/tekstur tinggi bisa berasal dari objek,
        // tetapi juga bisa dari pola lantai atau permukaan lain.
        surface = ENV_SURFACE_OBSTACLE;
      }

      result.surface[s][b] = surface;
    }
  }

  // Estimasi penghalang per sektor.
  for (int s = 0; s < 3; s++) {
    uint16_t combined =
      (scores[s][1] * 2 + scores[s][2]) / 3;

    if (result.surface[s][1] == ENV_SURFACE_OBSTACLE) {
      combined = max(combined, (uint16_t)65);
    }

    if (result.surface[s][2] == ENV_SURFACE_OBSTACLE) {
      combined = max(combined, (uint16_t)75);
    }

    // Bayangan saja tidak boleh otomatis menjadi penghalang.
    if (result.shadow[s] &&
        result.surface[s][1] != ENV_SURFACE_OBSTACLE &&
        result.surface[s][2] != ENV_SURFACE_OBSTACLE) {
      combined = min(combined, (uint16_t)40);
    }

    result.obstacleConfidence[s] =
      min((uint16_t)100, combined);

    uint8_t bright =
      s == 0 ? result.leftBright :
      s == 1 ? result.centerBright :
               result.rightBright;

    bool rawClear =
      bright > 20 &&
      result.obstacleConfidence[s] < 55;

    if (rawClear) {
      if (clearStable[s] < CLEAR_CONFIRM_FRAMES) {
        clearStable[s]++;
      }
      blockedStable[s] = 0;
    } else {
      if (blockedStable[s] < CLEAR_LOST_FRAMES) {
        blockedStable[s]++;
      }
      clearStable[s] = 0;
    }

    bool isClear =
      clearStable[s] >= CLEAR_CONFIRM_FRAMES;

    if (blockedStable[s] >= CLEAR_LOST_FRAMES) {
      isClear = false;
    }

    if (s == 0) result.leftClear = isClear;
    if (s == 1) result.centerClear = isClear;
    if (s == 2) result.rightClear = isClear;
  }

  // Deteksi perubahan gambar antar-frame.
  for (int gy = 0; gy < GRID_H; gy++) {
    for (int gx = 0; gx < GRID_W; gx++) {
      int x0 = gx * ENV_WIDTH / GRID_W;
      int x1 = (gx + 1) * ENV_WIDTH / GRID_W;
      int y0 = gy * ENV_HEIGHT / GRID_H;
      int y1 = (gy + 1) * ENV_HEIGHT / GRID_H;

      uint32_t sum = 0;
      uint16_t samples = 0;

      for (int y = y0; y < y1; y += 2) {
        for (int x = x0; x < x1; x += 2) {
          sum += getBrightness(image[y * ENV_WIDTH + x]);
          samples++;
        }
      }

      currentGrid[gy * GRID_W + gx] =
        samples ? sum / samples : 0;
    }
  }

  if (previousValid) {
    for (int gy = 0; gy < GRID_H; gy++) {
      for (int gx = 0; gx < GRID_W; gx++) {
        int index = gy * GRID_W + gx;

        int diff = abs(
          (int)currentGrid[index] -
          (int)previousGrid[index]
        );

        if (diff >= MOTION_THRESHOLD) {
          if (changedCells < 255) changedCells++;

          int cx =
            gx * ENV_WIDTH / GRID_W +
            ENV_WIDTH / GRID_W / 2;

          int s = cx < 32 ? 0 : (cx < 64 ? 1 : 2);

          if (changedRegion[s] < 255) {
            changedRegion[s]++;
          }
        }
      }
    }
  }

  memcpy(previousGrid, currentGrid, sizeof(previousGrid));
  previousValid = true;

  result.valid = true;
  result.obstacle = !result.centerClear;

  uint8_t clearCount =
    result.leftClear +
    result.centerClear +
    result.rightClear;

  result.confidence = clearCount * 100 / 3;
  result.motion = changedCells >= MIN_MOTION_CELLS;
  result.motionLevel = changedCells;

  if (changedCells >= GLOBAL_CHANGE_CELLS) {
    result.event = ENV_SCENE_CHANGED;
  } else if (result.motion) {
    if (changedRegion[0] >= changedRegion[1] &&
        changedRegion[0] >= changedRegion[2]) {
      result.event = ENV_MOTION_LEFT;
    } else if (
      changedRegion[1] >= changedRegion[0] &&
      changedRegion[1] >= changedRegion[2]) {
      result.event = ENV_MOTION_CENTER;
    } else {
      result.event = ENV_MOTION_RIGHT;
    }
  } else {
    result.event = ENV_NONE;
  }

  result.dominantColor =
    bestColor(global, result.colorConfidence);

  if (dominantColor > ENV_COLOR_UNKNOWN &&
      dominantColor <= ENV_COLOR_PURPLE) {
    result.dominantColor = (EnvColor)dominantColor;
    result.colorConfidence = colorConfidence;
  }

  state = result;
  return true;
}

EnvState envGet() {
  return state;
}
