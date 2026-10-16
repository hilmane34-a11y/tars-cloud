#include "knowledge.h"
#include <LittleFS.h>
#include <string.h>
#include <math.h>

static const char *KNOWLEDGE_FILE = "/knowledge.dat";
static const uint32_t KNOWLEDGE_MAGIC = 0x4B4E4F57;
static const uint8_t MAX_SAMPLES = 48;
static const uint8_t MATCH_THRESHOLD = 90;

struct __attribute__((packed)) KnowledgeRecord {
  uint32_t magic;
  char label[KNOWLEDGE_LABEL_SIZE];
  uint8_t features[KNOWLEDGE_FEATURE_SIZE];
};

static KnowledgeRecord records[MAX_SAMPLES];
static uint8_t recordCount = 0;
static bool ready = false;

bool knowledgeBegin() {
  recordCount = 0;
  ready = false;

  File f = LittleFS.open(KNOWLEDGE_FILE, "r");
  if (f) {
    while (f.available() >= (int)sizeof(KnowledgeRecord) &&
           recordCount < MAX_SAMPLES) {
      KnowledgeRecord r;
      if (f.read((uint8_t *)&r, sizeof(r)) != sizeof(r))
        break;

      if (r.magic != KNOWLEDGE_MAGIC || r.label[0] == '\0')
        break;

      r.label[KNOWLEDGE_LABEL_SIZE - 1] = '\0';
      records[recordCount++] = r;
    }
    f.close();
  }

  ready = true;
  return true;
}

bool knowledgeBuildFeatures(
    const uint16_t *image,
    uint8_t *features) {
  if (!image || !features) return false;

  // 4x4 grid; each cell contains mean R, G, B and edge contrast.
  for (int cy = 0; cy < 4; cy++) {
    for (int cx = 0; cx < 4; cx++) {
      uint32_t sumR = 0, sumG = 0, sumB = 0;
      uint32_t sumEdge = 0;
      uint32_t edgeCount = 0;
      uint32_t count = 0;

      int x0 = cx * KNOWLEDGE_WIDTH / 4;
      int x1 = (cx + 1) * KNOWLEDGE_WIDTH / 4;
      int y0 = cy * KNOWLEDGE_HEIGHT / 4;
      int y1 = (cy + 1) * KNOWLEDGE_HEIGHT / 4;

      for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
          uint16_t p = image[y * KNOWLEDGE_WIDTH + x];

          uint8_t r = ((p >> 11) & 0x1F) * 255 / 31;
          uint8_t g = ((p >> 5) & 0x3F) * 255 / 63;
          uint8_t b = (p & 0x1F) * 255 / 31;

          sumR += r;
          sumG += g;
          sumB += b;
          count++;

          if (x + 1 < x1) {
            uint16_t q = image[y * KNOWLEDGE_WIDTH + x + 1];
            int qr = ((q >> 11) & 0x1F) * 255 / 31;
            int qg = ((q >> 5) & 0x3F) * 255 / 63;
            int qb = (q & 0x1F) * 255 / 31;

            int lum1 = (77 * r + 150 * g + 29 * b) >> 8;
            int lum2 = (77 * qr + 150 * qg + 29 * qb) >> 8;

            sumEdge += abs(lum1 - lum2);
            edgeCount++;
          }
        }
      }

      int i = (cy * 4 + cx) * 4;
      if (!count) return false;

      features[i]     = sumR / count;
      features[i + 1] = sumG / count;
      features[i + 2] = sumB / count;
      features[i + 3] = edgeCount ? sumEdge / edgeCount : 0;
    }
  }

  return true;
}

bool knowledgeLearnFeatures(
    const char *label,
    const uint8_t *features) {
  if (!ready || !label || !features) return false;
  if (recordCount >= MAX_SAMPLES) return false;

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
  if (!knowledgeBuildFeatures(image, features)) return false;
  return knowledgeLearnFeatures(label, features);
}

bool knowledgeRecognizeFeatures(
    const uint8_t *features,
    char *labelOut,
    size_t labelOutSize,
    uint8_t *confidenceOut) {
  if (labelOut && labelOutSize) labelOut[0] = '\0';
  if (confidenceOut) *confidenceOut = 0;

  if (!ready || !features || !labelOut || !labelOutSize ||
      recordCount == 0) return false;

  float bestScore = -1.0f;
  int bestIndex = -1;

  for (int n = 0; n < recordCount; n++) {
    float colorDiff = 0.0f;
    float edgeDiff = 0.0f;

    for (int i = 0; i < KNOWLEDGE_FEATURE_SIZE; i += 4) {
      colorDiff += abs((int)features[i] -
                       (int)records[n].features[i]);
      colorDiff += abs((int)features[i + 1] -
                       (int)records[n].features[i + 1]);
      colorDiff += abs((int)features[i + 2] -
                       (int)records[n].features[i + 2]);

      edgeDiff += abs((int)features[i + 3] -
                      (int)records[n].features[i + 3]);
    }

    colorDiff /= (16.0f * 3.0f * 255.0f);
    edgeDiff  /= (16.0f * 255.0f);

    float score = 100.0f *
        (1.0f - (0.75f * colorDiff + 0.25f * edgeDiff));

    if (score > bestScore) {
      bestScore = score;
      bestIndex = n;
    }
  }

  if (bestIndex < 0) return false;

  uint8_t confidence = (uint8_t)constrain(
      (int)(bestScore + 0.5f), 0, 100);

  if (confidenceOut) *confidenceOut = confidence;
  if (bestScore < MATCH_THRESHOLD) return false;

  strncpy(labelOut, records[bestIndex].label, labelOutSize - 1);
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
    if (strcasecmp(records[i].label, label) == 0) removed++;
  }
  if (!removed) return false;

  const char *tmp = "/knowledge.tmp";
  const char *bak = "/knowledge.bak";

  LittleFS.remove(tmp);
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;

  size_t expected = 0;
  for (int i = 0; i < recordCount; i++) {
    if (strcasecmp(records[i].label, label) == 0) continue;
    expected += sizeof(KnowledgeRecord);
    if (f.write((const uint8_t *)&records[i],
                sizeof(KnowledgeRecord)) != sizeof(KnowledgeRecord)) {
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

  if (hadOriginal && !LittleFS.rename(KNOWLEDGE_FILE, bak)) {
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

  Serial.printf("TARS: LABEL '%s' DIHAPUS, %u sampel tersisa\n",
                label, recordCount);
  return true;
}
