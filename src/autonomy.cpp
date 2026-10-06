#include "autonomy.h"
#include "wheels.h"
#include "personality.h"

#define AUTO_SPEED 220
#define FORWARD_MS 2000
#define TURN_MS 800
#define OBSERVE_MS 30000
#define ENV_TIMEOUT 4000
#define REST_CONFIRM_MS 1500

#define AUTONOMY_START_DELAY 10000

#define SOFT_START_STEP_MS 150
#define SOFT_START_STEP 40

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

static bool preferLeft=true;

static uint32_t autonomyStartAt=0;
static bool autonomyReady=false;

static int16_t currentLeft=0;
static int16_t currentRight=0;
static int16_t targetLeft=0;
static int16_t targetRight=0;
static uint32_t lastSoftStart=0;

static void softStartStop(){
  currentLeft=0;
  currentRight=0;
  targetLeft=0;
  targetRight=0;
  wheelsStop();
}

static void softStartSet(int16_t left,int16_t right){
  targetLeft=left;
  targetRight=right;

  currentLeft=0;
  currentRight=0;

  lastSoftStart=millis();

  wheelsDrive(0,0);

  Serial.printf(
    "TARS MOTOR: START L=%d R=%d\n",
    targetLeft,
    targetRight
  );
}

static bool softStartUpdate(){
  uint32_t now=millis();

  if(currentLeft==targetLeft &&
     currentRight==targetRight)
    return true;

  if(now-lastSoftStart<SOFT_START_STEP_MS)
    return false;

  lastSoftStart=now;

  if(currentLeft<targetLeft)
    currentLeft=min(
      (int16_t)(currentLeft+SOFT_START_STEP),
      targetLeft
    );
  else if(currentLeft>targetLeft)
    currentLeft=max(
      (int16_t)(currentLeft-SOFT_START_STEP),
      targetLeft
    );

  if(currentRight<targetRight)
    currentRight=min(
      (int16_t)(currentRight+SOFT_START_STEP),
      targetRight
    );
  else if(currentRight>targetRight)
    currentRight=max(
      (int16_t)(currentRight-SOFT_START_STEP),
      targetRight
    );

  wheelsDrive(currentLeft,currentRight);

  Serial.printf(
    "TARS MOTOR: SPEED STEP L=%d R=%d\n",
    currentLeft,
    currentRight
  );

  return currentLeft==targetLeft &&
         currentRight==targetRight;
}

void autonomyBegin(){
  env={};
  moving=false;
  moveUntil=0;
  observeUntil=0;
  lastDecision=0;
  lastEnvUpdate=0;
  restSince=0;

  mode=EXPLORE;
  preferLeft=true;

  autonomyStartAt=millis()+AUTONOMY_START_DELAY;
  autonomyReady=false;

  softStartStop();
}

void autonomyStop(){
  softStartStop();
  moving=false;
  moveUntil=0;
}

void autonomySetEnvironment(const EnvState &e){
  env=e;

  if(e.valid)
    lastEnvUpdate=millis();
}

bool autonomyIsMoving(){
  return moving;
}

void autonomyUpdate(bool enabled,bool busy){
  uint32_t now=millis();

  if(!autonomyReady){
    if((int32_t)(now-autonomyStartAt)<0){
      wheelsStop();
      return;
    }

    autonomyReady=true;

    Serial.println(
      "TARS AUTO: START DELAY 10s SELESAI"
    );
  }

  if(!enabled||busy){
    autonomyStop();
    restSince=0;
    mode=EXPLORE;
    return;
  }

  if(!env.valid){
    autonomyStop();
    return;
  }

  if(now-lastEnvUpdate>ENV_TIMEOUT){
    autonomyStop();
    return;
  }

  if(moving){

    softStartUpdate();

    if((int32_t)(now-moveUntil)<0)
      return;

    autonomyStop();

    mode=OBSERVE;
    observeUntil=now+OBSERVE_MS;
    return;
  }

  if(mode==OBSERVE){
    wheelsStop();

    if((int32_t)(now-observeUntil)<0)
      return;

    mode=EXPLORE;
    lastDecision=0;
  }

  if(mode==EXPLORE&&personalityNeedsRest())
    mode=TIRED;

  if(mode==TIRED){
    autonomyStop();
    restSince=0;
    mode=SEEK_REST;
  }

  if(mode==SEEK_REST){
    if(env.leftClear&&
       env.centerClear&&
       env.rightClear&&
       !env.motion){

      if(!restSince)
        restSince=now;

      if(now-restSince>=REST_CONFIRM_MS){
        personalityStartRest();
        mode=RESTING;
      }
    }else{
      restSince=0;
    }

    return;
  }

  if(mode==RESTING){
    wheelsStop();

    if(!personalityIsResting()){
      personalityStopRest();
      mode=RECOVER;
    }

    return;
  }

  if(mode==RECOVER){
    autonomyStop();
    mode=EXPLORE;
    lastDecision=0;
    return;
  }

  if(!personalityCanExplore()){
    autonomyStop();
    return;
  }

  if(now-lastDecision<500)
    return;

  lastDecision=now;

  if(env.centerClear){
    softStartSet(AUTO_SPEED,AUTO_SPEED);

    moveUntil=now+FORWARD_MS;
    moving=true;
    return;
  }

  if(env.leftClear&&env.rightClear){
    if(preferLeft)
      softStartSet(AUTO_SPEED,-AUTO_SPEED);
    else
      softStartSet(-AUTO_SPEED,AUTO_SPEED);

    preferLeft=!preferLeft;
    moveUntil=now+TURN_MS;
    moving=true;
    return;
  }

  if(env.leftClear){
    softStartSet(AUTO_SPEED,-AUTO_SPEED);

    moveUntil=now+TURN_MS;
    moving=true;
    return;
  }

  if(env.rightClear){
    softStartSet(-AUTO_SPEED,AUTO_SPEED);

    moveUntil=now+TURN_MS;
    moving=true;
    return;
  }

  // Buntu: putar di tempat
  if(preferLeft)
    softStartSet(AUTO_SPEED,-AUTO_SPEED);
  else
    softStartSet(-AUTO_SPEED,AUTO_SPEED);

  preferLeft=!preferLeft;
  moveUntil=now+TURN_MS;
  moving=true;
}
