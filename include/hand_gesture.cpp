
#include "hand_gesture.h"
#include <Arduino.h>

#define STABLE_COUNT 3
#define ARM_STABLE_COUNT 3
#define ARM_MOVE_THRESHOLD 5
#define MIN_BRIGHT_PERCENT 8
#define MAX_BRIGHT_PERCENT 75

static int lastGesture = HAND_NONE;
static int stableGesture = HAND_NONE;
static int stableCount = 0;

static int lastCenterY = -1;
static int armCandidate = HAND_NONE;
static int armStableCount = 0;
static bool armTracking = false;

int handGestureUpdate(const uint16_t *image, int w, int h)
{
    if (!image || w <= 0 || h <= 0)
        return HAND_NONE;

    int x0 = w / 4;
    int x1 = (w * 3) / 4;
    int y0 = h / 6;
    int y1 = (h * 5) / 6;

    uint32_t bgSum = 0;
    int bgCount = 0;

    for (int y = 0; y < h; y += 2) {
        for (int x = 0; x < w; x += 2) {
            if (x >= x0 && x < x1 && y >= y0 && y < y1)
                continue;

            uint16_t p = image[y * w + x];
            int r = ((p >> 11) & 31) * 255 / 31;
            int g = ((p >> 5) & 63) * 255 / 63;
            int b = (p & 31) * 255 / 31;
            bgSum += (r * 30 + g * 59 + b * 11) / 100;
            bgCount++;
        }
    }

    if (!bgCount)
        return HAND_NONE;

    int bgMean = bgSum / bgCount;
    int threshold = constrain(bgMean + 18, 70, 225);

    int brightPixels = 0;
    int area = 0;
    long sumX = 0;
    long sumY = 0;

    for (int y = y0; y < y1; y += 2) {
        for (int x = x0; x < x1; x += 2) {
            uint16_t p = image[y * w + x];
            int r = ((p >> 11) & 31) * 255 / 31;
            int g = ((p >> 5) & 63) * 255 / 63;
            int b = (p & 31) * 255 / 31;
            int gray = (r * 30 + g * 59 + b * 11) / 100;

            if (gray > threshold) {
                brightPixels++;
                sumX += x;
                sumY += y;
            }
            area++;
        }
    }

    if (!area) return HAND_NONE;

    int percent = brightPixels * 100 / area;

    // Tidak ada objek yang cukup jelas: reset tracking lengan.
    if (percent < MIN_BRIGHT_PERCENT ||
        percent > MAX_BRIGHT_PERCENT ||
        brightPixels < 8) {
        lastCenterY = -1;
        armCandidate = HAND_NONE;
        armStableCount = 0;
        armTracking = false;
        lastGesture = HAND_NONE;
        stableGesture = HAND_NONE;
        stableCount = 0;
        return HAND_NONE;
    }

    int centerY = sumY / brightPixels;

    // Gerakan lengan baru diizinkan setelah posisi awal terbentuk.
    if (!armTracking || lastCenterY < 0) {
        lastCenterY = centerY;
        armTracking = true;
    } else {
        int deltaY = centerY - lastCenterY;
        int armRaw = HAND_NONE;

        if (deltaY <= -ARM_MOVE_THRESHOLD)
            armRaw = HAND_ARM_UP;
        else if (deltaY >= ARM_MOVE_THRESHOLD)
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

            // Perbarui posisi perlahan agar noise kecil tidak
            // dianggap sebagai gerakan lengan.
            lastCenterY = (lastCenterY * 3 + centerY) / 4;
        }
    }

    // Deteksi telapak terbuka / mengepal tetap menggunakan
    // persentase area terang seperti logika sebelumnya.
    int raw = HAND_NONE;

    if (percent >= 28)
        raw = HAND_OPEN;
    else
        raw = HAND_CLOSED;

    if (raw == lastGesture)
        stableCount++;
    else {
        lastGesture = raw;
        stableCount = 1;
    }

    if (stableCount >= STABLE_COUNT && raw != stableGesture) {
        stableGesture = raw;
        stableCount = 0;
        return stableGesture;
    }

    return HAND_NONE;
}
