#include "knowledge.h"
#include <LittleFS.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

static const char *KNOWLEDGE_FILE = "/knowledge.dat";
static const char *KNOWLEDGE_TMP  = "/knowledge.tmp";
static const char *KNOWLEDGE_BAK  = "/knowledge.bak";

static const uint32_t MAGIC_OLD = 0x4B4E4F58;
static const uint32_t MAGIC_NEW = 0x4B4E4F59;
static const uint8_t MAX_SAMPLES = 48;
static const uint8_t MATCH_THRESHOLD = 84;

struct __attribute__((packed)) KnowledgeRecord {
  uint32_t magic;
  char label[KNOWLEDGE_LABEL_SIZE];
  uint8_t features[KNOWLEDGE_FEATURE_SIZE];
};

static KnowledgeRecord records[MAX_SAMPLES];
static uint8_t recordCount = 0;
static bool ready = false;

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static void getRGB(uint16_t p, int &r, int &g, int &b) {
  r = ((p >> 11) & 31) * 255 / 31;
  g = ((p >> 5) & 63) * 255 / 63;
  b = (p & 31) * 255 / 31;
}

static int colorDistance(uint16_t a, uint16_t b) {
  int ar, ag, ab, br, bg, bb;
  getRGB(a, ar, ag, ab);
  getRGB(b, br, bg, bb);
  return (abs(ar - br) * 3 + abs(ag - bg) * 4 +
          abs(ab - bb) * 2) / 9;
}

static bool validLabel(const char *label) {
  return label && label[0] &&
         strnlen(label, KNOWLEDGE_LABEL_SIZE) <
         KNOWLEDGE_LABEL_SIZE;
}

bool knowledgeBegin() {
  ready = false;
  recordCount = 0;

  if (!LittleFS.begin(false)) {
    Serial.println("TARS: LITTLEFS GAGAL, DATA TIDAK DIHAPUS");
    return false;
  }

  if (!LittleFS.exists(KNOWLEDGE_FILE)) {
    ready = true;
    Serial.println("TARS: MEMORI OBJEK KOSONG");
    return true;
  }

  File f = LittleFS.open(KNOWLEDGE_FILE, "r");
  if (!f) {
    Serial.println("TARS: KNOWLEDGE GAGAL DIBUKA");
    return false;
  }

  const size_t recordSize = sizeof(KnowledgeRecord);
  size_t fileSize = f.size();

  if (fileSize % recordSize != 0 ||
      fileSize / recordSize > MAX_SAMPLES) {
    f.close();
    Serial.println("TARS: FORMAT KNOWLEDGE TIDAK COCOK; DATA DIPERTAHANKAN");
    return false;
  }

  uint8_t loaded = 0;
  bool valid = true;

  while (f.available() && loaded < MAX_SAMPLES) {
    KnowledgeRecord rec;

    if (f.read((uint8_t *)&rec, recordSize) != recordSize) {
      valid = false;
      break;
    }

    if ((rec.magic != MAGIC_OLD && rec.magic != MAGIC_NEW) ||
        rec.label[0] == '\0' ||
        memchr(rec.label, '\0', KNOWLEDGE_LABEL_SIZE) == nullptr) {
      valid = false;
      break;
    }

    records[loaded++] = rec;
  }

  f.close();

  if (!valid || loaded != fileSize / recordSize) {
    recordCount = 0;
    Serial.println("TARS: DATA KNOWLEDGE TIDAK VALID; FILE ASLI TIDAK DIHAPUS");
    return false;
  }

  recordCount = loaded;
  ready = true;

  Serial.printf("TARS: MEMORI OBJEK DIMUAT %u SAMPEL\n", recordCount);
  return true;
}

bool knowledgeBuildFeatures(const uint16_t *image, uint8_t *features) {
  if (!image || !features) return false;

  const int W = KNOWLEDGE_WIDTH;
  const int H = KNOWLEDGE_HEIGHT;

  uint32_t edgeR = 0, edgeG = 0, edgeB = 0;
  uint32_t edgeCount = 0;

  // Perkirakan warna latar dari tepian gambar.
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      if (x != 0 && x != W - 1 &&
          y != 0 && y != H - 1) continue;

      int r, g, b;
      getRGB(image[y * W + x], r, g, b);
      edgeR += r;
      edgeG += g;
      edgeB += b;
      edgeCount++;
    }
  }

  if (!edgeCount) return false;

  uint16_t background = rgb565(
    edgeR / edgeCount,
    edgeG / edgeCount,
    edgeB / edgeCount
  );

  int minX = W, minY = H, maxX = -1, maxY = -1;
  int foregroundCount = 0;

  // Cari area yang berbeda dari latar.
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      if (colorDistance(image[y * W + x], background) < 28)
        continue;

      if (x < minX) minX = x;
      if (x > maxX) maxX = x;
      if (y < minY) minY = y;
      if (y > maxY) maxY = y;
      foregroundCount++;
    }
  }

  if (maxX < minX || maxY < minY) return false;

  int boxW = maxX - minX + 1;
  int boxH = maxY - minY + 1;
  int boxArea = boxW * boxH;

  if (boxW < 5 || boxH < 5 ||
      boxArea < 30 ||
      foregroundCount < 20 ||
      foregroundCount > (W * H * 92 / 100))
    return false;

  // Fitur 4x4: bentuk, kegelapan, warna, dan tepi.
  int idx = 0;

  for (int cy = 0; cy < 4; cy++) {
    for (int cx = 0; cx < 4; cx++) {
      int x0 = minX + cx * boxW / 4;
      int x1 = minX + (cx + 1) * boxW / 4;
      int y0 = minY + cy * boxH / 4;
      int y1 = minY + (cy + 1) * boxH / 4;

      int count = 0;
      int darkSum = 0;
      int chromaSum = 0;
      int edgeSum = 0;
      int pixels = 0;

      for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
          uint16_t p = image[y * W + x];
          int r, g, b, br, bg, bb;
          getRGB(p, r, g, b);
          getRGB(background, br, bg, bb);

          int dist = colorDistance(p, background);
          bool fg = dist >= 28;

          if (fg) count++;

          int lum = (r * 3 + g * 6 + b) / 10;
          int bgLum = (br * 3 + bg * 6 + bb) / 10;

          int darkness = bgLum - lum;
          if (darkness < 0) darkness = 0;
          if (darkness > 255) darkness = 255;
          darkSum += darkness;

          int hi = r;
          if (g > hi) hi = g;
          if (b > hi) hi = b;

          int lo = r;
          if (g < lo) lo = g;
          if (b < lo) lo = b;

          chromaSum += hi - lo;

          if (x > minX && y > minY) {
            int right = colorDistance(p, image[y * W + x - 1]);
            int down  = colorDistance(p, image[(y - 1) * W + x]);
            edgeSum += (right + down) / 2;
          }

          pixels++;
        }
      }

      if (!pixels) pixels = 1;

      features[idx++] = (uint8_t)(count * 255 / pixels);
      features[idx++] = (uint8_t)(darkSum / pixels);
      features[idx++] = (uint8_t)(chromaSum / pixels);
      features[idx++] = (uint8_t)(edgeSum / pixels);
    }
  }

  return true;
}

bool knowledgeLearnFeatures(const char *label, const uint8_t *features) {
  if (!ready || !validLabel(label) || !features ||
      recordCount >= MAX_SAMPLES) {
    Serial.println("TARS: GAGAL BELAJAR, CEK LABEL/MEMORI");
    return false;
  }

  KnowledgeRecord rec = {};
  rec.magic = MAGIC_NEW;
  strncpy(rec.label, label, KNOWLEDGE_LABEL_SIZE - 1);
  memcpy(rec.features, features, KNOWLEDGE_FEATURE_SIZE);

  File f = LittleFS.open(KNOWLEDGE_FILE, "a");
  if (!f) {
    Serial.println("TARS: FILE KNOWLEDGE GAGAL DIBUKA");
    return false;
  }

  size_t written = f.write((const uint8_t *)&rec, sizeof(rec));
  f.flush();
  f.close();

  if (written != sizeof(rec)) {
    Serial.println("TARS: PENYIMPANAN FITUR GAGAL");
    return false;
  }

  records[recordCount++] = rec;
  Serial.printf("TARS: OBJEK '%s' DISIMPAN\n", label);
  return true;
}

bool knowledgeLearn(const char *label, const uint16_t *image) {
  uint8_t features[KNOWLEDGE_FEATURE_SIZE];

  if (!knowledgeBuildFeatures(image, features)) {
    Serial.println("TARS: OBJEK TIDAK TERPISAH DARI LATAR");
    return false;
  }

  return knowledgeLearnFeatures(label, features);
}

bool knowledgeRecognizeFeatures(
  const uint8_t *features,
  char *labelOut,
  size_t labelOutSize,
  uint8_t *confidenceOut
) {
  if (!ready || !features || !labelOut || labelOutSize == 0)
    return false;

  labelOut[0] = '\0';
  if (confidenceOut) *confidenceOut = 0;

  int bestScore = 1000;
  int secondScore = 1000;
  int bestIndex = -1;

  for (int n = 0; n < recordCount; n++) {
    // Fitur lama tetap disimpan, tetapi tidak dicampur dengan
    // fitur baru karena cara pembentukan gambar bisa berbeda.
    if (records[n].magic != MAGIC_NEW) continue;

    int shapeDiff = 0, darkDiff = 0;
    int colorDiff = 0, edgeDiff = 0;

    for (int i = 0; i < KNOWLEDGE_FEATURE_SIZE; i += 4) {
      shapeDiff += abs((int)features[i] -
                       (int)records[n].features[i]);
      darkDiff += abs((int)features[i + 1] -
                      (int)records[n].features[i + 1]);
      colorDiff += abs((int)features[i + 2] -
                       (int)records[n].features[i + 2]);
      edgeDiff += abs((int)features[i + 3] -
                      (int)records[n].features[i + 3]);
    }

    shapeDiff /= 16;
    darkDiff /= 16;
    colorDiff /= 16;
    edgeDiff /= 16;

    int score =
      (shapeDiff * 45 +
       darkDiff * 20 +
       colorDiff * 10 +
       edgeDiff * 25) / 100;

    if (score < bestScore) {
      secondScore = bestScore;
      bestScore = score;
      bestIndex = n;
    } else if (score < secondScore) {
      secondScore = score;
    }
  }

  if (bestIndex < 0) return false;

  int confidence = 100 - bestScore * 100 / 255;
  if (confidence < 0) confidence = 0;
  if (confidence > 100) confidence = 100;

  if (confidenceOut) *confidenceOut = (uint8_t)confidence;

  if (bestScore > (255 - MATCH_THRESHOLD))
    return false;

  strncpy(labelOut, records[bestIndex].label, labelOutSize - 1);
  labelOut[labelOutSize - 1] = '\0';
  return true;
}

bool knowledgeRecognize(
  const uint16_t *image,
  char *labelOut,
  size_t labelOutSize,
  uint8_t *confidenceOut
) {
  uint8_t features[KNOWLEDGE_FEATURE_SIZE];

  if (!knowledgeBuildFeatures(image, features)) {
    if (labelOut && labelOutSize) labelOut[0] = '\0';
    if (confidenceOut) *confidenceOut = 0;
    return false;
  }

  return knowledgeRecognizeFeatures(
    features, labelOut, labelOutSize, confidenceOut
  );
}

uint16_t knowledgeSampleCount() {
  return recordCount;
}

bool knowledgeDeleteLabel(const char *label) {
  if (!ready || !validLabel(label)) return false;

  KnowledgeRecord kept[MAX_SAMPLES];
  uint8_t keptCount = 0;
  uint8_t removed = 0;

  for (int i = 0; i < recordCount; i++) {
    if (strcasecmp(records[i].label, label) == 0) {
      removed++;
    } else {
      kept[keptCount++] = records[i];
    }
  }

  if (!removed) return false;

  LittleFS.remove(KNOWLEDGE_TMP);
  File tmp = LittleFS.open(KNOWLEDGE_TMP, "w");
  if (!tmp) return false;

  bool writeOK = true;
  for (int i = 0; i < keptCount; i++) {
    if (tmp.write((const uint8_t *)&kept[i],
                  sizeof(KnowledgeRecord)) != sizeof(KnowledgeRecord)) {
      writeOK = false;
      break;
    }
  }

  tmp.flush();
  tmp.close();

  if (!writeOK) {
    LittleFS.remove(KNOWLEDGE_TMP);
    return false;
  }

  // Backup file lama sebelum menggantinya.
  LittleFS.remove(KNOWLEDGE_BAK);

  if (LittleFS.exists(KNOWLEDGE_FILE) &&
      !LittleFS.rename(KNOWLEDGE_FILE, KNOWLEDGE_BAK)) {
    LittleFS.remove(KNOWLEDGE_TMP);
    return false;
  }

  if (!LittleFS.rename(KNOWLEDGE_TMP, KNOWLEDGE_FILE)) {
    if (LittleFS.exists(KNOWLEDGE_BAK))
      LittleFS.rename(KNOWLEDGE_BAK, KNOWLEDGE_FILE);
    return false;
  }

  memcpy(records, kept, keptCount * sizeof(KnowledgeRecord));
  recordCount = keptCount;

  Serial.printf("TARS: LABEL '%s' DIHAPUS, %u SAMPEL\n",
                label, removed);
  return true;
}
