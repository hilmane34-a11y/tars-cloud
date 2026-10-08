#include "visual_tracking.h"
#include <wheels.h>

#define TRACK_CENTER 160
#define TRACK_DEADZONE 35
#define TRACK_SPEED 150

#define TRACK_LOST_MS 1000
#define TRACK_RETURN_SPEED 150

static VisualTrackState state=TRACK_IDLE;

static int16_t targetX=TRACK_CENTER;
static uint32_t lastSeen=0;

static int8_t turnDirection=0;
static int32_t turnBalance=0;

static uint32_t lastTrackTick=0;

void visualTrackingBegin(){
  state=TRACK_IDLE;
  targetX=TRACK_CENTER;
  lastSeen=0;
  turnDirection=0;
  turnBalance=0;
  lastTrackTick=millis();
  wheelsStop();
}

void visualTrackingTarget(int16_t x,bool valid){
  uint32_t now=millis();

  if(!valid){
    visualTrackingLost();
    return;
  }

  x=constrain(x,0,319);
  targetX=x;
  lastSeen=now;

  if(state==TRACK_RETURNING){
    wheelsStop();
    state=TRACKING;
  }

  if(abs(x-TRACK_CENTER)<=TRACK_DEADZONE){
    wheelsStop();
    turnDirection=0;
    return;
  }

  state=TRACKING;

  if(lastTrackTick==0)
    lastTrackTick=now;

  uint32_t dt=now-lastTrackTick;
  if(dt>100)dt=100;
  lastTrackTick=now;

  if(x<TRACK_CENTER){
    // Putar kiri:
    // kiri mundur, kanan maju
    wheelsDrive(-TRACK_SPEED,TRACK_SPEED);
    turnDirection=-1;
    turnBalance-=dt;
  }else{
    // Putar kanan:
    // kiri maju, kanan mundur
    wheelsDrive(TRACK_SPEED,-TRACK_SPEED);
    turnDirection=1;
    turnBalance+=dt;
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

    if(lastTrackTick==0)
      lastTrackTick=now;

    uint32_t dt=now-lastTrackTick;

    if(dt>100)dt=100;

    lastTrackTick=now;

    if(targetX<TRACK_CENTER)
      turnBalance-=dt;
    else
      turnBalance+=dt;

    return;
  }

  if(state==TRACK_LOST){
    wheelsStop();

    // Kalau tidak pernah benar-benar berputar,
    // tidak perlu melakukan return.
    if(abs(turnBalance)<100){
      turnBalance=0;
      targetX=TRACK_CENTER;
      turnDirection=0;
      state=TRACK_IDLE;
      return;
    }

    state=TRACK_RETURNING;
    return;
  }

  if(state==TRACK_RETURNING){

    // turnBalance < 0 = sebelumnya terlalu banyak ke kiri
    // maka sekarang putar kanan.
    if(turnBalance<0){
      wheelsDrive(TRACK_RETURN_SPEED,-TRACK_RETURN_SPEED);

      uint32_t dt=now-lastTrackTick;
      if(dt>100)dt=100;
      turnBalance+=dt;
      lastTrackTick=now;
    }

    // turnBalance > 0 = sebelumnya terlalu banyak ke kanan
    // maka sekarang putar kiri.
    else if(turnBalance>0){
      wheelsDrive(-TRACK_RETURN_SPEED,TRACK_RETURN_SPEED);

      uint32_t dt=now-lastTrackTick;
      if(dt>100)dt=100;
      turnBalance-=dt;
      lastTrackTick=now;
    }

    else{
      wheelsStop();
      targetX=TRACK_CENTER;
      turnDirection=0;
      state=TRACK_IDLE;
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
