#include "hand_gesture.h"

#define STABLE_COUNT 3

static int lastRaw = HAND_NONE;
static int stableGesture = HAND_NONE;
static int stableCount = 0;

int handGestureUpdate(const uint16_t *image, int w, int h)
{
    if (!image || w <= 0 || h <= 0)
        return HAND_NONE;

    int x0 = w / 4;
    int x1 = (w * 3) / 4;
    int y0 = h / 6;
    int y1 = (h * 5) / 6;

    int pixels = 0;
    int area = 0;

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

            if (gray > 75)
                pixels++;

            area++;
        }
    }

    if (area <= 0)
        return HAND_NONE;

    int percent = (pixels * 100) / area;

    int raw = HAND_NONE;

    if (percent >= 28)
        raw = HAND_OPEN;
    else if (percent >= 4)
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
