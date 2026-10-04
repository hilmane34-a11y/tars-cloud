#include "personality.h"

static PersonalityState p={85,0,60,0,0};
static uint32_t lastTick=0,lastSpeech=0;
static bool resting=false;

void personalityBegin(){
  p={85,0,60,0,0};
  lastTick=lastSpeech=millis();
  resting=false;
}

void personalityUpdate(bool busy,bool explored,bool spoke){
  uint32_t now=millis();
  if(now-lastTick<1000)return;

  float dt=min(1.0f,(now-lastTick)/1000.0f);
  lastTick=now;
  if(busy)return;

  if(resting){
    p.energy=min(100.0f,p.energy+0.8f*dt);
    p.fatigue=max(0.0f,p.fatigue-1.2f*dt);
    p.boredom=min(100.0f,p.boredom+0.1f*dt);

    if(p.energy>=85 && p.fatigue<=15)
      resting=false;
  }else{
    if(explored){
      // Aktivitas eksplorasi berlangsung bertahap
      p.energy=max(0.0f,p.energy-0.04f*dt);
      p.fatigue=min(100.0f,p.fatigue+0.08f*dt);
      p.curiosity=min(100.0f,p.curiosity+0.03f*dt);
      p.boredom=max(0.0f,p.boredom-0.12f*dt);
    }else{
      p.boredom=min(100.0f,p.boredom+0.30f*dt);
      p.curiosity=min(100.0f,p.curiosity+0.08f*dt);
    }

    if(spoke){
      p.boredom=max(0.0f,p.boredom-15.0f);
      p.energy=max(0.0f,p.energy-1.0f);
    }
  }

  if(p.energy<20 || p.fatigue>80)p.mood=2;
  else if(p.boredom>65)p.mood=1;
  else if(p.curiosity>75)p.mood=3;
  else p.mood=0;
}

PersonalityState personalityGet(){
  return p;
}

bool personalityCanExplore(){
  return !resting && p.energy>30 &&
         p.fatigue<70 && p.curiosity>40;
}

bool personalityNeedsRest(){
  return p.energy<=30 || p.fatigue>=70;
}

bool personalityIsResting(){
  return resting;
}

void personalityStartRest(){
  resting=true;
}

void personalityStopRest(){
  resting=false;
}

bool personalityWantsSpeak(){
  return !resting && p.boredom>70 && p.energy>20 &&
         millis()-lastSpeech>120000;
}

void personalitySpeechDone(){
  lastSpeech=millis();
  p.boredom=max(0.0f,p.boredom-20.0f);
  p.energy=max(0.0f,p.energy-1.0f);
}
