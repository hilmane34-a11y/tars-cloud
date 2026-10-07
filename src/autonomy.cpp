#include "autonomy.h"
#include "wheels.h"
#include "personality.h"
#include <tars_emotion.h>

#define AUTO_SPEED 255
#define FORWARD_MS 8000
#define TURN_MS 4000
#define OBSERVE_MS 20000
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

static bool preferLeft=true;

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

  wheelsStop();
}

void autonomyStop(){
  wheelsStop();
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
  tarsEmotionExhausted();
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
    wheelsForward(AUTO_SPEED);
    moveUntil=now+FORWARD_MS;
    moving=true;
    return;
  }

  if(env.leftClear&&env.rightClear){
    if(preferLeft)
      wheelsLeft(AUTO_SPEED);
    else
      wheelsRight(AUTO_SPEED);

    preferLeft=!preferLeft;
    moveUntil=now+TURN_MS;
    moving=true;
    return;
  }

  if(env.leftClear){
    wheelsLeft(AUTO_SPEED);
    moveUntil=now+TURN_MS;
    moving=true;
    return;
  }

  if(env.rightClear){
    wheelsRight(AUTO_SPEED);
    moveUntil=now+TURN_MS;
    moving=true;
    return;
  }

  // Buntu: putar di tempat
  if(preferLeft)
    wheelsLeft(AUTO_SPEED);
  else
    wheelsRight(AUTO_SPEED);

  preferLeft=!preferLeft;
  moveUntil=now+TURN_MS;
  moving=true;
}
