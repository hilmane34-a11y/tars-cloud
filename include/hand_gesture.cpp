#include "hand_gesture.h"

#define STABLE_COUNT 3

static int lastGesture = HAND_NONE;
static int stableGesture = HAND_NONE;
static int stableCount = 0;

static int lastCenterY = -1;
static int lastArmGesture = HAND_NONE;

int handGestureUpdate(const uint16_t *image, int w, int h)
{
    if (!image || w <= 0 || h <= 0)
        return HAND_NONE;

    int x0 = w / 4;
    int x1 = (w * 3) / 4;
    int y0 = h / 6;
    int y1 = (h * 5) / 6;

    // ===============================
    // CARI BACKGROUND BRIGHTNESS
    // ===============================

    uint32_t bgSum = 0;
    int bgCount = 0;

    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            if (x >= x0 && x < x1 && y >= y0 && y < y1)
                continue;

            uint16_t p = image[y * w + x];

            uint8_t r = ((p >> 11) & 0x1F) << 3;
            uint8_t g = ((p >> 5) & 0x3F) << 2;
            uint8_t b = (p & 0x1F) << 3;

            uint16_t gray =
                (r * 30 + g * 59 + b * 11) / 100;

            bgSum += gray;
            bgCount++;
        }
    }

    if (bgCount <= 0)
        return HAND_NONE;

    int bgMean = bgSum / bgCount;

    int threshold = bgMean + 12;

    if (threshold > 220)
        threshold = 220;

    // ===============================
    // DETEKSI AREA TANGAN / LENGAN
    // ===============================

    int brightPixels = 0;
    int area = 0;

    long sumX = 0;
    long sumY = 0;

    for (int y = y0; y < y1; y++)
    {
        for (int x = x0; x < x1; x++)
        {
            uint16_t p = image[y * w + x];

            uint8_t r = ((p >> 11) & 0x1F) << 3;
            uint8_t g = ((p >> 5) & 0x3F) << 2;
            uint8_t b = (p & 0x1F) << 3;

            uint16_t gray =
                (r * 30 + g * 59 + b * 11) / 100;

            if (gray > threshold)
            {
                brightPixels++;
                sumX += x;
                sumY += y;
            }

            area++;
        }
    }

    if (area <= 0)
        return HAND_NONE;

    int percent = (brightPixels * 100) / area;

    // Tidak ada objek cukup jelas
    if (percent < 5)
    {
        lastCenterY = -1;
        lastArmGesture = HAND_NONE;
        return HAND_NONE;
    }

    // ===============================
    // TITIK TENGAH TANGAN/LENGAN
    // ===============================

    int centerY = -1;

    if (brightPixels > 0)
        centerY = sumY / brightPixels;

    // ===============================
    // DETEKSI GERAK LENGAN
    // ===============================

    if (lastCenterY >= 0 && centerY >= 0)
    {
        int deltaY = centerY - lastCenterY;

        // Kamera: Y kecil = atas
        // Kamera: Y besar = bawah

        if (deltaY <= -2)
        {
            lastCenterY = centerY;
            lastArmGesture = HAND_ARM_UP;
            return HAND_ARM_UP;
        }

        if (deltaY >= 2)
        {
            lastCenterY = centerY;
            lastArmGesture = HAND_ARM_DOWN;
            return HAND_ARM_DOWN;
        }
    }

    lastCenterY = centerY;

    // ===============================
    // DETEKSI GRIPER
    // ===============================

    int raw = HAND_NONE;

    if (percent >= 28)
        raw = HAND_OPEN;
    else if (percent >= 5)
        raw = HAND_CLOSED;

    // ===============================
    // STABILIZER
    // ===============================

    if (raw == lastGesture)
    {
        stableCount++;
    }
    else
    {
        lastGesture = raw;
        stableCount = 1;
    }

    if (stableCount >= STABLE_COUNT)
    {
        if (raw != stableGesture)
        {
            stableGesture = raw;
            stableCount = 0;
            return stableGesture;
        }
    }

    return HAND_NONE;
}
