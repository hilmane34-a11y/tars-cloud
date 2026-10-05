#include "autonomy.h"
#include <Arduino.h>
#include "wheels.h"
#include "personality.h"

#define AUTO_SPEED 120
#define FORWARD_MS 3000
#define TURN_MS 800
#define OBSERVE_MS 10000
#define BACKOFF_MS 700
#define ENV_TIMEOUT 4000
#define REST_CONFIRM_MS 1500

enum AutoState {
  EXPLORE,
  OBSERVE,
  TIRED,
  SEEK_REST,
  RESTING,
  RECOVER
};

static AutoState mode=EXPLORE;
static EnvState env={};

static bool moving=false;
static uint32_t moveUntil=0;
static uint32_t observeUntil=0;
static uint32_t lastDecision=0;
static uint32_t lastEnvUpdate=0;
static uint32_t restSince=0;
static uint32_t lastStatus=0;

static bool preferLeft=true;

static const char* stateName(AutoState s){
  switch(s){
    case EXPLORE:   return "EXPLORE";
    case OBSERVE:   return "OBSERVE";
    case TIRED:     return "TIRED";
    case SEEK_REST: return "SEEK_REST";
    case RESTING:   return "RESTING";
    case RECOVER:   return "RECOVER";
  }
  return "UNKNOWN";
}

static void printStatus(){
  PersonalityState p=personalityGet();

  Serial.printf(
    "TARS AUTO | STATE=%s MOVE=%s | ENERGY=%.1f FATIGUE=%.1f CURIOUS=%.1f BOREDOM=%.1f | ENV L=%d C=%d R=%d OBS=%d MOT=%d LV=%d\n",
    stateName(mode),
    moving?"YES":"NO",
    p.energy,
    p.fatigue,
    p.curiosity,
    p.boredom,
    env.leftClear,
    env.centerClear,
    env.rightClear,
    env.obstacle,
    env.motion,
    env.motionLevel
  );
}

static void setMode(AutoState newMode){
  if(mode!=newMode){
    mode=newMode;
    Serial.printf("TARS AUTO: STATE -> %s\n",stateName(mode));
    printStatus();
  }
}

void autonomyBegin(){
  env={};
  moving=false;
  moveUntil=0;
  observeUntil=0;
  lastDecision=0;
  lastEnvUpdate=0;
  restSince=0;
  lastStatus=0;

  mode=EXPLORE;
  preferLeft=true;

  wheelsStop();

  Serial.println("TARS AUTO: BEGIN");
  printStatus();
}

void autonomyStop(){
  wheelsStop();
  moving=false;
  moveUntil=0;

  Serial.println("TARS AUTO: MOTOR STOP");
}

void autonomySetEnvironment(const EnvState &e){
  env=e;

  if(e.valid){
    lastEnvUpdate=millis();

    Serial.printf(
      "TARS ENV: L=%d C=%d R=%d OBS=%d MOT=%d LV=%d COLOR=%d CONF=%d\n",
      env.leftClear,
      env.centerClear,
      env.rightClear,
      env.obstacle,
      env.motion,
      env.motionLevel,
      env.dominantColor,
      env.colorConfidence
    );
  }
}

bool autonomyIsMoving(){
  return moving;
}

void autonomyUpdate(bool enabled,bool busy){
  uint32_t now=millis();

  // Status periodik setiap 5 detik
  if(now-lastStatus>=5000){
    lastStatus=now;
    printStatus();
  }

  if(!enabled||busy){
    if(moving||mode!=EXPLORE)
      Serial.printf(
        "TARS AUTO: STOP reason=%s\n",
        busy?"BUSY":"DISABLED"
      );

    autonomyStop();
    restSince=0;
    setMode(EXPLORE);
    return;
  }

  if(!env.valid){
    autonomyStop();
    Serial.println("TARS AUTO: WAIT ENV");
    return;
  }

  if(now-lastEnvUpdate>ENV_TIMEOUT){
    autonomyStop();
    Serial.println("TARS AUTO: ENV TIMEOUT");
    return;
  }

  if(moving){
    if((int32_t)(now-moveUntil)<0)
      return;

    autonomyStop();

    setMode(OBSERVE);
    observeUntil=now+OBSERVE_MS;

    Serial.printf(
      "TARS AUTO: OBSERVE %lu ms\n",
      (unsigned long)OBSERVE_MS
    );

    return;
  }

  if(mode==OBSERVE){
    wheelsStop();

    // Ada sesuatu di depan / gerakan mendekat
    if(env.obstacle ||
       (env.motion&&!env.centerClear)){

      Serial.println("TARS AUTO: OBSTACLE/MOTION DEPAN -> MUNDUR");

      wheelsBackward(AUTO_SPEED);
      moveUntil=now+BACKOFF_MS;
      moving=true;

      Serial.printf(
        "TARS AUTO: BACKWARD %lu ms\n",
        (unsigned long)BACKOFF_MS
      );

      return;
    }

    // Tetap mengamati sampai waktu observe selesai
    if((int32_t)(now-observeUntil)<0)
      return;

    Serial.println("TARS AUTO: OBSERVE SELESAI");

    setMode(EXPLORE);
    lastDecision=0;
  }

  if(mode==EXPLORE&&personalityNeedsRest()){
    Serial.println("TARS AUTO: PERSONALITY MEMINTA REST");
    setMode(TIRED);
  }

  if(mode==TIRED){
    autonomyStop();
    restSince=0;

    Serial.println("TARS AUTO: TIRED -> CARI TEMPAT REST");

    setMode(SEEK_REST);
  }

  if(mode==SEEK_REST){
    if(env.leftClear&&
       env.centerClear&&
       env.rightClear&&
       !env.motion){

      if(!restSince){
        restSince=now;
        Serial.println("TARS AUTO: TEMPAT REST TERLIHAT AMAN");
      }

      if(now-restSince>=REST_CONFIRM_MS){
        Serial.println("TARS AUTO: MULAI REST");
        personalityStartRest();
        setMode(RESTING);
      }
    }else{
      restSince=0;
    }

    return;
  }

  if(mode==RESTING){
    wheelsStop();

    if(now-lastStatus>=1000){
      lastStatus=now;
      printStatus();
    }

    if(!personalityIsResting()){
      Serial.println("TARS AUTO: REST SELESAI");
      personalityStopRest();
      setMode(RECOVER);
    }

    return;
  }

  if(mode==RECOVER){
    autonomyStop();

    Serial.println("TARS AUTO: RECOVER -> EXPLORE");

    setMode(EXPLORE);
    lastDecision=0;
    return;
  }

  if(!personalityCanExplore()){
    autonomyStop();
    Serial.println("TARS AUTO: EXPLORE DITAHAN PERSONALITY");
    return;
  }

  if(now-lastDecision<500)
    return;

  lastDecision=now;

  if(env.centerClear){
    Serial.println("TARS AUTO: FORWARD");

    wheelsForward(AUTO_SPEED);
    moveUntil=now+FORWARD_MS;
    moving=true;

    Serial.printf(
      "TARS AUTO: FORWARD %lu ms SPEED=%d\n",
      (unsigned long)FORWARD_MS,
      AUTO_SPEED
    );

    return;
  }

  if(env.leftClear&&env.rightClear){
    if(preferLeft){
      Serial.println("TARS AUTO: TURN LEFT");
      wheelsLeft(AUTO_SPEED);
    }else{
      Serial.println("TARS AUTO: TURN RIGHT");
      wheelsRight(AUTO_SPEED);
    }

    preferLeft=!preferLeft;
    moveUntil=now+TURN_MS;
    moving=true;

    Serial.printf(
      "TARS AUTO: TURN %lu ms SPEED=%d\n",
      (unsigned long)TURN_MS,
      AUTO_SPEED
    );

    return;
  }

  if(env.leftClear){
    Serial.println("TARS AUTO: TURN LEFT");

    wheelsLeft(AUTO_SPEED);
    moveUntil=now+TURN_MS;
    moving=true;

    return;
  }

  if(env.rightClear){
    Serial.println("TARS AUTO: TURN RIGHT");

    wheelsRight(AUTO_SPEED);
    moveUntil=now+TURN_MS;
    moving=true;

    return;
  }

  // Buntu: putar di tempat
  Serial.println("TARS AUTO: BUNTU -> PUTAR");

  if(preferLeft){
    Serial.println("TARS AUTO: TURN LEFT");
    wheelsLeft(AUTO_SPEED);
  }else{
    Serial.println("TARS AUTO: TURN RIGHT");
    wheelsRight(AUTO_SPEED);
  }

  preferLeft=!preferLeft;
  moveUntil=now+TURN_MS;
  moving=true;
}
