#include "autonomy.h"
#include "wheels.h"
#include "personality.h"

#define AUTO_SPEED 65
#define FORWARD_MS 350
#define TURN_MS 280
#define ENV_TIMEOUT 1800
#define REST_CONFIRM_MS 5000

enum AutoState { EXPLORE, TIRED, SEEK_REST, RESTING, RECOVER };

static AutoState mode=EXPLORE;
static EnvState env={};
static bool moving=false;
static uint32_t moveUntil=0,lastDecision=0;
static uint32_t lastEnvUpdate=0,restSince=0;
static bool preferLeft=true;

void autonomyBegin(){
  env={};
  moving=false;
  moveUntil=lastDecision=lastEnvUpdate=0;
  restSince=0;
  mode=EXPLORE;
  wheelsStop();
}

void autonomyStop(){
  wheelsStop();
  moving=false;
  moveUntil=0;
}

void autonomySetEnvironment(const EnvState &environment){
  env=environment;
  lastEnvUpdate=millis();
  if(!env.valid)autonomyStop();
}

bool autonomyIsMoving(){return moving;}

void autonomyUpdate(bool enabled,bool busy){
  uint32_t now=millis();

  if(!enabled || busy || !env.valid ||
     now-lastEnvUpdate>ENV_TIMEOUT){
    autonomyStop();
    return;
  }

  if(moving){
    if((int32_t)(now-moveUntil)>=0)autonomyStop();
    else return;
  }

  if(personalityNeedsRest() && mode==EXPLORE){
    mode=TIRED;
    autonomyStop();
  }

  if(mode==TIRED){
    mode=SEEK_REST;
    restSince=0;
  }

  if(mode==SEEK_REST){
    // Hanya istirahat jika seluruh area terlihat cukup jelas.
    if(env.leftClear && env.centerClear && env.rightClear &&
       !env.motion){
      if(!restSince)restSince=now;
      if(now-restSince>=REST_CONFIRM_MS){
        mode=RESTING;
        personalityStartRest();
      }
    }else{
      restSince=0;
      autonomyStop();
    }
    return;
  }

  if(mode==RESTING){
    personalityUpdate(false,false,false);
    if(!personalityIsResting()){
      personalityStopRest();
      mode=RECOVER;
    }
    return;
  }

  if(mode==RECOVER){
    mode=EXPLORE;
    lastDecision=now;
    return;
  }

  if(!personalityCanExplore()){
    autonomyStop();
    return;
  }

  if(now-lastDecision<1200)return;
  lastDecision=now;

  if(env.centerClear){
    wheelsForward(AUTO_SPEED);
    moveUntil=now+FORWARD_MS;
  }else if(env.leftClear && env.rightClear){
    if(preferLeft)wheelsLeft(AUTO_SPEED);
    else wheelsRight(AUTO_SPEED);
    preferLeft=!preferLeft;
    moveUntil=now+TURN_MS;
  }else if(env.leftClear){
    wheelsLeft(AUTO_SPEED);
    moveUntil=now+TURN_MS;
  }else if(env.rightClear){
    wheelsRight(AUTO_SPEED);
    moveUntil=now+TURN_MS;
  }else{
    autonomyStop();
    return;
  }

  moving=true;
}
