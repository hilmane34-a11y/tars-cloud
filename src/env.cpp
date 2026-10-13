#include "env.h"
#include <string.h>
#include <stdlib.h>

#define ENV_WIDTH  96
#define ENV_HEIGHT 32

#define GRID_W 16
#define GRID_H 8
#define GRID_CELLS (GRID_W * GRID_H)

#define MOTION_THRESHOLD       30
#define MIN_MOTION_CELLS       10
#define GLOBAL_CHANGE_CELLS    50

#define LIGHT_DIFF_THRESHOLD   12
#define LIGHT_MIN_CELLS        64
#define LIGHT_DIRECTION_PERCENT 78
#define LIGHT_MEAN_DELTA       12

#define OBJECT_SCORE_THRESHOLD 45
#define WALL_SCORE_THRESHOLD   45

#define SHADOW_DARK_THRESHOLD  15
#define CORNER_EDGE_THRESHOLD  12
#define CORNER_SIDE_THRESHOLD  12

static EnvState currentState;

static uint8_t previousGrid[GRID_CELLS];
static bool motionInitialized = false;

struct ZoneStats {
  uint32_t brightnessSum;
  uint32_t contrastSum;
  uint32_t edgeVSum;
  uint32_t edgeHSum;
  uint16_t pixels;
};

struct GridStats {
  uint32_t brightnessSum;
  uint32_t textureSum;
  uint16_t pixels;
};

static uint8_t clampByte(int value)
{
  if (value < 0) return 0;
  if (value > 255) return 255;
  return (uint8_t)value;
}

static uint8_t pixelBrightness(uint16_t pixel)
{
  uint8_t r = (pixel >> 11) & 0x1F;
  uint8_t g = (pixel >> 5) & 0x3F;
  uint8_t b = pixel & 0x1F;

  r = (r * 255) / 31;
  g = (g * 255) / 63;
  b = (b * 255) / 31;

  return (uint8_t)(
    (77UL * r + 150UL * g + 29UL * b) >> 8
  );
}

static void pixelRGB(
  uint16_t pixel,
  uint8_t &r,
  uint8_t &g,
  uint8_t &b
)
{
  uint8_t r5 = (pixel >> 11) & 0x1F;
  uint8_t g6 = (pixel >> 5) & 0x3F;
  uint8_t b5 = pixel & 0x1F;

  r = (r5 * 255) / 31;
  g = (g6 * 255) / 63;
  b = (b5 * 255) / 31;
}

static EnvColor classifyColor(
  uint32_t rSum,
  uint32_t gSum,
  uint32_t bSum,
  uint16_t count,
  uint8_t &confidence
)
{
  confidence = 0;

  if (!count) {
    return ENV_COLOR_UNKNOWN;
  }

  int r = rSum / count;
  int g = gSum / count;
  int b = bSum / count;

  int maxC = r;
  if (g > maxC) maxC = g;
  if (b > maxC) maxC = b;

  int minC = r;
  if (g < minC) minC = g;
  if (b < minC) minC = b;

  int saturation = maxC - minC;
  int brightness = (77 * r + 150 * g + 29 * b) >> 8;

  confidence = clampByte(saturation * 2);

  if (brightness < 28) {
    confidence = clampByte(180 - brightness);
    return ENV_COLOR_BLACK;
  }

  if (brightness > 225 && saturation < 28) {
    confidence = clampByte(brightness - 60);
    return ENV_COLOR_WHITE;
  }

  if (saturation < 22) {
    confidence = clampByte(150 - saturation);
    return ENV_COLOR_GRAY;
  }

  if (r > g * 1.35 && r > b * 1.35) {
    if (g > b * 1.20) {
      return ENV_COLOR_ORANGE;
    }

    return ENV_COLOR_RED;
  }

  if (r > 150 && g > 125 && b < 100) {
    return ENV_COLOR_YELLOW;
  }

  if (g > r * 1.20 && g > b * 1.10) {
    return ENV_COLOR_GREEN;
  }

  if (g > 100 && b > 100 && abs(g - b) < 55) {
    return ENV_COLOR_CYAN;
  }

  if (b > r * 1.15 && b > g * 1.05) {
    return ENV_COLOR_BLUE;
  }

  if (r > g * 1.10 && b > g * 1.10) {
    return ENV_COLOR_PURPLE;
  }

  return ENV_COLOR_UNKNOWN;
}

static void resetState()
{
  memset(&currentState, 0, sizeof(currentState));

  currentState.event = ENV_NONE;
  currentState.dominantColor = ENV_COLOR_UNKNOWN;

  currentState.leftColor = ENV_COLOR_UNKNOWN;
  currentState.centerColor = ENV_COLOR_UNKNOWN;
  currentState.rightColor = ENV_COLOR_UNKNOWN;

  for (int s = 0; s < 3; s++) {
    currentState.leftClear = true;
    currentState.centerClear = true;
    currentState.rightClear = true;

    currentState.shadowKind[s] = ENV_SHADOW_UNKNOWN;

    for (int z = 0; z < 3; z++) {
      currentState.surface[s][z] = ENV_SURFACE_UNKNOWN;
    }
  }
}

void envBegin()
{
  resetState();

  memset(previousGrid, 0, sizeof(previousGrid));
  motionInitialized = false;
}

void envResetMotion()
{
  memset(previousGrid, 0, sizeof(previousGrid));
  motionInitialized = false;

  currentState.motion = false;
  currentState.motionLevel = 0;
  currentState.event = ENV_NONE;
  currentState.lightingChanged = false;
}

EnvState envGet()
{
  return currentState;
}

bool envAnalyze(const uint16_t *image, EnvState &result)
{
  return envAnalyze(
    image,
    ENV_COLOR_UNKNOWN,
    0,
    result
  );
}

bool envAnalyze(
  const uint16_t *image,
  uint8_t dominantColor,
  uint8_t colorConfidence,
  EnvState &result
)
{
  if (!image) {
    result = currentState;
    return false;
  }

  ZoneStats zones[3][3];
  GridStats grid[GRID_CELLS];

  memset(zones, 0, sizeof(zones));
  memset(grid, 0, sizeof(grid));

  uint32_t sectorBrightness[3] = {0, 0, 0};
  uint32_t sectorR[3] = {0, 0, 0};
  uint32_t sectorG[3] = {0, 0, 0};
  uint32_t sectorB[3] = {0, 0, 0};

  uint16_t sectorPixels[3] = {0, 0, 0};

  uint32_t allR = 0;
  uint32_t allG = 0;
  uint32_t allB = 0;

  uint32_t allBrightness = 0;
  uint16_t allPixels = 0;

  // ------------------------------------------------------
  // 1. Analisis piksel: kecerahan, kontras, dan tepi.
  // ------------------------------------------------------

  for (int y = 0; y < ENV_HEIGHT; y++) {
    for (int x = 0; x < ENV_WIDTH; x++) {
      int index = y * ENV_WIDTH + x;

      uint16_t pixel = image[index];

      uint8_t brightness = pixelBrightness(pixel);

      uint8_t r, g, b;
      pixelRGB(pixel, r, g, b);

      int sector = (x * 3) / ENV_WIDTH;
      int band = (y * 3) / ENV_HEIGHT;

      if (sector > 2) sector = 2;
      if (band > 2) band = 2;

      ZoneStats &zone = zones[sector][band];

      zone.brightnessSum += brightness;
      zone.pixels++;

      sectorBrightness[sector] += brightness;
      sectorR[sector] += r;
      sectorG[sector] += g;
      sectorB[sector] += b;
      sectorPixels[sector]++;

      allR += r;
      allG += g;
      allB += b;

      allBrightness += brightness;
      allPixels++;

      // Perbedaan horizontal: indikasi garis vertikal.
      if (x > 0) {
        uint8_t leftBrightness =
          pixelBrightness(image[index - 1]);

        uint8_t diff = abs(
          (int)brightness - (int)leftBrightness
        );

        zone.edgeVSum += diff;
        zone.contrastSum += diff;
      }

      // Perbedaan vertikal: indikasi garis horizontal.
      if (y > 0) {
        uint8_t upperBrightness =
          pixelBrightness(image[index - ENV_WIDTH]);

        uint8_t diff = abs(
          (int)brightness - (int)upperBrightness
        );

        zone.edgeHSum += diff;
        zone.contrastSum += diff;
      }

      // Sel grid 6x4 piksel.
      int gx = x / (ENV_WIDTH / GRID_W);
      int gy = y / (ENV_HEIGHT / GRID_H);

      if (gx >= GRID_W) gx = GRID_W - 1;
      if (gy >= GRID_H) gy = GRID_H - 1;

      int cell = gy * GRID_W + gx;

      grid[cell].brightnessSum += brightness;
      grid[cell].pixels++;

      if (x > 0) {
        uint8_t leftBrightness =
          pixelBrightness(image[index - 1]);

        grid[cell].textureSum += abs(
          (int)brightness - (int)leftBrightness
        );
      }

      if (y > 0) {
        uint8_t upperBrightness =
          pixelBrightness(image[index - ENV_WIDTH]);

        grid[cell].textureSum += abs(
          (int)brightness - (int)upperBrightness
        );
      }
    }
  }

  uint8_t cellBrightness[GRID_CELLS];
  uint8_t cellTexture[GRID_CELLS];

  for (int i = 0; i < GRID_CELLS; i++) {
    if (grid[i].pixels) {
      cellBrightness[i] = clampByte(
        grid[i].brightnessSum / grid[i].pixels
      );

      cellTexture[i] = clampByte(
        grid[i].textureSum / grid[i].pixels
      );
    } else {
      cellBrightness[i] = 0;
      cellTexture[i] = 0;
    }
  }

  uint8_t zoneBrightness[3][3];
  uint8_t zoneContrast[3][3];
  uint8_t zoneEdgeV[3][3];
  uint8_t zoneEdgeH[3][3];

  for (int s = 0; s < 3; s++) {
    for (int z = 0; z < 3; z++) {
      ZoneStats &zone = zones[s][z];

      if (!zone.pixels) {
        zoneBrightness[s][z] = 0;
        zoneContrast[s][z] = 0;
        zoneEdgeV[s][z] = 0;
        zoneEdgeH[s][z] = 0;
        continue;
      }

      zoneBrightness[s][z] = clampByte(
        zone.brightnessSum / zone.pixels
      );

      zoneContrast[s][z] = clampByte(
        zone.contrastSum / zone.pixels
      );

      zoneEdgeV[s][z] = clampByte(
        zone.edgeVSum / zone.pixels
      );

      zoneEdgeH[s][z] = clampByte(
        zone.edgeHSum / zone.pixels
      );
    }
  }

  // ------------------------------------------------------
  // 2. Perubahan global: gerakan atau perubahan cahaya.
  // ------------------------------------------------------

  int changedCells = 0;
  int positiveChanges = 0;
  int negativeChanges = 0;
  int changedXSum = 0;

  int brightnessDeltaSum = 0;

  if (motionInitialized) {
    for (int i = 0; i < GRID_CELLS; i++) {
      int delta =
        (int)cellBrightness[i] - (int)previousGrid[i];

      brightnessDeltaSum += delta;

      if (abs(delta) >= MOTION_THRESHOLD) {
        changedCells++;
        changedXSum += i % GRID_W;

        if (delta > 0) {
          positiveChanges++;
        } else {
          negativeChanges++;
        }
      }
    }
  }

  for (int i = 0; i < GRID_CELLS; i++) {
    previousGrid[i] = cellBrightness[i];
  }

  bool globalLightingChange = false;

  if (motionInitialized && changedCells >= LIGHT_MIN_CELLS) {
    int sameDirection = positiveChanges;

    if (negativeChanges > sameDirection) {
      sameDirection = negativeChanges;
    }

    int directionPercent =
      (sameDirection * 100) / changedCells;

    int meanDelta = brightnessDeltaSum / GRID_CELLS;

    if (
      directionPercent >= LIGHT_DIRECTION_PERCENT &&
      abs(meanDelta) >= LIGHT_MEAN_DELTA
    ) {
      globalLightingChange = true;
    }
  }

  currentState.motion = false;
  currentState.motionLevel = 0;
  currentState.event = ENV_NONE;
  currentState.lightingChanged = false;

  if (motionInitialized) {
    if (globalLightingChange) {
      currentState.lightingChanged = true;
      currentState.event = ENV_LIGHTING_CHANGED;

      // Perubahan cahaya global tidak dianggap sebagai
      // gerakan objek atau perubahan adegan.
      currentState.motion = false;
      currentState.motionLevel = 0;
    } else if (changedCells >= GLOBAL_CHANGE_CELLS) {
      currentState.motion = true;
      currentState.event = ENV_SCENE_CHANGED;

      currentState.motionLevel = clampByte(
        (changedCells * 255) / GRID_CELLS
      );
    } else if (changedCells >= MIN_MOTION_CELLS) {
      currentState.motion = true;

      currentState.motionLevel = clampByte(
        (changedCells * 255) / GRID_CELLS
      );

      int averageX = changedXSum / changedCells;

      if (averageX < GRID_W / 3) {
        currentState.event = ENV_MOTION_LEFT;
      } else if (averageX < (GRID_W * 2) / 3) {
        currentState.event = ENV_MOTION_CENTER;
      } else {
        currentState.event = ENV_MOTION_RIGHT;
      }
    }
  }

  motionInitialized = true;

  // ------------------------------------------------------
  // 3. Warna dominan dan warna setiap sektor.
  // ------------------------------------------------------

  uint8_t calculatedColorConfidence = 0;

  EnvColor calculatedDominantColor = classifyColor(
    allR,
    allG,
    allB,
    allPixels,
    calculatedColorConfidence
  );

  if (
    dominantColor > ENV_COLOR_UNKNOWN &&
    dominantColor <= ENV_COLOR_PURPLE
  ) {
    currentState.dominantColor = (EnvColor)dominantColor;
    currentState.colorConfidence = colorConfidence;
  } else {
    currentState.dominantColor = calculatedDominantColor;
    currentState.colorConfidence = calculatedColorConfidence;
  }

  EnvColor sectorColors[3];
  uint8_t sectorColorConfidence[3];

  for (int s = 0; s < 3; s++) {
    sectorColors[s] = classifyColor(
      sectorR[s],
      sectorG[s],
      sectorB[s],
      sectorPixels[s],
      sectorColorConfidence[s]
    );
  }

  currentState.leftColor = sectorColors[0];
  currentState.centerColor = sectorColors[1];
  currentState.rightColor = sectorColors[2];

  currentState.leftColorConfidence = sectorColorConfidence[0];
  currentState.centerColorConfidence = sectorColorConfidence[1];
  currentState.rightColorConfidence = sectorColorConfidence[2];

  currentState.leftBright = clampByte(
    sectorBrightness[0] / (sectorPixels[0] ? sectorPixels[0] : 1)
  );

  currentState.centerBright = clampByte(
    sectorBrightness[1] / (sectorPixels[1] ? sectorPixels[1] : 1)
  );

  currentState.rightBright = clampByte(
    sectorBrightness[2] / (sectorPixels[2] ? sectorPixels[2] : 1)
  );

  // ------------------------------------------------------
  // 4. Dinding, lantai, dan kandidat objek.
  // ------------------------------------------------------

  bool anyObject = false;
  int detectedWalls = 0;

  for (int s = 0; s < 3; s++) {
    int wallScore = 0;
    int bestObjectScore = 0;

    for (int z = 0; z < 3; z++) {
      int brightness = zoneBrightness[s][z];
      int contrast = zoneContrast[s][z];
      int edgeV = zoneEdgeV[s][z];
      int edgeH = zoneEdgeH[s][z];

      int averageEdge = (edgeV + edgeH) / 2;

      // Skor permukaan yang relatif seragam.
      int uniformityScore =
        100 - (contrast * 2) - (averageEdge * 2);

      if (uniformityScore < 0) uniformityScore = 0;
      if (uniformityScore > 100) uniformityScore = 100;

      if (
        z < 2 &&
        brightness >= 12 &&
        uniformityScore > wallScore
      ) {
        wallScore = uniformityScore;
      }

      // Permukaan datar di zona bawah bisa menyerupai lantai.
      if (z == 2) {
        if (
          brightness >= 12 &&
          contrast < 24 &&
          averageEdge < 20
        ) {
          currentState.surface[s][z] = ENV_SURFACE_FLOOR;
        }
      }

      // Kandidat objek: kontras atau tepi yang kuat.
      // Ini tidak mengidentifikasi jenis objek.
      if (z >= 1) {
        int objectScore =
          (averageEdge * 2) + (contrast / 2);

        if (brightness < 8) {
          objectScore -= 8;
        }

        if (objectScore < 0) objectScore = 0;
        if (objectScore > 100) objectScore = 100;

        if (objectScore > bestObjectScore) {
          bestObjectScore = objectScore;
        }

        if (
          objectScore >= OBJECT_SCORE_THRESHOLD &&
          contrast >= 8
        ) {
          currentState.surface[s][z] = ENV_SURFACE_OBSTACLE;
        } else if (
          z < 2 &&
          uniformityScore >= WALL_SCORE_THRESHOLD
        ) {
          currentState.surface[s][z] = ENV_SURFACE_WALL;
        }
      } else {
        if (
          brightness >= 12 &&
          uniformityScore >= WALL_SCORE_THRESHOLD
        ) {
          currentState.surface[s][z] = ENV_SURFACE_WALL;
        }
      }
    }

    currentState.wallConfidence[s] = clampByte(wallScore);
    currentState.wallDetected[s] =
      wallScore >= WALL_SCORE_THRESHOLD;

    if (currentState.wallDetected[s]) {
      detectedWalls++;
    }

    currentState.objectConfidence[s] =
      clampByte(bestObjectScore);

    currentState.obstacleConfidence[s] =
      currentState.objectConfidence[s];

    currentState.objectDetected[s] =
      bestObjectScore >= OBJECT_SCORE_THRESHOLD;

    if (currentState.objectDetected[s]) {
      anyObject = true;
    }
  }

  currentState.obstacle = anyObject;

  // ------------------------------------------------------
  // 5. Deteksi garis sudut atau pertemuan bidang dinding.
  // ------------------------------------------------------

  currentState.cornerDetected = false;
  currentState.cornerConfidence = 0;

  for (int s = 0; s < 3; s++) {
    currentState.wallCorner[s] = false;
  }

  int bestCornerX = -1;
  int bestCornerScore = 0;

  for (int x = 3; x < ENV_WIDTH - 4; x++) {
    int verticalEdgeSum = 0;
    int persistentRows = 0;

    int leftSum = 0;
    int rightSum = 0;
    int sideCount = 0;

    for (int y = 0; y < ENV_HEIGHT; y++) {
      int index = y * ENV_WIDTH + x;

      int a = pixelBrightness(image[index - 1]);
      int b = pixelBrightness(image[index]);

      int difference = abs(a - b);

      verticalEdgeSum += difference;

      if (difference >= CORNER_EDGE_THRESHOLD) {
        persistentRows++;
      }

      // Perbandingan sisi kiri dan kanan garis kandidat.
      if (x >= 4 && x + 3 < ENV_WIDTH) {
        leftSum += pixelBrightness(image[index - 4]);
        leftSum += pixelBrightness(image[index - 3]);

        rightSum += pixelBrightness(image[index + 1]);
        rightSum += pixelBrightness(image[index + 2]);

        sideCount += 2;
      }
    }

    int meanVerticalEdge = verticalEdgeSum / ENV_HEIGHT;

    int leftMean = sideCount ? leftSum / sideCount : 0;
    int rightMean = sideCount ? rightSum / sideCount : 0;

    int sideDifference = abs(leftMean - rightMean);

    int persistencePercent =
      (persistentRows * 100) / ENV_HEIGHT;

    if (
      meanVerticalEdge >= CORNER_EDGE_THRESHOLD &&
      sideDifference >= CORNER_SIDE_THRESHOLD &&
      persistencePercent >= 40
    ) {
      int score =
        meanVerticalEdge +
        sideDifference +
        persistencePercent / 2;

      if (score > bestCornerScore) {
        bestCornerScore = score;
        bestCornerX = x;
      }
    }
  }

  if (bestCornerX >= 0) {
    int sector = (bestCornerX * 3) / ENV_WIDTH;

    if (sector > 2) sector = 2;

    currentState.wallCorner[sector] = true;
    currentState.cornerDetected = true;

    currentState.cornerConfidence = clampByte(
      bestCornerScore
    );

    // Garis dekat batas dua sektor bisa memengaruhi
    // kedua sektor yang bersebelahan.
    int localX = bestCornerX % (ENV_WIDTH / 3);

    if (localX <= 3 && sector > 0) {
      currentState.wallCorner[sector - 1] = true;
    }

    if (localX >= (ENV_WIDTH / 3) - 4 && sector < 2) {
      currentState.wallCorner[sector + 1] = true;
    }
  }

  // ------------------------------------------------------
  // 6. Kandidat bayangan.
  // ------------------------------------------------------

  for (int s = 0; s < 3; s++) {
    currentState.shadow[s] = false;
    currentState.shadowConfidence[s] = 0;
    currentState.shadowKind[s] = ENV_SHADOW_UNKNOWN;

    int bestShadowScore = 0;
    int bestShadowBand = -1;

    for (int z = 0; z < 3; z++) {
      int brightness = zoneBrightness[s][z];
      int contrast = zoneContrast[s][z];
      int edge = (
        zoneEdgeV[s][z] + zoneEdgeH[s][z]
      ) / 2;

      int neighborSum = 0;
      int neighborCount = 0;

      // Band di atas dan di bawah.
      if (z > 0) {
        neighborSum += zoneBrightness[s][z - 1];
        neighborCount++;
      }

      if (z < 2) {
        neighborSum += zoneBrightness[s][z + 1];
        neighborCount++;
      }

      // Sektor di kiri dan kanan.
      if (s > 0) {
        neighborSum += zoneBrightness[s - 1][z];
        neighborCount++;
      }

      if (s < 2) {
        neighborSum += zoneBrightness[s + 1][z];
        neighborCount++;
      }

      if (!neighborCount) continue;

      int neighborMean = neighborSum / neighborCount;
      int darknessDifference = neighborMean - brightness;

      // Bayangan mungkin berupa area gelap dengan
      // perbedaan yang jelas dari area sekitarnya.
      int shadowScore = 0;

      if (
        darknessDifference >= SHADOW_DARK_THRESHOLD &&
        contrast <= 40
      ) {
        shadowScore =
          darknessDifference +
          (edge / 2);

        if (shadowScore > 100) shadowScore = 100;
      }

      if (shadowScore > bestShadowScore) {
        bestShadowScore = shadowScore;
        bestShadowBand = z;
      }
    }

    if (bestShadowScore >= 25 && bestShadowBand >= 0) {
      currentState.shadow[s] = true;
      currentState.shadowConfidence[s] =
        clampByte(bestShadowScore);

      currentState.surface[s][bestShadowBand] =
        ENV_SURFACE_SHADOW;

      // Klasifikasi ini hanya perkiraan berdasarkan konteks.
      if (currentState.objectDetected[s]) {
        currentState.shadowKind[s] = ENV_SHADOW_OBJECT;
      } else if (currentState.wallDetected[s]) {
        currentState.shadowKind[s] = ENV_SHADOW_WALL;
      } else {
        currentState.shadowKind[s] = ENV_SHADOW_UNKNOWN;
      }
    }
  }

  // ------------------------------------------------------
  // 7. Perkiraan apakah sektor relatif bebas penghalang.
  // ------------------------------------------------------

  currentState.leftClear =
    !currentState.objectDetected[0] &&
    currentState.leftBright > 12;

  currentState.centerClear =
    !currentState.objectDetected[1] &&
    currentState.centerBright > 12;

  currentState.rightClear =
    !currentState.objectDetected[2] &&
    currentState.rightBright > 12;

  // Confidence keseluruhan berdasarkan konsistensi
  // informasi visual, bukan probabilitas terkalibrasi.
  int usefulZones = 0;

  for (int s = 0; s < 3; s++) {
    for (int z = 0; z < 3; z++) {
      if (
        zoneBrightness[s][z] > 8 ||
        zoneContrast[s][z] > 8
      ) {
        usefulZones++;
      }
    }
  }

  currentState.confidence = clampByte(
    35 + (usefulZones * 5)
  );

  if (currentState.confidence > 90) {
    currentState.confidence = 90;
  }

  currentState.valid = true;

  result = currentState;
  return true;
}
