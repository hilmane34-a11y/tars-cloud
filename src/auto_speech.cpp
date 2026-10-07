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
case 0:return "Gunakan gaya santai dan sedikit humor.";
case 1:return "Gunakan gaya penasaran dan spontan.";
case 2:return "Gunakan gaya cerdas, ringan, dan natural.";
case 3:return "Gunakan gaya sedikit cuek tetapi tetap ramah.";
default:return "Gunakan gaya akrab dan ekspresif.";
}
}

static void queueVision(const String &prompt){
if(!prompt.length())return;

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

void autoSpeechNotifyVision(
const String &description
){
if(!description.length())return;

queueVision(
"Amati gambar kamera saat ini. "
"Konteks pengamatan: "+
description+
". Fokus hanya untuk mencari manusia atau hewan. "
"Periksa gambar dengan teliti. "
"Jangan menganggap benda mati, bayangan, "
"perubahan cahaya, perubahan warna, kendaraan, "
"robot, atau noise kamera sebagai manusia atau hewan. "
"Jika tidak terlihat manusia atau hewan dengan jelas, "
"jawab tepat [DIAM]. "
"Jika terlihat manusia atau hewan dengan jelas, "
"jelaskan secara singkat dan natural apa yang terlihat. "
"Jangan mengarang apa yang terlihat."
);
}

void autoSpeechNotifyVisionEvent(
EnvEvent event
){
/*

* Hanya gerakan lokal yang memicu pemeriksaan Vision.
* 
* ENV_SCENE_CHANGED sengaja tidak digunakan karena
* perubahan seluruh pemandangan bisa disebabkan oleh
* cahaya, bayangan, kamera bergeser, atau perubahan
* lingkungan lain yang bukan manusia/hewan.
  */
  if(
  event!=ENV_MOTION_LEFT &&
  event!=ENV_MOTION_CENTER &&
  event!=ENV_MOTION_RIGHT
  )
  return;

uint32_t now=millis();

if(
lastEvent!=0 &&
now-lastEvent<AUTO_VISION_COOLDOWN
)
return;

String description;

switch(event){

case ENV_MOTION_LEFT:
  description=
    "Ada gerakan yang terdeteksi "
    "di sisi kiri kamera.";
  break;

case ENV_MOTION_CENTER:
  description=
    "Ada gerakan yang terdeteksi "
    "tepat di depan kamera.";
  break;

case ENV_MOTION_RIGHT:
  description=
    "Ada gerakan yang terdeteksi "
    "di sisi kanan kamera.";
  break;

default:
  return;

}

lastEvent=now;

autoSpeechNotifyVision(
description
);
}

void autoSpeechUpdate(
bool enabled,
bool listening,
bool speaking
){
if(
!enabled ||
listening ||
speaking ||
pending ||
processing ||
!speakCallback
)
return;

if(tarsEmotionHasPending())
  return;

if(personalityIsResting())
return;

PersonalityState state=
personalityGet();

if(
state.energy<=20 ||
state.fatigue>=80
)
return;

uint32_t now=millis();

/*

* PRIORITAS 1
* RASA PENASARAN TERHADAP LINGKUNGAN
  */
  if(
  visionQueued &&
  visionPrompt.length() &&
  state.curiosity>=AUTO_CURIOSITY_MIN
  ){

if(
  lastVisionRequest!=0 &&
  now-lastVisionRequest<
    AUTO_VISION_COOLDOWN
)
  return;

String prompt=visionPrompt;

visionQueued=false;
visionPrompt="";

lastVisionRequest=now;

prompt=
  "[AUTO_VISION] "+prompt;

pending=true;
processing=true;

bool accepted=
  speakCallback(prompt);

if(!accepted){
  pending=false;
  processing=false;

  visionPrompt=prompt;
  visionQueued=true;
}

return;

}

/*

* PRIORITAS 2
* OBROLAN SPONTAN KARENA TERLALU LAMA DIAM
  */
  if(!personalityWantsSpeak())
  return;

String prompt=
"[AUTO_CHAT] Kamu adalah TARS, "
"robot AI perempuan yang sedang "
"berinteraksi dengan tuanmu, Ilman. "

"Mulailah percakapan secara spontan "
"dan natural. Pilih topik berdasarkan "
"kepribadianmu: rasa penasaran, "
"pengalaman interaksi, pertanyaan ringan, "
"humor, pengamatan umum, atau sesuatu "
"yang menarik untuk dibicarakan. "

"Jangan mengaku melihat sesuatu jika "
"tidak ada data visual. "
"Jangan membahas kamera jika tidak "
"relevan. "

"Jangan mengatakan bahwa kamu sedang "
"bosan atau bahwa percakapan ini "
"dipicu sistem otomatis.";

prompt+=" "+randomStyle();

prompt+=
" Buat kalimat baru dan bervariasi "
"setiap kali berbicara. "
"Jangan mengulang sapaan, lelucon, "
"atau kalimat sebelumnya. "
"Jangan menggunakan kalimat suara "
"bawaan atau rekaman firmware. "
"Gunakan bahasa Indonesia yang natural, "
"singkat, ekspresif, dan sesuai "
"kepribadian TARS. "
"Jangan menyebutkan sistem internal.";

pending=true;
processing=true;

bool accepted=
speakCallback(prompt);

if(!accepted){
pending=false;
processing=false;
}
}

void autoSpeechDone(){

if(!pending)
return;

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
