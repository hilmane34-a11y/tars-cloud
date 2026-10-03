#include "auto_speech.h"
#include "personality.h"
#include <esp_system.h>

static AutoSpeechCallback speakCallback=nullptr;
static bool pending=false;
static String visionPrompt;
static uint32_t nextSpeechTime=0;

#define AUTO_SPEECH_MIN 180000UL
#define AUTO_SPEECH_MAX 900000UL

static void scheduleNext(){
  nextSpeechTime=millis()+random(AUTO_SPEECH_MIN,AUTO_SPEECH_MAX+1);
}

static String randomStyle(){
  switch(random(0,6)){
    case 0:return "Gunakan gaya santai dan spontan.";
    case 1:return "Gunakan komentar ringan dan sedikit humor.";
    case 2:return "Gunakan gaya penasaran dan natural.";
    case 3:return "Gunakan kalimat singkat, cerdas, dan tidak kaku.";
    case 4:return "Gunakan gaya ramah dengan pilihan kata berbeda.";
    default:return "Gunakan gaya bicara kasual dan sedikit cuek.";
  }
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
    " Pastikan sesuai dengan gambar. "
    "Jika manusia terlihat jelas, sapa secara natural. "
    "Jika ada hewan, objek menarik, atau potensi bahaya yang terlihat jelas, "
    "komentari secara singkat dan relevan. "
    "Jangan menebak identitas, jarak, atau kondisi yang tidak terlihat. "
    "Jika tidak ada hal penting, jawab tepat [DIAM].";
}

void autoSpeechNotifyVisionEvent(EnvEvent event){
  switch(event){
    case ENV_MOTION_LEFT:
      autoSpeechNotifyVision("Terdeteksi perubahan gerakan di sisi kiri gambar.");
      break;
    case ENV_MOTION_CENTER:
      autoSpeechNotifyVision("Terdeteksi perubahan gerakan di tengah gambar.");
      break;
    case ENV_MOTION_RIGHT:
      autoSpeechNotifyVision("Terdeteksi perubahan gerakan di sisi kanan gambar.");
      break;
    case ENV_SCENE_CHANGED:
      autoSpeechNotifyVision("Tampilan lingkungan mengalami perubahan.");
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
      "Bicaralah hanya jika ada sesuatu yang benar-benar menarik atau penting. "
      "Jika manusia atau hewan terlihat jelas, boleh menyapa atau berkomentar. "
      "Jika ada halangan atau potensi bahaya yang terlihat jelas, beri peringatan. "
      "Jika tidak ada hal penting, jawab tepat [DIAM].";
  }

  prompt+=" "+randomStyle();
  prompt+=
    " Buat kalimat baru dan bervariasi setiap kali berbicara. "
    "Jangan mengulang sapaan, lelucon, atau kalimat sebelumnya. "
    "Jangan menggunakan kalimat suara bawaan atau rekaman firmware. "
    "Gunakan bahasa Indonesia yang natural, singkat, dan sesuai kepribadian TARS. "
    "Jangan menyebutkan bahwa kamu sedang menjalankan sistem otomatis.";

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
