
#include "visual_tracking.h"
#include <wheels.h>
#include <Arduino.h>
#include <stdlib.h>

#define TRACK_CENTER 160
#define TRACK_DEADZONE 35

#define TRACK_MIN_SPEED 210
#define TRACK_MAX_SPEED 255
#define TRACK_ACCEL_STEP 5
#define TRACK_UPDATE_MS 25

#define TRACK_LOST_MS 1200
#define TRACK_RETURN_SPEED 255
#define TRACK_RETURN_MIN 215
#define TRACK_RETURN_STEP 3

static VisualTrackState state = TRACK_IDLE;
static int16_t targetX = TRACK_CENTER;
static uint32_t lastSeen = 0;
static int32_t turnBalance = 0;
static uint32_t lastTrackTick = 0;
static uint32_t lastUpdate = 0;
static int currentSpeed = 0;
static int returnSpeed = TRACK_RETURN_MIN;
static bool targetValid = false;

static int targetSpeedFor(int x) {
  int error = abs(x - TRACK_CENTER);
  if (error <= TRACK_DEADZONE) return 0;

  int speed = TRACK_MIN_SPEED +
    (error - TRACK_DEADZONE) *
    (TRACK_MAX_SPEED - TRACK_MIN_SPEED) /
    (TRACK_CENTER - TRACK_DEADZONE);

  return constrain(speed, TRACK_MIN_SPEED, TRACK_MAX_SPEED);
}

void visualTrackingBegin() {
  state = TRACK_IDLE;
  targetX = TRACK_CENTER;
  lastSeen = 0;
  turnBalance = 0;
  lastTrackTick = millis();
  lastUpdate = 0;
  currentSpeed = 0;
  returnSpeed = TRACK_RETURN_MIN;
  targetValid = false;
  wheelsStop();
}

void visualTrackingTarget(int16_t x, bool valid) {
  uint32_t now = millis();

  if (!valid) {
    visualTrackingLost();
    return;
  }

  x = constrain(x, 0, 319);
  targetX = x;
  lastSeen = now;
  targetValid = true;

  // Jika target ditemukan lagi saat sedang kembali,
  // hentikan proses kembali dan lanjutkan pelacakan.
  if (state == TRACK_RETURNING || state == TRACK_LOST) {
    wheelsStop();
    currentSpeed = 0;
    lastTrackTick = now;
    state = TRACKING;
  } else {
    state = TRACKING;
  }
}

void visualTrackingLost() {
  // Jangan langsung kembali karena satu frame bisa gagal terdeteksi.
  // Berhenti sementara dan tunggu timeout di visualTrackingUpdate().
  targetValid = false;
  wheelsStop();
  currentSpeed = 0;
}

void visualTrackingUpdate(bool active, bool busy) {
  uint32_t now = millis();

  // Saat STT, pemrosesan pertanyaan, atau TTS:
  // TARS harus diam dan tidak menjalankan tracking.
  if (!active || busy) {
    wheelsStop();
    currentSpeed = 0;
    lastTrackTick = now;
    lastUpdate = now;
    return;
  }

  if (now - lastUpdate < TRACK_UPDATE_MS) return;
  lastUpdate = now;

  if (state == TRACKING) {
    // Target hilang: berhenti, lalu tunggu batas timeout.
    if (!targetValid) {
      wheelsStop();
      currentSpeed = 0;

      if (now - lastSeen >= TRACK_LOST_MS) {
        state = TRACK_LOST;
      }
      return;
    }

    int error = targetX - TRACK_CENTER;
    int desired = targetSpeedFor(targetX);

    // Target di tengah: tidak perlu berputar.
    if (desired == 0) {
      wheelsStop();
      currentSpeed = 0;
      lastTrackTick = now;
      return;
    }

    // Perubahan kecepatan bertahap agar putaran tidak terlalu kasar.
    if (currentSpeed < desired) {
      currentSpeed += TRACK_ACCEL_STEP;
      if (currentSpeed > desired) currentSpeed = desired;
    } else if (currentSpeed > desired) {
      currentSpeed -= TRACK_ACCEL_STEP;
      if (currentSpeed < desired) currentSpeed = desired;
    }

    uint32_t dt = now - lastTrackTick;
    if (dt > 50) dt = 50;
    lastTrackTick = now;

    // Hanya berputar di tempat, tidak maju atau mundur.
    if (error < 0) {
      wheelsDrive(-currentSpeed, currentSpeed);
      turnBalance -= dt;
    } else {
      wheelsDrive(currentSpeed, -currentSpeed);
      turnBalance += dt;
    }
    return;
  }

  if (state == TRACK_LOST) {
    wheelsStop();
    currentSpeed = 0;

    // Jika perputaran sebelumnya sangat kecil, tidak perlu kembali.
    if (abs(turnBalance) < 100) {
      turnBalance = 0;
      targetX = TRACK_CENTER;
      state = TRACK_IDLE;
      return;
    }

    returnSpeed = TRACK_RETURN_MIN;
    lastTrackTick = now;
    state = TRACK_RETURNING;
    return;
  }

  if (state == TRACK_RETURNING) {
    if (turnBalance == 0) {
      wheelsStop();
      currentSpeed = 0;
      targetX = TRACK_CENTER;
      targetValid = false;
      state = TRACK_IDLE;
      return;
    }

    if (returnSpeed < TRACK_RETURN_SPEED) {
      returnSpeed += TRACK_RETURN_STEP;
      if (returnSpeed > TRACK_RETURN_SPEED)
        returnSpeed = TRACK_RETURN_SPEED;
    }

    uint32_t dt = now - lastTrackTick;
    if (dt > 50) dt = 50;
    lastTrackTick = now;

    // Mengimbangi perkiraan durasi putaran sebelumnya.
    if (turnBalance < 0) {
      wheelsDrive(returnSpeed, -returnSpeed);
      turnBalance += dt;
      if (turnBalance > 0) turnBalance = 0;
    } else {
      wheelsDrive(-returnSpeed, returnSpeed);
      turnBalance -= dt;
      if (turnBalance < 0) turnBalance = 0;
    }
    return;
  }

  wheelsStop();
}

VisualTrackState visualTrackingState() {
  return state;
}

bool visualTrackingIsMoving() {
  return state == TRACKING || state == TRACK_RETURNING;
}
