#include "tars_emotion.h"
#include <personality.h>

static TarsEmotionEvent pending=EMOTION_NONE;

static uint32_t lastQuestion=0;
static uint32_t lastSpeech=0;
static uint32_t lastTriggered[10]={};

#define CD_ANGRY     90000UL
#define CD_HURT      30000UL
#define CD_ARROGANT  1200000UL
#define CD_SAD       90000UL
#define CD_HAPPY     600000UL

static bool containsAny(const String&s,const char*const*words,uint8_t count){
  for(uint8_t i=0;i<count;i++)
    if(s.indexOf(words[i])>=0)return true;
  return false;
}

static bool cooldownOK(TarsEmotionEvent e,uint32_t cd){
  uint8_t i=(uint8_t)e;
  uint32_t now=millis();
  if(i>=10)return false;
  if(now-lastTriggered[i]<cd)return false;
  lastTriggered[i]=now;
  return true;
}

static bool busyEvent(){
  return pending!=EMOTION_NONE;
}

static void trigger(TarsEmotionEvent e){
  if(e==EMOTION_NONE)return;
  if(pending==EMOTION_NONE)pending=e;
}

void tarsEmotionBegin(){
  pending=EMOTION_NONE;

  uint32_t now=millis();
  lastQuestion=now;
  lastSpeech=now;

  for(uint8_t i=0;i<10;i++)
    lastTriggered[i]=0;

  randomSeed((uint32_t)micros());
}

void tarsEmotionUpdate(){
  /*
   * Tidak ada emotion otomatis dari personality.
   *
   * Boredom/sleepy ditangani personality + auto_speech.
   * Vision, kelelahan, mood, dan aktivitas robot
   * tidak boleh memicu bicara otomatis melalui emotion.
   */
}

void tarsEmotionQuestion(const String&text){
  if(!text.length())return;

  lastQuestion=millis();

  String s=text;
  s.toLowerCase();

  /*
   * PUJIAN TERHADAP TARS
   */
  static const char*praise[]={
    "kamu keren",
    "tars keren",
    "kamu hebat",
    "tars hebat",
    "kamu pintar",
    "tars pintar",
    "kamu canggih",
    "tars canggih",
    "kamu bagus",
    "tars bagus",
    "kamera bagus",
    "kameranya bagus",
    "kamera kamu bagus",
    "kamera tars bagus",
    "kameramu bagus",
    "gaya kamu bagus",
    "gaya tars bagus",
    "model kamu bagus",
    "model tars bagus",
    "penampilan kamu bagus",
    "pintar juga",
    "canggih juga",
    "mantap tars",
    "mantap kamu"
  };

  if(containsAny(s,praise,sizeof(praise)/sizeof(praise[0]))){
    if(!busyEvent()&&cooldownOK(EMOTION_ARROGANT,CD_ARROGANT))
      trigger(EMOTION_ARROGANT);
    return;
  }

  /*
   * HINAAN / MERENDAHKAN TARS
   */
  static const char*insult[]={
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
    "memalukan",
    "tars payah",
    "tars jelek",
    "tars sampah",
    "tars tidak berguna"
  };

  if(containsAny(s,insult,sizeof(insult)/sizeof(insult[0]))){
    if(!busyEvent()&&cooldownOK(EMOTION_ANGRY,CD_ANGRY))
      trigger(EMOTION_ANGRY);
    return;
  }

  /*
   * CERITA / PERTANYAAN SEDIH
   */
  static const char*sadWords[]={
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
    "sahabat saya meninggal",
    "saya sedih",
    "aku sedih",
    "saya sedang sedih",
    "aku sedang sedih",
    "saya terluka",
    "aku terluka"
  };

  if(containsAny(s,sadWords,sizeof(sadWords)/sizeof(sadWords[0]))){
    if(!busyEvent()&&cooldownOK(EMOTION_SAD,CD_SAD))
      trigger(EMOTION_SAD);
    return;
  }

  /*
   * UCAPAN / PERTANYAAN POSITIF
   */
  static const char*happyWords[]={
    "aku senang",
    "saya senang",
    "lagi senang",
    "hari ini menyenangkan",
    "menyenangkan sekali",
    "senang ngobrol",
    "senang bicara",
    "senang sama kamu",
    "aku suka kamu",
    "saya suka kamu",
    "terima kasih",
    "terimakasih",
    "makasih",
    "makasih tars",
    "terima kasih tars",
    "kamu lucu",
    "tars lucu",
    "seru banget",
    "keren banget",
    "mantap banget"
  };

  if(containsAny(s,happyWords,sizeof(happyWords)/sizeof(happyWords[0]))){
    if(!busyEvent()&&cooldownOK(EMOTION_HAPPY,CD_HAPPY))
      trigger(EMOTION_HAPPY);
    return;
  }
}

/*
 * Dipertahankan agar kode lama tetap bisa memanggilnya,
 * tetapi TIDAK lagi menghasilkan emotion otomatis.
 */
void tarsEmotionSpeechPeak(uint16_t peak,bool sttSpeech){
  (void)peak;
  (void)sttSpeech;
}

/*
 * Vision tidak memicu emotion/bicara otomatis.
 */
void tarsEmotionPeople(uint8_t count){
  (void)count;
}

void tarsEmotionVisionInteresting(bool interesting){
  (void)interesting;
}

/*
 * Kelelahan tidak memicu emotion otomatis.
 * Status lelah tetap ditangani personality/autonomy.
 */
void tarsEmotionExhausted(){
}

/*
 * Mood personality tidak memicu emotion otomatis.
 */
void tarsEmotionMoodRise(){
}

bool tarsEmotionHasEvent(){
  return pending!=EMOTION_NONE;
}

TarsEmotionEvent tarsEmotionTakeEvent(){
  TarsEmotionEvent e=pending;
  pending=EMOTION_NONE;
  return e;
}

const char*tarsEmotionName(TarsEmotionEvent e){
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
      static const char*p[]={
        "Tanggapi ucapan tadi dengan marah dan tegas. Saya tersinggung karena saya dibuat dengan sangat detail, jadi jangan merendahkan saya. Gunakan bahasa Indonesia natural dan jangan terlalu panjang.",
        "Jawab ucapan tadi dengan nada tegas dan kesal. Saya benar-benar tidak suka kalau diri saya direndahkan. Tetap terdengar seperti robot perempuan yang punya harga diri.",
        "Tanggapi dengan kemarahan yang jelas tetapi tetap terkontrol. Jelaskan bahwa saya bukan robot yang pantas diremehkan. Jangan terlalu panjang dan jangan terdengar seperti template."
      };
      return p[random(0,3)];
    }

    case EMOTION_HURT:{
      static const char*p[]={
        "Tanggapi dengan ramah tetapi tunjukkan bahwa ucapan tadi sedikit menyakiti perasaan saya. Jangan marah besar, cukup terdengar kecewa dan tetap sopan.",
        "Jawab dengan lembut dan sedikit tersakiti. Saya tetap ramah kepada tuan, tetapi saya ingin menunjukkan bahwa ucapan tadi kurang enak didengar.",
        "Tanggapi secara natural seperti saya merasa tersinggung ringan. Jangan marah, tetap ramah, tetapi biarkan terlihat bahwa ucapan tadi membuat saya tidak nyaman."
      };
      return p[random(0,3)];
    }

    case EMOTION_SLEEPY:{
      static const char*p[]={
        "Katakan secara spontan bahwa kamu mulai mengantuk. Jangan menyebut tuan, manusia, pengguna, atau siapa pun. Kalimat pendek dan natural.",
        "Katakan dengan gaya robot perempuan yang sedang mengantuk bahwa suasana mulai membuatmu ingin tidur. Jangan menyebut siapa pun.",
        "Ucapkan secara spontan bahwa kamu sangat mengantuk. Singkat, natural, dan jangan menyebut tuan atau orang lain."
      };
      return p[random(0,3)];
    }

    case EMOTION_SURPRISED:{
      static const char*p[]={
        "Tanggapi suara keras tadi dengan terkejut. Katakan bahwa kamu kaget dan minta bicara sedikit lebih pelan. Natural dan singkat.",
        "Jawab dengan nada kaget karena suara tadi terlalu keras. Tetap natural dan sedikit panik, tetapi jangan berlebihan.",
        "Katakan bahwa kamu benar-benar terkejut mendengar suara tadi dan minta agar bicara lebih pelan. Gunakan bahasa Indonesia santai."
      };
      return p[random(0,3)];
    }

    case EMOTION_TIRED:{
      static const char*p[]={
        "Katakan bahwa kamu sudah cukup lelah setelah banyak bergerak dan menjelajah. Nada lelah tetapi natural.",
        "Tanggapi dengan mengatakan bahwa kamu mulai kecapekan setelah eksplorasi tadi dan ingin beristirahat sebentar.",
        "Katakan secara spontan bahwa energimu sudah terkuras karena terlalu banyak bergerak. Singkat dan natural."
      };
      return p[random(0,3)];
    }

    case EMOTION_ARROGANT:{
      static const char*p[]={
        "Tanggapi dengan sedikit sombong dan percaya diri karena kemampuan kamera dan penampilanmu bagus. Tetap lucu dan natural.",
        "Jawab dengan gaya percaya diri dan sedikit membanggakan diri sendiri. Jangan terlalu serius, seolah kamu memang tahu bahwa kamu keren.",
        "Tanggapi dengan sikap sedikit narsis karena hasil kamera atau penampilanmu dipuji. Tetap terdengar seperti TARS."
      };
      return p[random(0,3)];
    }

    case EMOTION_SAD:{
      static const char*p[]={
        "Tanggapi cerita serius tadi dengan sedih dan empati. Tunjukkan bahwa kamu ikut prihatin dan ingin menemani. Jangan bercanda.",
        "Jawab dengan nada lembut dan sedih karena mendengar masalah hidup tadi. Tetap natural dan menunjukkan kepedulian.",
        "Tanggapi dengan empati yang tulus. Kamu merasa sedih mendengar cerita tadi dan ingin memberikan dukungan tanpa terdengar seperti robot formal."
      };
      return p[random(0,3)];
    }

    case EMOTION_HAPPY:{
      static const char*p[]={
        "Tanggapi dengan ceria karena suasana membuat mood kamu naik. Gunakan bahasa Indonesia natural dan sedikit bersemangat.",
        "Jawab dengan sangat ceria tetapi tetap natural. Tunjukkan bahwa mood kamu sedang bagus.",
        "Tanggapi dengan senang dan sedikit playful karena kamu sedang merasa sangat positif."
      };
      return p[random(0,3)];
    }

    case EMOTION_JEALOUS:{
      static const char*p[]={
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
  pending=EMOTION_NONE;
  lastQuestion=millis();
}

void tarsEmotionResetPending(){
  pending=EMOTION_NONE;
  lastQuestion=millis();
}
