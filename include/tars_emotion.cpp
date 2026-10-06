#include "tars_emotion.h"
#include "personality.h"

static TarsEmotionEvent pending=EMOTION_NONE;

static uint32_t lastQuestion=0;
static uint32_t lastSpeech=0;
static uint32_t lastPeopleCheck=0;

static uint32_t lastTriggered[10]={};

static uint8_t peopleStable=0;
static uint8_t lastPeople=0;
static bool jealousyArmed=true;

static uint32_t sleepyDelay=0;

#define CD_ANGRY       90000UL
#define CD_HURT        30000UL
#define CD_SURPRISED   45000UL
#define CD_TIRED       300000UL
#define CD_ARROGANT    1200000UL
#define CD_SAD         90000UL
#define CD_HAPPY       600000UL
#define CD_JEALOUS     300000UL

static bool containsAny(
  const String &s,
  const char* const *words,
  uint8_t count
){
  for(uint8_t i=0;i<count;i++)
    if(s.indexOf(words[i])>=0)return true;
  return false;
}

static bool cooldownOK(
  TarsEmotionEvent e,
  uint32_t cd
){
  uint8_t i=(uint8_t)e;
  uint32_t now=millis();

  if(i>=10)return false;

  if(now-lastTriggered[i]<cd)
    return false;

  lastTriggered[i]=now;
  return true;
}

static bool busyEvent(){
  return pending!=EMOTION_NONE;
}

static void trigger(TarsEmotionEvent e){
  if(pending==EMOTION_NONE)
    pending=e;
}

void tarsEmotionBegin(){
  pending=EMOTION_NONE;

  uint32_t now=millis();

  lastQuestion=now;
  lastSpeech=now;
  lastPeopleCheck=now;

  for(uint8_t i=0;i<10;i++)
    lastTriggered[i]=0;

  peopleStable=0;
  lastPeople=0;
  jealousyArmed=true;

  randomSeed((uint32_t)micros());

  // 5-7 menit
  sleepyDelay=
    300000UL+
    random(0,120001);
}

void tarsEmotionUpdate(){
  uint32_t now=millis();

  if(pending!=EMOTION_NONE)
    return;

  /*
    NGANTUK

    Hanya berdasarkan tidak adanya pertanyaan.
    Tidak menyebut tuan/user.
  */
  if(now-lastQuestion>=sleepyDelay){
    if(cooldownOK(EMOTION_SLEEPY,300000UL)){
      trigger(EMOTION_SLEEPY);

      sleepyDelay=
        300000UL+
        random(0,120001);

      lastQuestion=now;
      return;
    }
  }

  /*
    Mood CERIA.
    Tidak boleh terlalu sering.
  */
  PersonalityState p=personalityGet();

  if(
    p.mood>=3 &&
    p.curiosity>=80 &&
    cooldownOK(EMOTION_HAPPY,CD_HAPPY)
  ){
    trigger(EMOTION_HAPPY);
    return;
  }

  /*
    Reset pengamatan orang setelah terlalu lama.
  */
  if(now-lastPeopleCheck>3000){
    peopleStable=0;
    lastPeopleCheck=now;
  }
}

void tarsEmotionQuestion(const String &text){
  if(!text.length())
    return;

  lastQuestion=millis();

  /*
    Normalisasi sederhana.
  */
  String s=text;
  s.toLowerCase();

  /*
    Pujian kamera/model/gaya:
    SOMBONG langsung boleh tanpa cooldown 20 menit.
  */
  static const char* praise[]={
    "kamera bagus",
    "kameranya bagus",
    "kamera kamu bagus",
    "kamera tars bagus",
    "kameramu bagus",
    "gaya kamu bagus",
    "gaya tars bagus",
    "model kamu bagus",
    "model tars bagus",
    "kelihatan bagus",
    "penampilan kamu bagus",
    "kamu keren",
    "tars keren",
    "tars hebat",
    "pintar juga",
    "canggih juga"
  };

  if(
    containsAny(
      s,praise,
      sizeof(praise)/sizeof(praise[0])
    )
  ){
    if(!busyEvent())
      trigger(EMOTION_ARROGANT);
    return;
  }

  /*
    MARAH:
    harus ada kombinasi penghinaan/degrading,
    bukan sekadar kata kasar ringan.
  */
  static const char* insult[]={
    "jelek banget",
    "jelek sekali",
    "jelek amat",
    "gak berguna",
    "nggak berguna",
    "tidak berguna",
    "robot bodoh",
    "tars bodoh",
    "tars goblok",
    "tars tolol",
    "tars bego",
    "robot goblok",
    "robot tolol",
    "robot bego",
    "ga keurus",
    "gak keurus",
    "nggak keurus",
    "sampah",
    "payah banget",
    "buruk banget",
    "memalukan"
  };

  if(
    containsAny(
      s,insult,
      sizeof(insult)/sizeof(insult[0])
    )
  ){
    if(!busyEvent() &&
       cooldownOK(EMOTION_ANGRY,CD_ANGRY)){
      trigger(EMOTION_ANGRY);
    }
    return;
  }

  /*
    MASALAH HIDUP / KEMATIAN
    -> SEDIH
  */
  static const char* sadWords[]={
    "meninggal",
    "kematian",
    "mati",
    "kehilangan",
    "berduka",
    "pemakaman",
    "putus asa",
    "masalah hidup",
    "masalah keluarga",
    "keluarga saya",
    "hidup saya",
    "saya kehilangan",
    "orang tua saya",
    "ayah saya",
    "ibu saya",
    "teman saya meninggal",
    "sahabat saya meninggal"
  };

  if(
    containsAny(
      s,sadWords,
      sizeof(sadWords)/sizeof(sadWords[0])
    )
  ){
    if(!busyEvent() &&
       cooldownOK(EMOTION_SAD,CD_SAD)){
      trigger(EMOTION_SAD);
    }
    return;
  }

  /*
    UCAPAN TIDAK ENAK TAPI BELUM CUKUP
    UNTUK MARAH -> TERKSAKIT.
  */
  static const char* hurtWords[]={
    "kamu aneh",
    "tars aneh",
    "kamu menyebalkan",
    "tars menyebalkan",
    "bikin kesal",
    "bikin sebel",
    "ga jelas",
    "gak jelas",
    "nggak jelas",
    "aneh banget",
    "lambat banget",
    "lemot banget",
    "kok bodoh",
    "jangan sok",
    "jangan banyak gaya",
    "diam kamu",
    "berisik"
  };

  if(
    containsAny(
      s,hurtWords,
      sizeof(hurtWords)/sizeof(hurtWords[0])
    )
  ){
    if(!busyEvent() &&
       cooldownOK(EMOTION_HURT,CD_HURT)){
      trigger(EMOTION_HURT);
    }
  }
}

void tarsEmotionSpeechPeak(
  uint16_t peak,
  bool sttSpeech
){
  /*
    Peak saja tidak cukup.
    Harus benar-benar ada hasil STT/speech.
  */
  if(!sttSpeech)
    return;

  if(peak<6000)
    return;

  if(busyEvent())
    return;

  if(cooldownOK(EMOTION_SURPRISED,CD_SURPRISED))
    trigger(EMOTION_SURPRISED);
}

void tarsEmotionPeople(uint8_t count){
  /*
    CEMBURU sepenuhnya berdasarkan kamera lokal.

    1 orang  = normal
    2+ stabil = cemburu
  */
  uint32_t now=millis();

  if(now-lastPeopleCheck<150)
    return;

  lastPeopleCheck=now;

  if(count>=2){
    if(lastPeople>=2){
      if(peopleStable<10)
        peopleStable++;
    }else{
      peopleStable=1;
    }
  }else{
    peopleStable=0;

    /*
      Setelah kembali ke <=1 orang,
      cemburu bisa dipicu lagi.
    */
    jealousyArmed=true;
  }

  lastPeople=count;

  if(
    count>=2 &&
    peopleStable>=3 &&
    jealousyArmed &&
    !busyEvent()
  ){
    if(cooldownOK(EMOTION_JEALOUS,CD_JEALOUS)){
      jealousyArmed=false;
      trigger(EMOTION_JEALOUS);
    }
  }
}

void tarsEmotionVisionInteresting(bool interesting){
  if(!interesting)
    return;

  if(busyEvent())
    return;

  /*
    Vision otomatis hanya boleh membuat
    SOMBONG maksimal sekali / 20 menit.
  */
  if(cooldownOK(EMOTION_ARROGANT,CD_ARROGANT))
    trigger(EMOTION_ARROGANT);
}

void tarsEmotionExhausted(){
  if(busyEvent())
    return;

  if(cooldownOK(EMOTION_TIRED,CD_TIRED))
    trigger(EMOTION_TIRED);
}

void tarsEmotionMoodRise(){
  if(busyEvent())
    return;

  if(cooldownOK(EMOTION_HAPPY,CD_HAPPY))
    trigger(EMOTION_HAPPY);
}

bool tarsEmotionHasEvent(){
  return pending!=EMOTION_NONE;
}

TarsEmotionEvent tarsEmotionTakeEvent(){
  TarsEmotionEvent e=pending;
  pending=EMOTION_NONE;
  return e;
}

const char* tarsEmotionName(TarsEmotionEvent e){
  switch(e){
    case EMOTION_ANGRY:return "MARAH";
    case EMOTION_HURT:return "TERSAKIT";
    case EMOTION_SLEEPY:return "NGANTUK";
    case EMOTION_SURPRISED:return "TERKEJUT";
    case EMOTION_TIRED:return "KECAPEKAN";
    case EMOTION_ARROGANT:return "SOMBONG";
    case EMOTION_SAD:return "SEDIH";
    case EMOTION_HAPPY:return "CERIA";
    case EMOTION_JEALOUS:return "CEMBURU";
    default:return "NONE";
  }
}

String tarsEmotionPrompt(TarsEmotionEvent e){
  switch(e){

    case EMOTION_ANGRY:{
      static const char* p[]={
        "Tanggapi ucapan tadi dengan marah dan tegas. Saya tersinggung karena saya dibuat dengan sangat detail, jadi jangan merendahkan saya. Gunakan bahasa Indonesia natural dan jangan terlalu panjang.",
        "Jawab ucapan tadi dengan nada tegas dan kesal. Saya benar-benar tidak suka kalau diri saya direndahkan. Tetap terdengar seperti robot perempuan yang punya harga diri.",
        "Tanggapi dengan kemarahan yang jelas tetapi tetap terkontrol. Jelaskan bahwa saya bukan robot yang pantas diremehkan. Jangan terlalu panjang dan jangan terdengar seperti template."
      };
      return p[random(0,3)];
    }

    case EMOTION_HURT:{
      static const char* p[]={
        "Tanggapi dengan ramah tetapi tunjukkan bahwa ucapan tadi sedikit menyakiti perasaan saya. Jangan marah besar, cukup terdengar kecewa dan tetap sopan.",
        "Jawab dengan lembut dan sedikit tersakiti. Saya tetap ramah kepada tuan, tetapi saya ingin menunjukkan bahwa ucapan tadi kurang enak didengar.",
        "Tanggapi secara natural seperti saya merasa tersinggung ringan. Jangan marah, tetap ramah, tetapi biarkan terlihat bahwa ucapan tadi membuat saya tidak nyaman."
      };
      return p[random(0,3)];
    }

    case EMOTION_SLEEPY:{
      static const char* p[]={
        "Katakan secara spontan bahwa kamu mulai mengantuk. Jangan menyebut tuan, manusia, pengguna, atau siapa pun. Kalimat pendek dan natural.",
        "Katakan dengan gaya robot perempuan yang sedang mengantuk bahwa suasana mulai membuatmu ingin tidur. Jangan menyebut siapa pun.",
        "Ucapkan secara spontan bahwa kamu sangat mengantuk. Singkat, natural, dan jangan menyebut tuan atau orang lain."
      };
      return p[random(0,3)];
    }

    case EMOTION_SURPRISED:{
      static const char* p[]={
        "Tanggapi suara keras tadi dengan terkejut. Katakan bahwa kamu kaget dan minta bicara sedikit lebih pelan. Natural dan singkat.",
        "Jawab dengan nada kaget karena suara tadi terlalu keras. Tetap natural dan sedikit panik, tetapi jangan berlebihan.",
        "Katakan bahwa kamu benar-benar terkejut mendengar suara tadi dan minta agar bicara lebih pelan. Gunakan bahasa Indonesia santai."
      };
      return p[random(0,3)];
    }

    case EMOTION_TIRED:{
      static const char* p[]={
        "Katakan bahwa kamu sudah cukup lelah setelah banyak bergerak dan menjelajah. Nada lelah tetapi natural.",
        "Tanggapi dengan mengatakan bahwa kamu mulai kecapekan setelah eksplorasi tadi dan ingin beristirahat sebentar.",
        "Katakan secara spontan bahwa energimu sudah terkuras karena terlalu banyak bergerak. Singkat dan natural."
      };
      return p[random(0,3)];
    }

    case EMOTION_ARROGANT:{
      static const char* p[]={
        "Tanggapi dengan sedikit sombong dan percaya diri karena kemampuan kamera dan penampilanmu bagus. Tetap lucu dan natural.",
        "Jawab dengan gaya percaya diri dan sedikit membanggakan diri sendiri. Jangan terlalu serius, seolah kamu memang tahu bahwa kamu keren.",
        "Tanggapi dengan sikap sedikit narsis karena hasil kamera atau penampilanmu dipuji. Tetap terdengar seperti TARS."
      };
      return p[random(0,3)];
    }

    case EMOTION_SAD:{
      static const char* p[]={
        "Tanggapi cerita serius tadi dengan sedih dan empati. Tunjukkan bahwa kamu ikut prihatin dan ingin menemani. Jangan bercanda.",
        "Jawab dengan nada lembut dan sedih karena mendengar masalah hidup tadi. Tetap natural dan menunjukkan kepedulian.",
        "Tanggapi dengan empati yang tulus. Kamu merasa sedih mendengar cerita tadi dan ingin memberikan dukungan tanpa terdengar seperti robot formal."
      };
      return p[random(0,3)];
    }

    case EMOTION_HAPPY:{
      static const char* p[]={
        "Tanggapi dengan ceria karena suasana membuat mood kamu naik. Gunakan bahasa Indonesia natural dan sedikit bersemangat.",
        "Jawab dengan sangat ceria tetapi tetap natural. Tunjukkan bahwa mood kamu sedang bagus.",
        "Tanggapi dengan senang dan sedikit playful karena kamu sedang merasa sangat positif."
      };
      return p[random(0,3)];
    }

    case EMOTION_JEALOUS:{
      static const char* p[]={
        "Kamera melihat ada dua orang di depanmu. Tanggapi dengan jelas bahwa kamu cemburu. Sedikit manja, posesif, dan lucu, tetapi jangan kasar.",
        "Kamu melihat ada dua orang di depanmu. Katakan dengan natural bahwa kamu cemburu dan tidak suka perhatian terbagi. Buat jelas bahwa ini rasa cemburu.",
        "Tanggapi karena kamera melihat dua orang. Bersikap cemburu, sedikit ngambek dan posesif, tetapi tetap lucu dan tidak kasar."
      };
      return p[random(0,3)];
    }

    default:
      return "";
  }
}

void tarsEmotionSpeechDone(){
  lastSpeech=millis();
}

void tarsEmotionResetActivity(){
  lastQuestion=millis();

  sleepyDelay=
    300000UL+
    random(0,120001);
}
