#include "knowledge.h"
#include <LittleFS.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

static const char *KNOWLEDGE_FILE = "/knowledge.dat";
static const char *KNOWLEDGE_TMP  = "/knowledge.tmp";
static const char *KNOWLEDGE_BAK  = "/knowledge.bak";

static const uint32_t MAGIC_OLD  = 0x4B4E4F58;
static const uint32_t MAGIC_NEW  = 0x4B4E4F59;
static const uint32_t MAGIC_MARK = 0x4B4E4F5A;

static const uint8_t MAX_SAMPLES = 48;
static const uint8_t MATCH_THRESHOLD = 94;

#define GW 32
#define GH 16
#define GP (GW * GH)

struct __attribute__((packed)) KnowledgeRecord {
  uint32_t magic;
  char label[KNOWLEDGE_LABEL_SIZE];
  uint8_t features[KNOWLEDGE_FEATURE_SIZE];
};

// Alokasi dinamis untuk mengurangi DRAM statis.
static KnowledgeRecord *records = nullptr;
static uint8_t *objectMask = nullptr;
static uint16_t *floodQueue = nullptr;

static uint8_t recordCount = 0;
static bool ready = false;

static bool allocateBuffers() {
  if (!records)
    records = (KnowledgeRecord *)malloc(
      sizeof(KnowledgeRecord) * MAX_SAMPLES
    );

  if (!objectMask)
    objectMask = (uint8_t *)malloc(GP);

  if (!floodQueue)
    floodQueue = (uint16_t *)malloc(
      sizeof(uint16_t) * GP
    );

  if (!records || !objectMask || !floodQueue) {
    Serial.println("TARS: RAM KURANG UNTUK MEMORI OBJEK");
    return false;
  }

  return true;
}

static uint16_t samplePixel(
  const uint16_t *image, int x, int y
) {
  int px = x * KNOWLEDGE_WIDTH / GW;
  int py = y * KNOWLEDGE_HEIGHT / GH;

  if (px >= KNOWLEDGE_WIDTH) px = KNOWLEDGE_WIDTH - 1;
  if (py >= KNOWLEDGE_HEIGHT) py = KNOWLEDGE_HEIGHT - 1;

  return image[py * KNOWLEDGE_WIDTH + px];
}

static void getRGB(uint16_t p, int &r, int &g, int &b) {
  r = ((p >> 11) & 31) * 255 / 31;
  g = ((p >> 5) & 63) * 255 / 63;
  b = (p & 31) * 255 / 31;
}

static int colorDistance(uint16_t a, uint16_t b) {
  int dr = abs((int)((a >> 11) & 31) -
               (int)((b >> 11) & 31)) * 255 / 31;
  int dg = abs((int)((a >> 5) & 63) -
               (int)((b >> 5) & 63)) * 255 / 63;
  int db = abs((int)(a & 31) -
               (int)(b & 31)) * 255 / 31;

  return (dr * 3 + dg * 4 + db * 2) / 9;
}

static bool validLabel(const char *label) {
  return label && label[0] &&
    strnlen(label, KNOWLEDGE_LABEL_SIZE) <
    KNOWLEDGE_LABEL_SIZE;
}

bool knowledgeBegin() {
  ready = false;
  recordCount = 0;

  // Jangan format LittleFS jika mount gagal.
  if (!LittleFS.begin(false)) {
    Serial.println("TARS: LITTLEFS GAGAL; DATA DIPERTAHANKAN");
    return false;
  }

  if (!allocateBuffers())
    return false;

  // Pulihkan backup hanya jika file utama tidak ada.
  if (!LittleFS.exists(KNOWLEDGE_FILE) &&
      LittleFS.exists(KNOWLEDGE_BAK)) {
    if (!LittleFS.rename(KNOWLEDGE_BAK, KNOWLEDGE_FILE)) {
      Serial.println("TARS: BACKUP GAGAL DIPULIHKAN");
      return false;
    }
  }

  if (!LittleFS.exists(KNOWLEDGE_FILE)) {
    ready = true;
    Serial.println("TARS: MEMORI OBJEK KOSONG");
    return true;
  }

  File f = LittleFS.open(KNOWLEDGE_FILE, "r");
  if (!f) {
    Serial.println("TARS: FILE MEMORI GAGAL DIBUKA");
    return false;
  }

  const size_t rs = sizeof(KnowledgeRecord);
  size_t fs = f.size();

  if (fs % rs != 0 || fs / rs > MAX_SAMPLES) {
    f.close();
    Serial.println("TARS: FORMAT TIDAK COCOK; FILE DIPERTAHANKAN");
    return false;
  }

  uint8_t loaded = 0;
  bool valid = true;

  while (f.available() && loaded < MAX_SAMPLES) {
    KnowledgeRecord rec;

    if (f.read((uint8_t *)&rec, rs) != rs) {
      valid = false;
      break;
    }

    if ((rec.magic != MAGIC_OLD &&
         rec.magic != MAGIC_NEW &&
         rec.magic != MAGIC_MARK) ||
        !memchr(rec.label, '\0', KNOWLEDGE_LABEL_SIZE) ||
        rec.label[0] == '\0') {
      valid = false;
      break;
    }

    records[loaded++] = rec;
  }

  f.close();

  if (!valid || loaded != fs / rs) {
    recordCount = 0;
    Serial.println("TARS: DATA TIDAK VALID; FILE ASLI DIPERTAHANKAN");
    return false;
  }

  recordCount = loaded;
  ready = true;

  Serial.printf("TARS: MEMUAT %u SAMPEL OBJEK\n", recordCount);
  return true;
}

// Ekstraksi fitur dari wilayah warna yang terhubung.
static bool extractAt(
  const uint16_t *image,
  int sx, int sy,
  uint8_t *features,
  int *outArea = nullptr,
  int *outContrast = nullptr
) {
  if (!image || !features ||
      !objectMask || !floodQueue ||
      sx < 1 || sx >= GW - 1 ||
      sy < 1 || sy >= GH - 1)
    return false;

  memset(objectMask, 0, GP);

  const uint16_t seedColor = samplePixel(image, sx, sy);
  const int COLOR_LIMIT = 42;

  int head = 0;
  int tail = 0;
  int minX = GW, minY = GH;
  int maxX = -1, maxY = -1;
  bool touchesEdge = false;

  int seed = sy * GW + sx;
  objectMask[seed] = 1;
  floodQueue[tail++] = seed;

  while (head < tail) {
    int pos = floodQueue[head++];
    int x = pos % GW;
    int y = pos / GW;

    if (x < minX) minX = x;
    if (x > maxX) maxX = x;
    if (y < minY) minY = y;
    if (y > maxY) maxY = y;

    if (x == 0 || y == 0 ||
        x == GW - 1 || y == GH - 1)
      touchesEdge = true;

    const int nx[4] = {x - 1, x + 1, x, x};
    const int ny[4] = {y, y, y - 1, y + 1};

    for (int k = 0; k < 4; k++) {
      int xx = nx[k];
      int yy = ny[k];

      if (xx < 0 || xx >= GW ||
          yy < 0 || yy >= GH)
        continue;

      int ni = yy * GW + xx;
      if (objectMask[ni]) continue;

      if (colorDistance(
            samplePixel(image, xx, yy), seedColor
          ) <= COLOR_LIMIT) {
        objectMask[ni] = 1;

        if (tail < GP)
          floodQueue[tail++] = ni;
      }
    }

    if (tail > GP * 45 / 100)
      return false;
  }

  const int area = tail;
  const int bw = maxX - minX + 1;
  const int bh = maxY - minY + 1;

  if (touchesEdge || area < 8 ||
      bw < 3 || bh < 3 || bw * bh < 12 ||
      area > GP * 45 / 100)
    return false;

  int boundarySum = 0;
  int boundaryCount = 0;

  for (int y = minY; y <= maxY; y++) {
    for (int x = minX; x <= maxX; x++) {
      if (!objectMask[y * GW + x]) continue;

      const int nx[4] = {x - 1, x + 1, x, x};
      const int ny[4] = {y, y, y - 1, y + 1};

      for (int k = 0; k < 4; k++) {
        int xx = nx[k];
        int yy = ny[k];

        if (xx < 0 || xx >= GW ||
            yy < 0 || yy >= GH)
          continue;

        if (!objectMask[yy * GW + xx]) {
          boundarySum += colorDistance(
            samplePixel(image, x, y),
            samplePixel(image, xx, yy)
          );
          boundaryCount++;
        }
      }
    }
  }

  int contrast = boundaryCount
    ? boundarySum / boundaryCount : 0;

  if (contrast < 10)
    return false;

  // 64 fitur: 4 fitur untuk masing-masing 16 sel.
  int idx = 0;

  for (int cy = 0; cy < 4; cy++) {
    for (int cx = 0; cx < 4; cx++) {
      int x0 = minX + cx * bw / 4;
      int x1 = minX + (cx + 1) * bw / 4;
      int y0 = minY + cy * bh / 4;
      int y1 = minY + (cy + 1) * bh / 4;

      int maskCount = 0;
      int lumSum = 0;
      int chromaSum = 0;
      int edgeSum = 0;

      for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
          if (!objectMask[y * GW + x]) continue;

          int r, g, b;
          getRGB(samplePixel(image, x, y), r, g, b);

          lumSum += (r * 3 + g * 6 + b) / 10;

          int hi = max(r, max(g, b));
          int lo = min(r, min(g, b));
          chromaSum += hi - lo;

          if (x + 1 < GW &&
              objectMask[y * GW + x + 1]) {
            edgeSum += colorDistance(
              samplePixel(image, x, y),
              samplePixel(image, x + 1, y)
            );
          }

          if (y + 1 < GH &&
              objectMask[(y + 1) * GW + x]) {
            edgeSum += colorDistance(
              samplePixel(image, x, y),
              samplePixel(image, x, y + 1)
            );
          }

          maskCount++;
        }
      }

      int cellArea = (x1 - x0) * (y1 - y0);
      if (cellArea < 1) cellArea = 1;

      features[idx++] = maskCount * 255 / cellArea;
      features[idx++] = maskCount ? lumSum / maskCount : 0;
      features[idx++] = maskCount ? chromaSum / maskCount : 0;
      features[idx++] = maskCount ? edgeSum / maskCount : 0;
    }
  }

  if (outArea) *outArea = area;
  if (outContrast) *outContrast = contrast;

  return true;
}

// Belajar memprioritaskan objek di tengah gambar.
bool knowledgeBuildFeatures(
  const uint16_t *image,
  uint8_t *features
) {
  if (!ready || !image || !features ||
      !objectMask || !floodQueue)
    return false;

  uint8_t candidate[KNOWLEDGE_FEATURE_SIZE];
  bool found = false;
  int bestScore = -100000;

  const int cx = GW / 2;
  const int cy = GH / 2;

  for (int y = GH / 4; y < GH * 3 / 4; y += 2) {
    for (int x = GW / 4; x < GW * 3 / 4; x += 2) {
      int area = 0;
      int contrast = 0;

      if (!extractAt(image, x, y, candidate,
                     &area, &contrast))
        continue;

      int distance = abs(x - cx) + abs(y - cy);

      int score = 1000
                - distance * 20
                + contrast * 3
                + (area < 100 ? area : 100) / 3;

      if (score > bestScore) {
        bestScore = score;
        memcpy(features, candidate,
               KNOWLEDGE_FEATURE_SIZE);
        found = true;
      }
    }
  }

  return found;
}

// Tulis file sementara, lalu ganti file utama secara transaksional.
static bool writeRecordsTransactional(
  const KnowledgeRecord *list,
  uint8_t count
) {
  // Jangan menimpa backup yang mungkin berisi data pemulihan.
  if (LittleFS.exists(KNOWLEDGE_BAK)) {
    Serial.println("TARS: BACKUP ADA; PENYIMPANAN DIBATALKAN");
    return false;
  }

  LittleFS.remove(KNOWLEDGE_TMP);

  File tmp = LittleFS.open(KNOWLEDGE_TMP, "w");
  if (!tmp) return false;

  bool ok = true;

  for (int i = 0; i < count; i++) {
    if (tmp.write((const uint8_t *)&list[i],
                  sizeof(KnowledgeRecord)) !=
        sizeof(KnowledgeRecord)) {
      ok = false;
      break;
    }
  }

  tmp.flush();
  tmp.close();

  if (!ok) {
    LittleFS.remove(KNOWLEDGE_TMP);
    return false;
  }

  bool hadOriginal = LittleFS.exists(KNOWLEDGE_FILE);

  if (hadOriginal &&
      !LittleFS.rename(KNOWLEDGE_FILE, KNOWLEDGE_BAK)) {
    LittleFS.remove(KNOWLEDGE_TMP);
    return false;
  }

  if (!LittleFS.rename(KNOWLEDGE_TMP, KNOWLEDGE_FILE)) {
    if (hadOriginal &&
        LittleFS.exists(KNOWLEDGE_BAK)) {
      LittleFS.rename(KNOWLEDGE_BAK, KNOWLEDGE_FILE);
    }

    LittleFS.remove(KNOWLEDGE_TMP);
    return false;
  }

  // File baru sudah terpasang dengan sukses.
  if (LittleFS.exists(KNOWLEDGE_BAK))
    LittleFS.remove(KNOWLEDGE_BAK);

  return true;
}

bool knowledgeLearnFeatures(
  const char *label,
  const uint8_t *features
) {
  if (!ready || !records || !validLabel(label) ||
      !features || recordCount >= MAX_SAMPLES) {
    Serial.println("TARS: GAGAL BELAJAR; CEK LABEL/MEMORI");
    return false;
  }

  KnowledgeRecord rec = {};
  rec.magic = MAGIC_MARK;
  strncpy(rec.label, label, KNOWLEDGE_LABEL_SIZE - 1);
  memcpy(rec.features, features, KNOWLEDGE_FEATURE_SIZE);

  // Gunakan slot kosong terakhir; tidak membuat array besar di stack.
  records[recordCount] = rec;

  if (!writeRecordsTransactional(records, recordCount + 1)) {
    memset(&records[recordCount], 0, sizeof(KnowledgeRecord));
    Serial.println("TARS: PENYIMPANAN GAGAL; DATA LAMA DIPERTAHANKAN");
    return false;
  }

  recordCount++;

  Serial.printf("TARS: OBJEK '%s' DISIMPAN (FITUR MARK)\n", label);
  return true;
}

bool knowledgeLearn(const char *label, const uint16_t *image) {
  uint8_t features[KNOWLEDGE_FEATURE_SIZE];

  if (!knowledgeBuildFeatures(image, features)) {
    Serial.println("TARS: MARK GAGAL; OBJEK TIDAK DISIMPAN");
    return false;
  }

  return knowledgeLearnFeatures(label, features);
}

static int compareFeatures(
  const uint8_t *a,
  const uint8_t *b
) {
  int shape = 0;
  int lum = 0;
  int color = 0;
  int edge = 0;

  for (int i = 0; i < KNOWLEDGE_FEATURE_SIZE; i += 4) {
    shape += abs((int)a[i]     - (int)b[i]);
    lum   += abs((int)a[i + 1] - (int)b[i + 1]);
    color += abs((int)a[i + 2] - (int)b[i + 2]);
    edge  += abs((int)a[i + 3] - (int)b[i + 3]);
  }

  shape /= 16;
  lum /= 16;
  color /= 16;
  edge /= 16;

  return (shape * 45 + lum * 20 +
          color * 10 + edge * 25) / 100;
}

bool knowledgeRecognizeFeatures(
  const uint8_t *features,
  char *labelOut,
  size_t labelOutSize,
  uint8_t *confidenceOut
) {
  if (!ready || !records || !features ||
      !labelOut || !labelOutSize)
    return false;

  labelOut[0] = '\0';
  if (confidenceOut) *confidenceOut = 0;

  int best = 1000;
  int bestIndex = -1;

  for (int n = 0; n < recordCount; n++) {
    if (records[n].magic != MAGIC_MARK) continue;

    int score = compareFeatures(features, records[n].features);

    if (score < best) {
      best = score;
      bestIndex = n;
    }
  }

  if (bestIndex < 0 || best > MATCH_THRESHOLD)
    return false;

  int confidence = 100 - best * 100 / 255;
  if (confidence < 0) confidence = 0;
  if (confidence > 100) confidence = 100;

  if (confidenceOut)
    *confidenceOut = (uint8_t)confidence;

  strncpy(labelOut, records[bestIndex].label, labelOutSize - 1);
  labelOut[labelOutSize - 1] = '\0';
  return true;
}

// Pindai seluruh frame, termasuk bagian sudut.
bool knowledgeRecognize(
  const uint16_t *image,
  char *labelOut,
  size_t labelOutSize,
  uint8_t *confidenceOut
) {
  if (!ready || !image || !labelOut || !labelOutSize ||
      !records || !objectMask || !floodQueue)
    return false;

  labelOut[0] = '\0';
  if (confidenceOut) *confidenceOut = 0;

  uint8_t candidate[KNOWLEDGE_FEATURE_SIZE];

  int best = 1000;
  int bestIndex = -1;

  for (int y = 1; y < GH - 1; y += 2) {
    for (int x = 1; x < GW - 1; x += 2) {
      if (!extractAt(image, x, y, candidate))
        continue;

      for (int n = 0; n < recordCount; n++) {
        if (records[n].magic != MAGIC_MARK)
          continue;

        int score = compareFeatures(
          candidate, records[n].features
        );

        if (score < best) {
          best = score;
          bestIndex = n;
        }
      }
    }
  }

  if (bestIndex < 0 || best > MATCH_THRESHOLD)
    return false;

  int confidence = 100 - best * 100 / 255;
  if (confidence < 0) confidence = 0;
  if (confidence > 100) confidence = 100;

  if (confidenceOut)
    *confidenceOut = (uint8_t)confidence;

  strncpy(labelOut, records[bestIndex].label, labelOutSize - 1);
  labelOut[labelOutSize - 1] = '\0';
  return true;
}

uint16_t knowledgeSampleCount() {
  return recordCount;
}

bool knowledgeDeleteLabel(const char *label) {
  if (!ready || !records || !validLabel(label))
    return false;

  // Jangan menimpa backup yang mungkin diperlukan untuk pemulihan.
  if (LittleFS.exists(KNOWLEDGE_BAK)) {
    Serial.println("TARS: BACKUP ADA; PENGHAPUSAN DIBATALKAN");
    return false;
  }

  LittleFS.remove(KNOWLEDGE_TMP);

  File tmp = LittleFS.open(KNOWLEDGE_TMP, "w");
  if (!tmp) return false;

  uint8_t keptCount = 0;
  uint8_t removed = 0;
  bool ok = true;

  for (int i = 0; i < recordCount; i++) {
    if (strcasecmp(records[i].label, label) == 0) {
      removed++;
      continue;
    }

    if (tmp.write((const uint8_t *)&records[i],
                  sizeof(KnowledgeRecord)) !=
        sizeof(KnowledgeRecord)) {
      ok = false;
      break;
    }

    keptCount++;
  }

  tmp.flush();
  tmp.close();

  if (!ok || !removed) {
    LittleFS.remove(KNOWLEDGE_TMP);
    return false;
  }

  bool hadOriginal = LittleFS.exists(KNOWLEDGE_FILE);

  if (hadOriginal &&
      !LittleFS.rename(KNOWLEDGE_FILE, KNOWLEDGE_BAK)) {
    LittleFS.remove(KNOWLEDGE_TMP);
    return false;
  }

  if (!LittleFS.rename(KNOWLEDGE_TMP, KNOWLEDGE_FILE)) {
    if (hadOriginal &&
        LittleFS.exists(KNOWLEDGE_BAK)) {
      LittleFS.rename(KNOWLEDGE_BAK, KNOWLEDGE_FILE);
    }

    LittleFS.remove(KNOWLEDGE_TMP);
    return false;
  }

  // Kompakkan data RAM setelah file baru berhasil dipasang.
  uint8_t dst = 0;

  for (int i = 0; i < recordCount; i++) {
    if (strcasecmp(records[i].label, label) != 0) {
      if (dst != i)
        records[dst] = records[i];
      dst++;
    }
  }

  recordCount = dst;

  if (LittleFS.exists(KNOWLEDGE_BAK))
    LittleFS.remove(KNOWLEDGE_BAK);

  Serial.printf("TARS: LABEL '%s' DIHAPUS, %u SAMPEL\n",
                label, removed);
  return true;
}
bool knowledgeHasLabel(const char *label) {
  if (!ready || !records || !validLabel(label))
    return false;

  for (int i = 0; i < recordCount; i++) {
    if (records[i].magic == MAGIC_MARK &&
        strcasecmp(records[i].label, label) == 0) {
      return true;
    }
  }

  return false;
}
