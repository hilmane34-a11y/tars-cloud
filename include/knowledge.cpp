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
static const uint8_t MATCH_THRESHOLD = 84;

#define GW 64
#define GH 32
#define GP (GW * GH)

struct __attribute__((packed)) KnowledgeRecord {
  uint32_t magic;
  char label[KNOWLEDGE_LABEL_SIZE];
  uint8_t features[KNOWLEDGE_FEATURE_SIZE];
};

static KnowledgeRecord records[MAX_SAMPLES];
static uint8_t recordCount = 0;
static bool ready = false;

// Buffer sementara di DRAM; foto tidak disimpan ke LittleFS.
static uint8_t objectMask[GP];
static uint16_t floodQueue[GP];

static uint16_t samplePixel(const uint16_t *image, int x, int y) {
  return image[(y * 2) * KNOWLEDGE_WIDTH + x * 2];
}

static void getRGB(uint16_t p, int &r, int &g, int &b) {
  r = ((p >> 11) & 31) * 255 / 31;
  g = ((p >> 5) & 63) * 255 / 63;
  b = (p & 31) * 255 / 31;
}

// Perbandingan warna RGB565 tanpa konversi berulang ke 8-bit.
static int colorDistance(uint16_t a, uint16_t b) {
  int dr = abs((int)((a >> 11) & 31) -
               (int)((b >> 11) & 31)) * 255 / 31;
  int dg = abs((int)((a >> 5) & 63) -
               (int)((b >> 5) & 63)) * 255 / 63;
  int db = abs((int)(a & 31) - (int)(b & 31)) * 255 / 31;

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

  // false: jangan format LittleFS jika gagal mount.
  if (!LittleFS.begin(false)) {
    Serial.println("TARS: LITTLEFS GAGAL; DATA DIPERTAHANKAN");
    return false;
  }

  if (!LittleFS.exists(KNOWLEDGE_FILE)) {
    ready = true;
    Serial.println("TARS: MEMORI OBJEK KOSONG");
    return true;
  }

  File f = LittleFS.open(KNOWLEDGE_FILE, "r");
  if (!f) return false;

  const size_t rs = sizeof(KnowledgeRecord);
  size_t fs = f.size();

  if (fs % rs != 0 || fs / rs > MAX_SAMPLES) {
    f.close();
    Serial.println("TARS: FORMAT TIDAK COCOK; FILE TIDAK DIHAPUS");
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

// Ekstraksi fitur bentuk dari satu wilayah warna yang terhubung.
// Wilayah besar, wilayah yang mencapai tepi, dan wilayah tanpa
// perbedaan batas yang cukup dari latar ditolak.
static bool extractAt(
  const uint16_t *image,
  int sx, int sy,
  uint8_t *features,
  int *outArea = nullptr,
  int *outContrast = nullptr
) {
  if (!image || !features ||
      sx < 1 || sx >= GW - 1 ||
      sy < 1 || sy >= GH - 1)
    return false;

  memset(objectMask, 0, sizeof(objectMask));

  const uint16_t seedColor = samplePixel(image, sx, sy);
  const int COLOR_LIMIT = 42;

  int head = 0;
  int tail = 0;
  int minX = GW, minY = GH;
  int maxX = -1, maxY = -1;
  bool touchesEdge = false;

  const int seed = sy * GW + sx;
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

    if (x == 0 || y == 0 || x == GW - 1 || y == GH - 1)
      touchesEdge = true;

    const int nx[4] = {x - 1, x + 1, x, x};
    const int ny[4] = {y, y, y - 1, y + 1};

    for (int k = 0; k < 4; k++) {
      int xx = nx[k];
      int yy = ny[k];

      if (xx < 0 || xx >= GW || yy < 0 || yy >= GH)
        continue;

      int ni = yy * GW + xx;
      if (objectMask[ni]) continue;

      if (colorDistance(samplePixel(image, xx, yy), seedColor)
          <= COLOR_LIMIT) {
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
      bw < 3 || bh < 3 ||
      bw * bh < 12 ||
      area > GP * 45 / 100)
    return false;

  // Ukur perbedaan warna pada batas objek dengan wilayah di luar.
  // Ini membantu menolak lantai atau latar yang menyatu.
  int boundarySum = 0;
  int boundaryCount = 0;

  for (int y = minY; y <= maxY; y++) {
    for (int x = minX; x <= maxX; x++) {
      if (!objectMask[y * GW + x]) continue;

      const int nx[4] = {x - 1, x + 1, x, x};
      const int ny[4] = {y, y, y - 1, y + 1};

      for (int k = 0; k < 4; k++) {
        int xx = nx[k], yy = ny[k];

        if (xx < 0 || xx >= GW || yy < 0 || yy >= GH)
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
    ? boundarySum / boundaryCount
    : 0;

  // Jika batas objek hampir sama dengan latar, jangan belajar.
  if (contrast < 10)
    return false;

  // Fitur 64 byte: 4 nilai x 16 sel.
  // 1) kepadatan siluet
  // 2) kecerahan
  // 3) variasi warna
  // 4) detail tepi di dalam objek
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

// Belajar: cari kandidat di tengah dan pilih wilayah yang
// punya batas jelas, bentuk memadai, dan dekat pusat gambar.
bool knowledgeBuildFeatures(
  const uint16_t *image,
  uint8_t *features
) {
  if (!image || !features) return false;

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

      // Prioritaskan pusat, batas yang jelas, dan wilayah
      // yang cukup besar tanpa memenuhi sebagian besar frame.
      int score = 1000
                - distance * 20
                + contrast * 3
                + (area < 300 ? area : 300) / 3;

      if (score > bestScore) {
        bestScore = score;
        memcpy(features, candidate, KNOWLEDGE_FEATURE_SIZE);
        found = true;
      }
    }
  }

  return found;
}

// Simpan ulang secara transaksional agar kegagalan tulis tidak
// langsung merusak file memori utama.
static bool writeRecordsTransactional(
  const KnowledgeRecord *list,
  uint8_t count
) {
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

  // Jika file lama ada, pindahkan ke backup dahulu.
  LittleFS.remove(KNOWLEDGE_BAK);

  bool hadOriginal = LittleFS.exists(KNOWLEDGE_FILE);

  if (hadOriginal &&
      !LittleFS.rename(KNOWLEDGE_FILE, KNOWLEDGE_BAK)) {
    LittleFS.remove(KNOWLEDGE_TMP);
    return false;
  }

  if (!LittleFS.rename(KNOWLEDGE_TMP, KNOWLEDGE_FILE)) {
    if (hadOriginal && LittleFS.exists(KNOWLEDGE_BAK))
      LittleFS.rename(KNOWLEDGE_BAK, KNOWLEDGE_FILE);

    LittleFS.remove(KNOWLEDGE_TMP);
    return false;
  }

  // Backup lama baru dibuang setelah file baru berhasil dipasang.
  if (LittleFS.exists(KNOWLEDGE_BAK))
    LittleFS.remove(KNOWLEDGE_BAK);

  return true;
}

bool knowledgeLearnFeatures(
  const char *label,
  const uint8_t *features
) {
  if (!ready || !validLabel(label) || !features ||
      recordCount >= MAX_SAMPLES) {
    Serial.println("TARS: GAGAL BELAJAR; CEK LABEL/MEMORI");
    return false;
  }

  KnowledgeRecord rec = {};
  rec.magic = MAGIC_MARK;
  strncpy(rec.label, label, KNOWLEDGE_LABEL_SIZE - 1);
  memcpy(rec.features, features, KNOWLEDGE_FEATURE_SIZE);

  // Pertahankan semua sampel yang ada, termasuk format lama.
  KnowledgeRecord next[MAX_SAMPLES];

  for (int i = 0; i < recordCount; i++)
    next[i] = records[i];

  next[recordCount] = rec;

  if (!writeRecordsTransactional(next, recordCount + 1)) {
    Serial.println("TARS: PENYIMPANAN GAGAL; CEK LITTLEFS");
    return false;
  }

  records[recordCount++] = rec;

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
  if (!ready || !features || !labelOut || !labelOutSize)
    return false;

  labelOut[0] = '\0';
  if (confidenceOut) *confidenceOut = 0;

  int best = 1000;
  int bestIndex = -1;

  for (int n = 0; n < recordCount; n++) {
    // Fitur versi baru tidak dicampur dengan descriptor lama.
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

// Pengenalan memindai seluruh frame, termasuk area sudut.
// Langkah pemindaian dibuat lebih renggang untuk mengurangi beban CPU.
bool knowledgeRecognize(
  const uint16_t *image,
  char *labelOut,
  size_t labelOutSize,
  uint8_t *confidenceOut
) {
  if (!ready || !image || !labelOut || !labelOutSize)
    return false;

  labelOut[0] = '\0';
  if (confidenceOut) *confidenceOut = 0;

  uint8_t candidate[KNOWLEDGE_FEATURE_SIZE];

  int best = 1000;
  int bestIndex = -1;

  for (int y = 1; y < GH - 1; y += 3) {
    for (int x = 1; x < GW - 1; x += 3) {
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
  if (!ready || !validLabel(label))
    return false;

  // Tulis langsung ke TMP tanpa array kept besar di stack.
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

  if (!ok) {
    LittleFS.remove(KNOWLEDGE_TMP);
    return false;
  }

  if (!removed) {
    LittleFS.remove(KNOWLEDGE_TMP);
    return false;
  }

  LittleFS.remove(KNOWLEDGE_BAK);

  bool hadOriginal = LittleFS.exists(KNOWLEDGE_FILE);

  if (hadOriginal &&
      !LittleFS.rename(KNOWLEDGE_FILE, KNOWLEDGE_BAK)) {
    LittleFS.remove(KNOWLEDGE_TMP);
    return false;
  }

  if (!LittleFS.rename(KNOWLEDGE_TMP, KNOWLEDGE_FILE)) {
    if (hadOriginal && LittleFS.exists(KNOWLEDGE_BAK))
      LittleFS.rename(KNOWLEDGE_BAK, KNOWLEDGE_FILE);

    LittleFS.remove(KNOWLEDGE_TMP);
    return false;
  }

  // Kompakkan rekaman di RAM setelah file baru berhasil dipasang.
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
