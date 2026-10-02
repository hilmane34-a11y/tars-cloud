#include "auto_speech.h"
#include "personality.h"

static AutoSpeechCallback speakCallback=nullptr;
static bool pending=false;
static uint32_t lastAttempt=0;

void autoSpeechBegin(AutoSpeechCallback callback){
  speakCallback=callback;
  pending=false;
  lastAttempt=0;
}

void autoSpeechUpdate(bool enabled,bool listening,bool speaking){
  if(!enabled || listening || speaking || pending ||
     !speakCallback || !personalityWantsSpeak())return;

  if(millis()-lastAttempt<120000)return;
  lastAttempt=millis();
  pending=true;

  if(!speakCallback(
    "Kamu adalah TARS. Buat ucapan spontan singkat dalam bahasa Indonesia "
    "kepada tuan. Natural, sedikit penasaran atau manja, maksimal 20 kata."
  )){
    pending=false;
  }
}

void autoSpeechDone(){
  if(!pending)return;
  pending=false;
  personalitySpeechDone();
}
