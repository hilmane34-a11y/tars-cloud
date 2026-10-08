#include "hand_gesture.h"

#define MIN_PIXELS 35
#define STABLE_COUNT 3

static int lastRaw = HAND_NONE;
static int stableGesture = HAND_NONE;
static int stableCount = 0;

int handGestureUpdate(const uint16_t *image, int w, int h)
{
    if (!image || w <= 0 || h <= 0)
        return HAND_NONE;

    // ROI tengah kamera
    int x0 = w / 4;
    int x1 = (w * 3) / 4;
    int y0 = h / 6;
    int y1 = (h * 5) / 6;

    int pixels = 0;
    int total = 0;

    // Cari area terang/kontras yang kemungkinan merupakan tangan
    for (int y = y0; y < y1; y++)
    {
        for (int x = x0; x < x1; x++)
        {
            uint16_t p = image[y * w + x];

            uint8_t r = ((p >> 11) & 0x1F) << 3;
            uint8_t g = ((p >> 5) & 0x3F) << 2;
            uint8_t b = (p & 0x1F) << 3;

            uint16_t gray = (r * 30 + g * 59 + b * 11) / 100;

            total += gray;

            if (gray > 75)
                pixels++;
        }
    }

    int area = (x1 - x0) * (y1 - y0);

    if (area <= 0)
        return HAND_NONE;

    int percent = (pixels * 100) / area;

    int raw = HAND_NONE;

    /*
       Tangan terbuka biasanya menempati area lebih besar
       karena jari-jari menyebar.

       Tangan mengepal lebih padat/kecil.
    */
    if (percent >= 28)
        raw = HAND_OPEN;
    else if (percent >= 12)
        raw = HAND_CLOSED;

    if (raw == lastRaw)
        stableCount++;
    else
    {
        lastRaw = raw;
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
