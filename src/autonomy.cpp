#include "autonomy.h"
#include "wheels.h"
#include "personality.h"

#define AUTO_SPEED 85
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

void autonomySetEnvironment(const EnvState &e){
  env=e;
  lastEnvUpdate=millis();
  if(!env.valid)autonomyStop();
}

bool autonomyIsMoving(){return moving;}

void autonomyUpdate(bool enabled,bool busy){
  uint32_t now=millis();

  if(!enabled||busy||!env.valid||
     now-lastEnvUpdate>ENV_TIMEOUT){
    autonomyStop();
    return;
  }

  if(moving){
    if((int32_t)(now-moveUntil)<0)return;
    autonomyStop();
  }

  if(mode==EXPLORE&&personalityNeedsRest())
    mode=TIRED;

  if(mode==TIRED){
    autonomyStop();
    restSince=0;
    mode=SEEK_REST;
  }

  if(mode==SEEK_REST){
    // Belum ada sensor untuk memastikan lokasi aman.
    // Berhenti di tempat; jangan bergerak mencari lokasi.
    if(env.leftClear&&env.centerClear&&env.rightClear&&!env.motion){
      if(!restSince)restSince=now;
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
    if(!env.valid)return;
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

  if(!personalityCanExplore())return;
  if(now-lastDecision<1200)return;
  lastDecision=now;

  if(env.centerClear){
    wheelsForward(AUTO_SPEED);
    moveUntil=now+FORWARD_MS;
  }else if(env.leftClear&&env.rightClear){
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
