#include "autonomy.h"
#include "wheels.h"
#include "personality.h"
#include "tars_emotion.h"

#define IDLE_WAIT_MS       1500000UL  // 25 menit
#define AUTO_SPEED         255
#define MOVE_MS            1500UL
#define TURN_MS            700UL
#define OBSERVE_MS         5000UL
#define ENV_TIMEOUT        4000UL

enum AutoState {
  WAIT_IDLE,
  EXPLORE,
  OBSERVE,
  TIRED,
  SEEK_REST,
  RESTING,
  RECOVER
};

static AutoState mode = WAIT_IDLE;
static EnvState env = {};
static bool moving = false;
static bool preferLeft = true;
static bool idleMoveDone = false;

static uint32_t lastUserCommand = 0;
static uint32_t moveUntil = 0;
static uint32_t observeUntil = 0;
static uint32_t lastEnvUpdate = 0;
static uint32_t restSince = 0;

void autonomyBegin() {
  env = {};
  mode = WAIT_IDLE;
  moving = false;
  preferLeft = true;
  idleMoveDone = false;

  uint32_t now = millis();
  lastUserCommand = now;
  moveUntil = 0;
  observeUntil = 0;
  lastEnvUpdate = 0;
  restSince = 0;

  wheelsStop();
}

void autonomyStop() {
  wheelsStop();
  moving = false;
  moveUntil = 0;
}

void autonomySetEnvironment(const EnvState &e) {
  env = e;

  if (e.valid) {
    lastEnvUpdate = millis();
  }
}

bool autonomyIsMoving() {
  return moving;
}
void autonomyNotifyUserCommand() {
  lastUserCommand = millis();
  idleMoveDone = false;
  autonomyStop();
  mode = WAIT_IDLE;
  restSince = 0;
}
void autonomyUpdate(bool enabled, bool busy) {
  uint32_t now = millis();

  // Jangan bergerak ketika TARS sedang memproses pertanyaan,
  // STT, TTS, atau aktivitas lain yang ditandai busy.
  if (!enabled || busy) {
    autonomyStop();
    restSince = 0;
    mode = WAIT_IDLE;
    return;
  }

  // Tunggu data lingkungan kamera yang valid.
  if (!env.valid ||
      lastEnvUpdate == 0 ||
      now - lastEnvUpdate > ENV_TIMEOUT) {
    autonomyStop();
    return;
  }

  // Selesaikan gerakan berdasarkan batas waktu.
  if (moving) {
    if ((int32_t)(now - moveUntil) < 0) {
      return;
    }

    autonomyStop();
    mode = OBSERVE;
    observeUntil = now + OBSERVE_MS;
    return;
  }

  // Amati lingkungan sebelum memutuskan gerakan berikutnya.
  if (mode == OBSERVE) {
    wheelsStop();

    if ((int32_t)(now - observeUntil) < 0) {
      return;
    }

    mode = WAIT_IDLE;
  }

  // Jangan mengganggu mekanisme istirahat personality.
  if (mode == WAIT_IDLE && personalityNeedsRest()) {
    mode = TIRED;
  }

  if (mode == TIRED) {
    autonomyStop();
    restSince = 0;
    tarsEmotionExhausted();
    mode = SEEK_REST;
  }

  if (mode == SEEK_REST) {
    if (env.leftClear &&
        env.centerClear &&
        env.rightClear &&
        !env.motion) {
      if (restSince == 0) {
        restSince = now;
      }

      if (now - restSince >= 1500UL) {
        personalityStartRest();
        mode = RESTING;
      }
    } else {
      restSince = 0;
    }

    return;
  }

  if (mode == RESTING) {
    wheelsStop();

    if (!personalityIsResting()) {
      personalityStopRest();
      mode = RECOVER;
    }

    return;
  }

  if (mode == RECOVER) {
    autonomyStop();
    idleMoveDone = false;
    lastUserCommand = now;
    mode = WAIT_IDLE;
    return;
  }

  if (!personalityCanExplore()) {
    autonomyStop();
    return;
  }

  // Setelah ada perintah pengguna, tunggu 25 menit
  // sebelum satu sesi gerakan singkat.
  if (mode == WAIT_IDLE) {
    if (now - lastUserCommand < IDLE_WAIT_MS) {
      return;
    }

    if (idleMoveDone) {
      return;
    }

    mode = EXPLORE;
  }

  if (mode != EXPLORE) {
    return;
  }

  // Gerakan singkat sesuai kondisi lingkungan.
  if (env.centerClear) {
    wheelsForward(AUTO_SPEED);
    moveUntil = now + MOVE_MS;
    moving = true;
    idleMoveDone = true;
    return;
  }

  if (env.leftClear && env.rightClear) {
    if (preferLeft) {
      wheelsLeft(AUTO_SPEED);
    } else {
      wheelsRight(AUTO_SPEED);
    }

    preferLeft = !preferLeft;
    moveUntil = now + TURN_MS;
    moving = true;
    idleMoveDone = true;
    return;
  }

  if (env.leftClear) {
    wheelsLeft(AUTO_SPEED);
    moveUntil = now + TURN_MS;
    moving = true;
    idleMoveDone = true;
    return;
  }

  if (env.rightClear) {
    wheelsRight(AUTO_SPEED);
    moveUntil = now + TURN_MS;
    moving = true;
    idleMoveDone = true;
    return;
  }

  // Jika semua arah terhalang, jangan terus berputar.
  autonomyStop();
  idleMoveDone = true;
  mode = OBSERVE;
  observeUntil = now + OBSERVE_MS;
}
