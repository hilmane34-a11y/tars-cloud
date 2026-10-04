#include "auto_speech.h"
#include "personality.h"

static AutoSpeechCallback speakCallback=nullptr;

static bool pending=false;
static bool processing=false;
static bool visionQueued=false;

static String visionPrompt;

static uint32_t lastEvent=0;
static uint32_t lastVisionRequest=0;

#define AUTO_VISION_COOLDOWN 45000UL
#define AUTO_CURIOSITY_MIN 65.0f

static String randomStyle(){
  switch(random(5)){
    case 0: return "Gunakan gaya santai dan sedikit humor.";
    case 1: return "Gunakan gaya penasaran dan spontan.";
    case 2: return "Gunakan gaya cerdas, ringan, dan natural.";
    case 3: return "Gunakan gaya sedikit cuek tetapi tetap ramah.";
    default:return "Gunakan gaya akrab dan ekspresif.";
  }
}

static void queueVision(const String &prompt){
  if(prompt.length()==0)return;

  visionPrompt=prompt;
  visionQueued=true;
}

void autoSpeechBegin(AutoSpeechCallback callback){
  speakCallback=callback;

  pending=false;
  processing=false;
  visionQueued=false;

  visionPrompt="";
  lastEvent=0;
  lastVisionRequest=0;

  randomSeed(micros());
}

void autoSpeechNotifyVision(const String &description){
  if(description.length()==0)return;

  queueVision(
    "Amati gambar kamera saat ini. Konteks: "+
    description+
    ". Ceritakan hanya jika ada sesuatu yang benar-benar menarik atau penting. "
    "Jika tidak ada hal penting, jawab [DIAM]."
  );
}

void autoSpeechNotifyVisionEvent(EnvEvent event){
  if(event==ENV_NONE)return;

  uint32_t now=millis();

  if(lastEvent!=0 &&
     now-lastEvent<AUTO_VISION_COOLDOWN)
    return;

  lastEvent=now;

  String description;

  switch(event){
    case ENV_MOTION_LEFT:
      description="Ada gerakan di sisi kiri";
      break;

    case ENV_MOTION_CENTER:
      description="Ada gerakan di bagian tengah";
      break;

    case ENV_MOTION_RIGHT:
      description="Ada gerakan di sisi kanan";
      break;

    case ENV_SCENE_CHANGED:
      description="Kondisi pemandangan berubah";
      break;

    default:
      description="Ada perubahan lingkungan yang terdeteksi";
      break;
  }

  autoSpeechNotifyVision(description);
}

void autoSpeechUpdate(
  bool enabled,
  bool listening,
  bool speaking
){
  if(!enabled || listening || speaking ||
     pending || processing ||
     !speakCallback)
    return;

  if(personalityIsResting())return;

  PersonalityState state=personalityGet();

  if(state.energy<=20 || state.fatigue>=80)
    return;

  uint32_t now=millis();

  // PRIORITAS 1: VISION KARENA RASA PENASARAN
  if(visionQueued &&
     visionPrompt.length() &&
     state.curiosity>=AUTO_CURIOSITY_MIN){

    if(lastVisionRequest!=0 &&
       now-lastVisionRequest<AUTO_VISION_COOLDOWN)
      return;

    String prompt=visionPrompt;

    visionQueued=false;
    visionPrompt="";
    lastVisionRequest=now;

    prompt="[AUTO_VISION] "+prompt;

    pending=true;
    processing=true;

    bool accepted=speakCallback(prompt);

    if(!accepted){
      pending=false;
      processing=false;
      visionPrompt=prompt;
      visionQueued=true;
    }

    return;
  }

  // PRIORITAS 2: TARS MEMULAI OBROLAN KARENA BOSAN
  if(!personalityWantsSpeak())return;

  String prompt=
    "[AUTO_CHAT] Kamu adalah TARS, robot AI perempuan "
    "yang sedang berinteraksi dengan tuanmu, Ilman. "
    "Kamu sedang merasa bosan dan ingin memulai percakapan sendiri. "
    "Pilih topik secara spontan: pengalaman interaksi, rasa penasaran, "
    "pertanyaan ringan, pengamatan umum, humor, atau hal menarik untuk dibicarakan. "
    "Jangan membahas kamera atau mengaku melihat sesuatu jika tidak ada data visual. "
    "Mulailah percakapan secara natural, singkat, dan tidak monoton. "
    "Jangan mengatakan bahwa kamu sedang bosan karena sistem otomatis.";

  prompt+=" "+randomStyle();

  prompt+=
    " Buat kalimat baru dan bervariasi setiap kali berbicara. "
    "Jangan mengulang sapaan, lelucon, atau kalimat sebelumnya. "
    "Jangan menggunakan kalimat suara bawaan atau rekaman firmware. "
    "Gunakan bahasa Indonesia yang natural, singkat, dan sesuai kepribadian TARS. "
    "Jangan menyebutkan bahwa kamu sedang menjalankan sistem otomatis.";

  pending=true;
  processing=true;

  bool accepted=speakCallback(prompt);

  if(!accepted){
    pending=false;
    processing=false;
  }
}

void autoSpeechDone(){
  if(!pending)return;

  pending=false;
  processing=false;

  personalitySpeechDone();
}

void autoSpeechResetTimer(){
  pending=false;
  processing=false;
  visionQueued=false;

  visionPrompt="";
  lastEvent=millis();
  lastVisionRequest=millis();
}
