#ifndef HAND_GESTURE_H
#define HAND_GESTURE_H

#include <Arduino.h>

#define HAND_NONE       0
#define HAND_OPEN       1
#define HAND_CLOSED     2
#define HAND_ARM_UP     3
#define HAND_ARM_DOWN   4

int handGestureUpdate(const uint16_t *image, int w, int h);

#endif
