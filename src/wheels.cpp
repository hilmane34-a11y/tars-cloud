#include "wheels.h"

#define WHEEL_L1 23  // L9110S A-1A
#define WHEEL_L2 16   // L9110S A-2B (TX0)
#define WHEEL_R1 17  // L9110S B-1A
#define WHEEL_R2 19  // L9110S B-2A (RX0)

#define MAX_SPEED 255

static void motorWrite(uint8_t a, uint8_t b, int16_t speed) {
  speed = constrain(speed, -MAX_SPEED, MAX_SPEED);

  analogWrite(a, speed > 0 ? speed : 0);
  analogWrite(b, speed < 0 ? -speed : 0);
}

void wheelsBegin() {
  pinMode(WHEEL_L1, OUTPUT);
  pinMode(WHEEL_L2, OUTPUT);
  pinMode(WHEEL_R1, OUTPUT);
  pinMode(WHEEL_R2, OUTPUT);

  wheelsStop();
}

void wheelsDrive(int16_t left, int16_t right) {
  motorWrite(WHEEL_L1, WHEEL_L2, left);
  motorWrite(WHEEL_R1, WHEEL_R2, right);
}

void wheelsForward(uint8_t speed)  { wheelsDrive(speed, speed); }
void wheelsBackward(uint8_t speed) { wheelsDrive(-speed, -speed); }
void wheelsLeft(uint8_t speed)     { wheelsDrive(speed, -speed); }
void wheelsRight(uint8_t speed)    { wheelsDrive(-speed, speed); }
void wheelsStop()                  { wheelsDrive(0, 0); }
