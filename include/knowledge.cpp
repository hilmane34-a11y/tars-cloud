#include "knowledge.h"
#include <LittleFS.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

static const char *KNOWLEDGE_FILE = "/knowledge.dat";
static const uint32_t KNOWLEDGE_MAGIC = 0x4B4E4F59;
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

static inline void getRGB(
    uint16_t p, int &r, int &g, int &b) {
  r = ((p >> 11) & 31) * 255 / 31;
  g = ((p >> 5) & 63) * 255 / 63;
  b = (p & 31) * 255 / 31;
}

static inline int luminance(int r, int g, int b) {
  return (77 * r + 150 * g + 29 * b) >> 8;
}

static inline int colorDistance(
    int r, int g, int b,
    int br, int bg, int bb) {
  return (abs(r - br) * 3 +
          abs(g - bg) * 4 +
          abs(b - bb) * 2) / 3;
}

static inline bool isForeground(
    uint16_t p, int br, int bg, int bb) {
  int r, g, b;
  getRGB(p, r, g, b);

  // Perbedaan warna dari warna dominan tepi gambar.
  // Objek hitam tetap dapat lolos karena kontrasnya tinggi.
  return colorDistance(r, g, b, br, bg, bb) >= 28;
}

bool knowledgeBegin() {
  recordCount = 0;
  ready = false;

  File f = LittleFS.open(KNOWLEDGE_FILE, "r");
  bool incompatible = false;

  if (f) {
    while (f.available() >= (int)sizeof(KnowledgeRecord)) {
      KnowledgeRecord r;

      if (f.read((uint8_t *)&r, sizeof(r)) != sizeof(r) ||
          r.magic != KNOWLEDGE_MAGIC ||
          r.label[0] == '\0') {
        incompatible = true;
        break;
      }

      r.label[KNOWLEDGE_LABEL_SIZE - 1] = '\0';

      if (recordCount < MAX_SAMPLES) {
        records[recordCount++] = r;
      }
    }

    if (f.available() > 0) incompatible = true;
    f.close();
  }

  if (incompatible) {
    recordCount = 0;
    LittleFS.remove(KNOWLEDGE_FILE);
    Serial.println(
      "TARS: FORMAT KNOWLEDGE BARU, AJARKAN ULANG OBJEK"
    );
  }

  ready = true;
  return true;
}

bool knowledgeBuildFeatures(
    const uint16_t *image,
    uint8_t *features) {
  if (!image || !features) return false;

  const int W = KNOWLEDGE_WIDTH;
  const int H = KNOWLEDGE_HEIGHT;

  // Perkirakan warna latar dari tepi gambar.
  // Tidak menyimpan atau membuat salinan background.
  uint32_t sr = 0, sg = 0, sb = 0;
  uint32_t n = 0;

  for (int x = 0; x < W; x++) {
    int r, g, b;
    getRGB(image[x], r, g, b);
    sr += r; sg += g; sb += b; n++;

    int k = (H - 1) * W + x;
    getRGB(image[k], r, g, b);
    sr += r; sg += g; sb += b; n++;
  }

  for (int y = 1; y < H - 1; y++) {
    int r, g, b;
    getRGB(image[y * W], r, g, b);
    sr += r; sg += g; sb += b; n++;

    getRGB(image[y * W + W - 1], r, g, b);
    sr += r; sg += g; sb += b; n++;
  }

  if (!n) return false;

  int br = sr / n;
  int bg = sg / n;
  int bb = sb / n;
  int bgLum = luminance(br, bg, bb);

  int minX = W, minY = H, maxX = -1, maxY = -1;
  int foregroundCount = 0;

  // Cari objek di seluruh frame, bukan hanya tengah.
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      if (!isForeground(image[y * W + x], br, bg, bb))
        continue;

      if (x < minX) minX = x;
      if (x > maxX) maxX = x;
      if (y < minY) minY = y;
      if (y > maxY) maxY = y;
      foregroundCount++;
    }
  }

  int boxW = maxX - minX + 1;
  int boxH = maxY - minY + 1;
  int boxArea = boxW * boxH;

  // Tolak frame kosong atau perubahan yang memenuhi hampir
  // seluruh gambar. Tidak menyimpan background sebagai objek.
  if (maxX < 0 || foregroundCount < 12 ||
      boxW < 4 || boxH < 4 ||
      boxArea < 20 ||
      boxArea > (W * H * 92 / 100)) {
    memset(features, 0, KNOWLEDGE_FEATURE_SIZE);
    return false;
  }

  // 4x4 sel mengikuti bounding box objek.
  // Setiap sel menyimpan:
  // [0] kepadatan bentuk, [1] tingkat gelap relatif,
  // [2] warna/chroma, [3] kekuatan tepi.
  for (int cy = 0; cy < 4; cy++) {
    for (int cx = 0; cx < 4; cx++) {
      int x0 = minX + cx * boxW / 4;
      int x1 = minX + (cx + 1) * boxW / 4;
      int y0 = minY + cy * boxH / 4;
      int y1 = minY + (cy + 1) * boxH / 4;

      uint32_t count = 0, total = 0;
      uint32_t sumLum = 0, sumChr = 0, sumEdge = 0;
      uint32_t edgeN = 0;

      for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
          total++;
          uint16_t p = image[y * W + x];

          if (!isForeground(p, br, bg, bb)) continue;

          int r, g, b;
          getRGB(p, r, g, b);
          int lum = luminance(r, g, b);
          int hi = max(r, max(g, b));
          int lo = min(r, min(g, b));

          count++;
          sumLum += lum;
          sumChr += hi - lo;

          // Ukur tepi objek lokal tanpa buffer tambahan.
          if (x + 1 < x1 &&
              isForeground(image[y * W + x + 1],
                           br, bg, bb)) {
            int r2, g2, b2;
            getRGB(image[y * W + x + 1], r2, g2, b2);
            sumEdge += abs(lum -
                luminance(r2, g2, b2));
            edgeN++;
          }

          if (y + 1 < y1 &&
              isForeground(image[(y + 1) * W + x],
                           br, bg, bb)) {
            int r2, g2, b2;
            getRGB(image[(y + 1) * W + x], r2, g2, b2);
            sumEdge += abs(lum -
                luminance(r2, g2, b2));
            edgeN++;
          }
        }
      }

      int i = (cy * 4 + cx) * 4;

      features[i] = total
          ? constrain((int)(count * 255 / total), 0, 255)
          : 0;

      if (count) {
        int avgLum = sumLum / count;

        // 0 = tidak lebih gelap dari latar,
        // 100 = sangat gelap relatif terhadap latar.
        int dark = bgLum > 0
            ? (bgLum - avgLum) * 100 / bgLum
            : 0;
        features[i + 1] = constrain(dark, 0, 100);

        features[i + 2] = constrain(
            (int)(sumChr / count), 0, 255);

        features[i + 3] = edgeN
            ? constrain((int)(sumEdge / edgeN), 0, 255)
            : 0;
      } else {
        features[i + 1] = 0;
        features[i + 2] = 0;
        features[i + 3] = 0;
      }
    }
  }

  return true;
}

bool knowledgeLearnFeatures(
    const char *label,
    const uint8_t *features) {
  if (!ready || !label || !features ||
      recordCount >= MAX_SAMPLES) return false;

  while (*label == ' ') label++;
  if (!*label) return false;

  KnowledgeRecord r = {};
  r.magic = KNOWLEDGE_MAGIC;

  size_t len = strlen(label);
  if (len >= sizeof(r.label)) len = sizeof(r.label) - 1;
  memcpy(r.label, label, len);
  r.label[len] = '\0';

  memcpy(r.features, features, KNOWLEDGE_FEATURE_SIZE);

  File f = LittleFS.open(KNOWLEDGE_FILE, "a");
  if (!f) return false;

  size_t written = f.write((const uint8_t *)&r, sizeof(r));
  f.flush();
  f.close();

  if (written != sizeof(r)) return false;

  records[recordCount++] = r;
  return true;
}

bool knowledgeLearn(
    const char *label,
    const uint16_t *image) {
  uint8_t features[KNOWLEDGE_FEATURE_SIZE];

  if (!knowledgeBuildFeatures(image, features))
    return false;

  return knowledgeLearnFeatures(label, features);
}

bool knowledgeRecognizeFeatures(
    const uint8_t *features,
    char *labelOut,
    size_t labelOutSize,
    uint8_t *confidenceOut) {
  if (labelOut && labelOutSize) labelOut[0] = '\0';
  if (confidenceOut) *confidenceOut = 0;

  if (!ready || !features || !labelOut ||
      !labelOutSize || recordCount == 0) return false;

  float bestScore = -1.0f;
  int bestIndex = -1;

  for (int n = 0; n < recordCount; n++) {
    float shapeDiff = 0;
    float darkDiff = 0;
    float colorDiff = 0;
    float edgeDiff = 0;

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

    shapeDiff /= (16.0f * 255.0f);
    darkDiff /= (16.0f * 100.0f);
    colorDiff /= (16.0f * 255.0f);
    edgeDiff /= (16.0f * 255.0f);

    float diff =
        0.45f * shapeDiff +
        0.20f * darkDiff +
        0.10f * colorDiff +
        0.25f * edgeDiff;

    float score = 100.0f * (1.0f - diff);

    if (score > bestScore) {
      bestScore = score;
      bestIndex = n;
    }
  }

  if (bestIndex < 0) return false;

  uint8_t confidence = constrain(
      (int)(bestScore + 0.5f), 0, 100);

  if (confidenceOut) *confidenceOut = confidence;
  if (bestScore < MATCH_THRESHOLD) return false;

  strncpy(labelOut, records[bestIndex].label,
          labelOutSize - 1);
  labelOut[labelOutSize - 1] = '\0';
  return true;
}

bool knowledgeRecognize(
    const uint16_t *image,
    char *labelOut,
    size_t labelOutSize,
    uint8_t *confidenceOut) {
  uint8_t features[KNOWLEDGE_FEATURE_SIZE];

  if (!knowledgeBuildFeatures(image, features)) {
    if (labelOut && labelOutSize) labelOut[0] = '\0';
    if (confidenceOut) *confidenceOut = 0;
    return false;
  }

  return knowledgeRecognizeFeatures(
      features, labelOut, labelOutSize, confidenceOut);
}

uint16_t knowledgeSampleCount() {
  return recordCount;
}

bool knowledgeDeleteLabel(const char *label) {
  if (!ready || !label || !*label) return false;

  uint8_t removed = 0;
  for (int i = 0; i < recordCount; i++) {
    if (strcasecmp(records[i].label, label) == 0)
      removed++;
  }
  if (!removed) return false;

  const char *tmp = "/knowledge.tmp";
  const char *bak = "/knowledge.bak";

  LittleFS.remove(tmp);
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;

  size_t expected = 0;

  for (int i = 0; i < recordCount; i++) {
    if (strcasecmp(records[i].label, label) == 0)
      continue;

    expected += sizeof(KnowledgeRecord);

    if (f.write((const uint8_t *)&records[i],
                sizeof(KnowledgeRecord)) !=
                sizeof(KnowledgeRecord)) {
      f.close();
      LittleFS.remove(tmp);
      return false;
    }
  }

  f.flush();
  f.close();

  File check = LittleFS.open(tmp, "r");
  if (!check || check.size() != expected) {
    if (check) check.close();
    LittleFS.remove(tmp);
    return false;
  }
  check.close();

  LittleFS.remove(bak);
  bool hadOriginal = LittleFS.exists(KNOWLEDGE_FILE);

  if (hadOriginal &&
      !LittleFS.rename(KNOWLEDGE_FILE, bak)) {
    LittleFS.remove(tmp);
    return false;
  }

  if (!LittleFS.rename(tmp, KNOWLEDGE_FILE)) {
    if (hadOriginal) LittleFS.rename(bak, KNOWLEDGE_FILE);
    LittleFS.remove(tmp);
    return false;
  }

  int dst = 0;

  for (int i = 0; i < recordCount; i++) {
    if (strcasecmp(records[i].label, label) != 0) {
      if (dst != i) records[dst] = records[i];
      dst++;
    }
  }

  recordCount = dst;
  LittleFS.remove(bak);

  Serial.printf(
      "TARS: LABEL '%s' DIHAPUS, %u sampel tersisa\n",
      label, recordCount);

  return true;
}
