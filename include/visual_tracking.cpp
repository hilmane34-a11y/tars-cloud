#include "visual_tracking.h"
#include <wheels.h>

#define TRACK_CENTER 160
#define TRACK_DEADZONE 35
#define TRACK_SPEED 150
#define TRACK_LOST_MS 1000
#define TRACK_RETURN_MS 1800

static VisualTrackState state=TRACK_IDLE;
static int16_t targetX=TRACK_CENTER;
static uint32_t lastSeen=0;
static uint32_t returnStart=0;
static int8_t lastDirection=0;

void visualTrackingBegin(){
  state=TRACK_IDLE;
  targetX=TRACK_CENTER;
  lastSeen=0;
  returnStart=0;
  lastDirection=0;
  wheelsStop();
}

void visualTrackingTarget(int16_t x,bool valid){
  if(!valid){
    visualTrackingLost();
    return;
  }

  x=constrain(x,0,319);
  targetX=x;
  lastSeen=millis();

  if(state==TRACK_RETURNING)
    state=TRACKING;

  if(abs(x-TRACK_CENTER)<=TRACK_DEADZONE){
    wheelsStop();
    state=TRACKING;
    lastDirection=0;
    return;
  }

  state=TRACKING;

  if(x<TRACK_CENTER){
    // Putar kiri: kiri mundur, kanan maju
    wheelsDrive(-TRACK_SPEED,TRACK_SPEED);
    lastDirection=-1;
  }else{
    // Putar kanan: kiri maju, kanan mundur
    wheelsDrive(TRACK_SPEED,-TRACK_SPEED);
    lastDirection=1;
  }
}

void visualTrackingLost(){
  if(state!=TRACKING)return;

  wheelsStop();
  state=TRACK_LOST;
}

void visualTrackingUpdate(bool active,bool busy){
  if(!active||busy){
    wheelsStop();
    return;
  }

  uint32_t now=millis();

  if(state==TRACKING){
    if(now-lastSeen>TRACK_LOST_MS){
      wheelsStop();
      state=TRACK_LOST;
      return;
    }

    if(abs(targetX-TRACK_CENTER)<=TRACK_DEADZONE){
      wheelsStop();
      return;
    }

    if(targetX<TRACK_CENTER)
      wheelsDrive(-TRACK_SPEED,TRACK_SPEED);
    else
      wheelsDrive(TRACK_SPEED,-TRACK_SPEED);

    return;
  }

  if(state==TRACK_LOST){
    wheelsStop();
    returnStart=now;
    state=TRACK_RETURNING;
    return;
  }

  if(state==TRACK_RETURNING){
    if(now-returnStart<TRACK_RETURN_MS){

      if(lastDirection<0)
        wheelsDrive(TRACK_SPEED,-TRACK_SPEED);
      else if(lastDirection>0)
        wheelsDrive(-TRACK_SPEED,TRACK_SPEED);
      else
        wheelsStop();

    }else{
      wheelsStop();
      state=TRACK_IDLE;
      targetX=TRACK_CENTER;
      lastDirection=0;
    }

    return;
  }

  wheelsStop();
}

VisualTrackState visualTrackingState(){
  return state;
}

bool visualTrackingIsMoving(){
  return state==TRACKING||state==TRACK_RETURNING;
}
