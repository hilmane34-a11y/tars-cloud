#include "auto_speech.h"
#include "personality.h"

static AutoSpeechCallback speakCallback=nullptr;
static bool pending=false;
static String visionPrompt;
static uint32_t nextSpeechTime=0;

#define AUTO_SPEECH_MIN 180000UL
#define AUTO_SPEECH_MAX 900000UL

static void scheduleNext(){
  nextSpeechTime=millis()+random(AUTO_SPEECH_MIN,AUTO_SPEECH_MAX+1);
}

void autoSpeechBegin(AutoSpeechCallback callback){
  speakCallback=callback;
  pending=false;
  visionPrompt="";
  randomSeed(esp_random());
  scheduleNext();
}

void autoSpeechNotifyVision(const String &description){
  if(pending||!description.length())return;

  visionPrompt=
    "Amati gambar kamera secara langsung. Informasi tambahan: "+
    description+
    " Pastikan informasi tersebut sesuai gambar. "
    "Jika manusia terlihat jelas, sapa dengan ramah. "
    "Jika ada benda atau keadaan menarik yang benar-benar terlihat, "
    "komentari secara singkat. Jangan menebak identitas atau jarak. "
    "Jika tidak ada hal penting, jawab tepat [DIAM].";
}

void autoSpeechNotifyVisionEvent(EnvEvent event){
  switch(event){
    case ENV_MOTION_LEFT:
      autoSpeechNotifyVision("Ada perubahan gerakan di sisi kiri gambar.");
      break;
    case ENV_MOTION_CENTER:
      autoSpeechNotifyVision("Ada perubahan gerakan di bagian tengah gambar.");
      break;
    case ENV_MOTION_RIGHT:
      autoSpeechNotifyVision("Ada perubahan gerakan di sisi kanan gambar.");
      break;
    case ENV_SCENE_CHANGED:
      autoSpeechNotifyVision("Terjadi perubahan tampilan lingkungan.");
      break;
    default:
      break;
  }
}

void autoSpeechUpdate(bool enabled,bool listening,bool speaking){
  if(!enabled||listening||speaking||pending||!speakCallback)return;
  if((int32_t)(millis()-nextSpeechTime)<0)return;

  String prompt=visionPrompt;
  if(!prompt.length()){
    prompt=
      "Amati gambar kamera saat ini. "
      "Jika manusia terlihat jelas, sapa dengan ramah. "
      "Jika ada benda atau keadaan menarik yang benar-benar terlihat, "
      "berikan komentar singkat dan natural. "
      "Jangan menebak identitas, warna, atau jarak. "
      "Jika tidak ada hal penting, jawab tepat [DIAM].";
  }

  pending=true;
  if(speakCallback(prompt)){
    visionPrompt="";
    scheduleNext();
  }else{
    pending=false;
    nextSpeechTime=millis()+30000UL;
  }
}

void autoSpeechDone(){
  if(!pending)return;
  pending=false;
  personalitySpeechDone();
}
