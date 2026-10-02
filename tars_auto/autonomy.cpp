#include "autonomy.h"
#include "../tars_roda/wheels.h"
#include "personality.h"

static bool cameraValid=false;
static bool pathClear=false;
static bool moving=false;
static uint32_t moveUntil=0;
static uint32_t lastDecision=0;

void autonomyBegin(){
  autonomyStop();
}

void autonomyStop(){
  wheelsStop();
  moving=false;
  moveUntil=0;
}

void autonomySetSafety(bool valid,bool clear){
  cameraValid=valid;
  pathClear=clear;
  if(!valid || !clear)autonomyStop();
}

bool autonomyIsMoving(){return moving;}

void autonomyUpdate(bool enabled,bool busy){
  uint32_t now=millis();

  if(!enabled || busy || !personalityCanExplore() ||
     !cameraValid || !pathClear){
    autonomyStop();
    return;
  }

  if(moving){
    if((int32_t)(now-moveUntil)>=0)autonomyStop();
    return;
  }

  if(now-lastDecision<5000)return;
  lastDecision=now;

  // Gerak pendek; hanya setelah kamera menyatakan jalur aman.
  wheelsForward(65);
  moving=true;
  moveUntil=now+350;
}
