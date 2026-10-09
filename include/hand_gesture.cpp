#include "hand_gesture.h"
#include <Arduino.h>
#include <stdlib.h>

#define STABLE_COUNT 3
#define ARM_STABLE_COUNT 3
#define ARM_MOVE_PIXELS 3
#define MIN_SKIN_PIXELS 8
#define MAX_TRACK_WIDTH 128

static int lastGesture = HAND_NONE;
static int stableGesture = HAND_NONE;
static int stableCount = 0;

static int lastCenterY = -1;
static int armCandidate = HAND_NONE;
static int armStableCount = 0;

static bool handDetected = false;

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

int handGestureUpdate(const uint16_t *image, int w, int h) {
    if (!image || w <= 0 || h <= 0)
        return HAND_NONE;

    // Batasi jumlah kolom agar tetap hemat RAM.
    int sx = (w + MAX_TRACK_WIDTH - 1) / MAX_TRACK_WIDTH;
    if (sx < 1) sx = 1;

    int cols = (w + sx - 1) / sx;
    int top[MAX_TRACK_WIDTH];
    int bottom[MAX_TRACK_WIDTH];
    int colCount[MAX_TRACK_WIDTH] = {0};

    for (int i = 0; i < cols; i++) {
        top[i] = h;
        bottom[i] = -1;
    }

    int minX = w, maxX = -1;
    int minY = h, maxY = -1;
    int pixels = 0;
    long sumX = 0, sumY = 0;

    // Analisis warna langsung dari frame RGB565.
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
        }
    }

    // Tangan belum terlihat: motor tidak boleh menerima
    // perintah baru dari hasil deteksi ini.
    if (pixels < MIN_SKIN_PIXELS ||
        maxX < minX || maxY < minY ||
        maxX - minX < 2 || maxY - minY < 2) {
        lastCenterY = -1;
        armCandidate = HAND_NONE;
        armStableCount = 0;
        handDetected = false;
        lastGesture = HAND_NONE;
        stableGesture = HAND_NONE;
        stableCount = 0;
        return HAND_NONE;
    }

    int centerY = sumY / pixels;
    int boxW = maxX - minX + 1;
    int boxH = maxY - minY + 1;

    // Gerakan lengan hanya diperiksa jika objek tangan
    // sudah terdeteksi pada frame sebelumnya.
    if (handDetected && lastCenterY >= 0) {
        int deltaY = centerY - lastCenterY;
        int armRaw = HAND_NONE;

        if (deltaY <= -ARM_MOVE_PIXELS)
            armRaw = HAND_ARM_UP;
        else if (deltaY >= ARM_MOVE_PIXELS)
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
                lastGesture = HAND_NONE;
                stableCount = 0;
                return armRaw;
            }
        } else {
            armCandidate = HAND_NONE;
            armStableCount = 0;
            // Redam getaran posisi kecil.
            lastCenterY = (lastCenterY * 3 + centerY) / 4;
        }
    } else {
        lastCenterY = centerY;
    }

    handDetected = true;

    // Cari ujung-ujung jari dari kontur atas objek.
    // Ini pendekatan bentuk sederhana, bukan pengenalan AI.
    int peaks = 0;

    for (int c = 1; c < cols - 1; c++) {
        if (colCount[c] == 0 ||
            top[c] >= h ||
            top[c] > minY + boxH * 55 / 100)
            continue;

        if (top[c] <= top[c - 1] &&
            top[c] <= top[c + 1] &&
            (top[c - 1] - top[c] >= 1 ||
             top[c + 1] - top[c] >= 1)) {
            peaks++;
        }
    }

    int raw = HAND_CLOSED;

    // Telapak terbuka cenderung memiliki beberapa tonjolan
    // jari dan lebar yang relatif besar.
    if (peaks >= 2 && boxW * 10 >= boxH * 7)
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
