
#include "hand_gesture.h"
#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#define STABLE_COUNT 3
#define ARM_STABLE_COUNT 2
#define MIN_SKIN_PIXELS 12
#define MIN_MOVING_SKIN 5
#define MAX_TRACK_WIDTH 128

#define MOTION_W 32
#define MOTION_H 24
#define MOTION_THRESHOLD 14
#define LIGHT_CHANGE_TOLERANCE 10

static int lastGesture = HAND_NONE;
static int stableGesture = HAND_NONE;
static int stableCount = 0;

static int lastCenterY = -1;
static int armCandidate = HAND_NONE;
static int armStableCount = 0;
static bool handDetected = false;

static uint8_t previousLight[MOTION_W * MOTION_H] = {};
static bool previousLightValid = false;

static uint8_t motionMap[MOTION_W * MOTION_H] = {};

static uint8_t brightness565(uint16_t p) {
    int r = ((p >> 11) & 31) * 255 / 31;
    int g = ((p >> 5) & 63) * 255 / 63;
    int b = (p & 31) * 255 / 31;
    return (77 * r + 150 * g + 29 * b) >> 8;
}

static bool isSkin(uint16_t p) {
    int r = ((p >> 11) & 31) * 255 / 31;
    int g = ((p >> 5) & 63) * 255 / 63;
    int b = (p & 31) * 255 / 31;

    return r > 45 &&
           g > 20 &&
           b > 8 &&
           r > g &&
           g > b &&
           r - g > 7 &&
           g - b > 3 &&
           r - b > 18;
}

// Ambil sampel kecil tiap sel untuk menghemat RAM.
static void updateMotionMap(const uint16_t *image, int w, int h) {
    uint8_t current[MOTION_W * MOTION_H] = {};
    uint32_t total = 0;

    for (int gy = 0; gy < MOTION_H; gy++) {
        for (int gx = 0; gx < MOTION_W; gx++) {
            int x0 = gx * w / MOTION_W;
            int x1 = (gx + 1) * w / MOTION_W;
            int y0 = gy * h / MOTION_H;
            int y1 = (gy + 1) * h / MOTION_H;

            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;

            int x = (x0 + x1 - 1) / 2;
            int y = (y0 + y1 - 1) / 2;

            uint8_t v = brightness565(image[y * w + x]);
            int i = gy * MOTION_W + gx;

            current[i] = v;
            total += v;
        }
    }

    if (!previousLightValid) {
        memcpy(previousLight, current, sizeof(current));
        memset(motionMap, 0, sizeof(motionMap));
        previousLightValid = true;
        return;
    }

    int oldTotal = 0;
    for (int i = 0; i < MOTION_W * MOTION_H; i++)
        oldTotal += previousLight[i];

    int globalDelta =
        (int)(total / (MOTION_W * MOTION_H)) -
        (oldTotal / (MOTION_W * MOTION_H));

    for (int i = 0; i < MOTION_W * MOTION_H; i++) {
        int delta = (int)current[i] - (int)previousLight[i];

        // Kurangi perubahan yang menyerupai perubahan
        // pencahayaan pada keseluruhan gambar.
        int residual = abs(delta - globalDelta);

        motionMap[i] =
            residual >= MOTION_THRESHOLD &&
            abs(delta) >= LIGHT_CHANGE_TOLERANCE;
    }

    memcpy(previousLight, current, sizeof(current));
}

static bool pixelMoving(int x, int y, int w, int h) {
    int gx = x * MOTION_W / w;
    int gy = y * MOTION_H / h;

    if (gx < 0) gx = 0;
    if (gy < 0) gy = 0;
    if (gx >= MOTION_W) gx = MOTION_W - 1;
    if (gy >= MOTION_H) gy = MOTION_H - 1;

    return motionMap[gy * MOTION_W + gx] != 0;
}

int handGestureUpdate(const uint16_t *image, int w, int h) {
    if (!image || w <= 0 || h <= 0) {
        previousLightValid = false;
        handDetected = false;
        lastCenterY = -1;
        armCandidate = HAND_NONE;
        armStableCount = 0;
        stableCount = 0;
        lastGesture = HAND_NONE;
        stableGesture = HAND_NONE;
        return HAND_NONE;
    }

    updateMotionMap(image, w, h);

    int sx = (w + MAX_TRACK_WIDTH - 1) / MAX_TRACK_WIDTH;
    if (sx < 1) sx = 1;

    int cols = (w + sx - 1) / sx;
    int top[MAX_TRACK_WIDTH];
    int bottom[MAX_TRACK_WIDTH];
    int colCount[MAX_TRACK_WIDTH] = {};

    for (int i = 0; i < cols; i++) {
        top[i] = h;
        bottom[i] = -1;
    }

    int minX = w, maxX = -1;
    int minY = h, maxY = -1;
    int pixels = 0, movingPixels = 0;
    long sumX = 0, sumY = 0;
    long movingSumY = 0;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x += sx) {
            if (!isSkin(image[y * w + x]))
                continue;

            int c = x / sx;
            if (c >= cols) continue;

            pixels++;
            colCount[c]++;

            if (y < top[c]) top[c] = y;
            if (y > bottom[c]) bottom[c] = y;

            minX = min(minX, x);
            maxX = max(maxX, x);
            minY = min(minY, y);
            maxY = max(maxY, y);

            sumX += x;
            sumY += y;

            if (pixelMoving(x, y, w, h)) {
                movingPixels++;
                movingSumY += y;
            }
        }
    }

    if (pixels < MIN_SKIN_PIXELS ||
        maxX < minX || maxY < minY ||
        maxX - minX < 2 || maxY - minY < 2) {
        handDetected = false;
        lastCenterY = -1;
        armCandidate = HAND_NONE;
        armStableCount = 0;
        stableCount = 0;
        lastGesture = HAND_NONE;
        stableGesture = HAND_NONE;
        return HAND_NONE;
    }

    int centerY = sumY / pixels;
    int boxW = maxX - minX + 1;
    int boxH = maxY - minY + 1;

    // Gerakan lengan hanya diterima bila area kulit
    // ikut mengalami perubahan lokal, bukan sekadar
    // titik tengah bergeser karena perubahan warna.
    if (handDetected && lastCenterY >= 0 &&
        movingPixels >= MIN_MOVING_SKIN) {

        int movingCenterY = movingSumY / movingPixels;
        int deltaY = movingCenterY - lastCenterY;
        int threshold = max(3, h / 80);
        int armRaw = HAND_NONE;

        if (deltaY <= -threshold)
            armRaw = HAND_ARM_UP;
        else if (deltaY >= threshold)
            armRaw = HAND_ARM_DOWN;

        if (armRaw != HAND_NONE) {
            if (armCandidate == armRaw)
                armStableCount++;
            else {
                armCandidate = armRaw;
                armStableCount = 1;
            }

            if (armStableCount >= ARM_STABLE_COUNT) {
                lastCenterY = centerY;
                armCandidate = HAND_NONE;
                armStableCount = 0;
                handDetected = true;
                stableCount = 0;
                lastGesture = HAND_NONE;
                return armRaw;
            }
        } else {
            armCandidate = HAND_NONE;
            armStableCount = 0;
        }
    } else {
        armCandidate = HAND_NONE;
        armStableCount = 0;
    }

    lastCenterY = centerY;
    handDetected = true;

    // Cari tonjolan jari pada kontur atas.
    int peaks = 0;
    int lastPeak = -10;
    int prominence = max(2, h / 100);

    for (int c = 1; c < cols - 1; c++) {
        if (colCount[c] == 0 ||
            top[c] >= h ||
            top[c] > minY + boxH * 45 / 100)
            continue;

        int leftRise = top[c - 1] - top[c];
        int rightRise = top[c + 1] - top[c];

        if (leftRise >= prominence &&
            rightRise >= prominence &&
            c - lastPeak >= 2) {
            peaks++;
            lastPeak = c;
        }
    }

    // Lebar bentuk dan jumlah tonjolan harus mendukung
    // dugaan tangan terbuka. Dua tonjolan saja tidak cukup.
    int raw = HAND_CLOSED;

    if (peaks >= 3 && boxW * 10 >= boxH * 5)
        raw = HAND_OPEN;

    if (raw == lastGesture)
        stableCount++;
    else {
        lastGesture = raw;
        stableCount = 1;
    }

    if (stableCount >= STABLE_COUNT &&
        raw != stableGesture) {
        stableGesture = raw;
        stableCount = 0;
        return stableGesture;
    }

    return HAND_NONE;
}
