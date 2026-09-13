#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Wire.h>
#include <time.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "TARS_ElevenLabs_MP3.h"

#if __has_include("TARS_Experience_MP3.h")
#include "TARS_Experience_MP3.h"
#define HAS_EXPERIENCE_MP3 1
#else
#define HAS_EXPERIENCE_MP3 0
#endif

#include "config.h"
#include "wifi_manager.h"

#define MIC_PORT I2S_NUM_1
#define MIC_SCK 18
#define MIC_WS 19
#define MIC_SD 34
#define DAC_PIN 26

const uint32_t MIC_RATE=16000;
const uint32_t PLAY_RATE=22050;
const uint32_t REC_MAX=5000;
const uint32_t REC_MIN=700;
const uint32_t SILENCE=600;
const uint32_t LISTEN_MAX=15000;
const uint32_t PREROLL=500;
const uint32_t OLED_DELAY=15;

const int32_t MIC_TH=14000;
const int32_t MIC_SIL=12000;

const size_t BUF=1024;
const size_t DAC_BUF=16384;
const size_t PRE=MIC_RATE*PREROLL/1000;

const char *STT_FILE="/stt.wav";
const char *PLAY_FILE="/tars.mp3";

/*
   DUR = perkiraan durasi suara aktif dalam satuan 10 ms.

   TARS       ~0.70 detik
   JOKOWI     ~1.21 detik
   EXPERIENCE ~1.79 detik
*/
const uint16_t DUR_TARS=70;
const uint16_t DUR_JOKOWI=121;
const uint16_t DUR_EXP=179;

/*
   Wake TARS dibuat sedikit lebih longgar,
   tetapi diberi pagar durasi + margin supaya
   "hidup Jokowi" tidak mudah dianggap TARS.
*/
const float WAKE_TH=.29f;
const float JOKOWI_TH=.27f;
const float EXP_TH=.30f;
const float MARGIN=.030f;

/*
   Rentang durasi aktif yang masih dianggap
   masuk akal untuk kata "TARS".

   45 = 0.45 detik
   105 = 1.05 detik
*/
const uint16_t TARS_MIN_LEN=45;
const uint16_t TARS_MAX_LEN=105;


/* =========================================================
   TEMPLATE — JANGAN DIUBAH
   ========================================================= */

const uint8_t TEMPLATE_TARS[64]={
0,18,105,209,248,253,255,255,255,253,242,229,233,228,225,228,
214,219,233,230,232,231,215,215,226,224,225,228,224,230,237,226,
216,212,209,221,235,222,209,211,192,163,146,102,33,9,29,25,
4,4,4,0,0,5,9,9,9,8,10,11,9,6,5,4};

const uint8_t TEMPLATE_JOKOWI[64]={
0,54,192,227,253,144,35,70,237,245,83,6,0,2,7,12,
115,242,255,227,45,2,0,40,16,85,207,210,203,140,112,120,
125,125,134,134,112,105,93,90,88,80,76,70,65,69,75,74,
79,81,78,77,76,66,61,42,36,42,39,31,23,20,16,3};

const uint8_t TEMPLATE_EXP[64]={
3,17,186,237,255,229,59,150,120,106,41,1,0,2,175,255,
229,58,4,1,30,39,206,249,227,84,57,4,1,30,39,206,249,
227,84,57,41,10,0,0,47,180,73,92,101,197,213,219,215,
211,248,219,201,157,69,72,157,184,162,67,51,65,70,75,95,
132,139,137,119,83,40,30,12};


/* =========================================================
   OLED
   ========================================================= */

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);

bool oledOK=false;
bool micOK=false;
bool dacOK=false;
bool playing=false;
bool singMode=false;
bool ntpOK=false;

String oledText;
size_t oledPos=0;
size_t oledPage=0;

uint32_t oledTick=0;
uint32_t dotTick=0;
uint8_t dots=1;

void oledHeader(const char *s){
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(42,0);
  oled.print("T A R S");
  oled.drawLine(0,9,127,9,SSD1306_WHITE);
  oled.setCursor(3,14);
  oled.print(s);
}

void oledBase(const char *a,const String &b=""){
  if(!oledOK)return;

  oledHeader(a);

  if(b.length()){
    oled.setCursor(3,27);
    oled.print(b);
  }

  oled.display();
}

void oledListen(){
  if(!oledOK||millis()-dotTick<350)return;

  dotTick=millis();
  dots=dots>=4?1:dots+1;

  String s;
  for(byte i=0;i<dots;i++)s+='.';

  oledBase("STANDBY",s);
}

void oledType(){
  if(!oledOK||
     oledPos>=oledText.length()||
     millis()-oledTick<OLED_DELAY)
    return;

  oledTick=millis();

  oledHeader(singMode?"SINGING":"SPEAKING");

  int x=3,y=27;
  size_t i=oledPage;

  while(i<oledPos){
    char c=oledText[i++];

    if(c=='\n'||x>121){
      x=3;
      y+=8;

      if(c=='\n')continue;
    }

    if(y>59){
      oledPage=i-1;
      oledHeader(singMode?"SINGING":"SPEAKING");
      x=3;
      y=27;
      i=oledPage;
    }

    if(y<=59){
      oled.setCursor(x,y);
      oled.write(c);
      x=oled.getCursorX();
    }
  }

  byte n=0;

  while(oledPos<oledText.length()&&n<5){
    char c=oledText[oledPos++];

    if(c=='\n'||x>121){
      x=3;
      y+=8;

      if(c=='\n')continue;
    }

    if(y>59){
      oledPage=oledPos-1;
      oledHeader(singMode?"SINGING":"SPEAKING");
      x=3;
      y=27;
      continue;
    }

    oled.setCursor(x,y);
    oled.write(c);
    x=oled.getCursorX();
    n++;
  }

  oled.display();
}


/* =========================================================
   DAC
   ========================================================= */

bool initDAC(){
  pinMode(DAC_PIN,OUTPUT);
  dacWrite(DAC_PIN,0);
  return true;
}

inline void dacMute(){
  dacWrite(DAC_PIN,0);
}


/* =========================================================
   INMP441
   ========================================================= */

bool initMic(){
  i2s_config_t c={};

  c.mode=(i2s_mode_t)(
    I2S_MODE_MASTER|
    I2S_MODE_RX
  );

  c.sample_rate=MIC_RATE;
  c.bits_per_sample=I2S_BITS_PER_SAMPLE_32BIT;
  c.channel_format=I2S_CHANNEL_FMT_ONLY_RIGHT;
  c.communication_format=I2S_COMM_FORMAT_I2S;

  c.dma_buf_count=2;
  c.dma_buf_len=256;

  if(i2s_driver_install(
    MIC_PORT,
    &c,
    0,
    nullptr
  )!=ESP_OK)
    return false;

  i2s_pin_config_t p={
    I2S_PIN_NO_CHANGE,
    MIC_SCK,
    MIC_WS,
    I2S_PIN_NO_CHANGE,
    MIC_SD
  };

  return i2s_set_pin(MIC_PORT,&p)==ESP_OK;
}


/* =========================================================
   WAV
   ========================================================= */

void put16(uint8_t *p,uint16_t v){
  p[0]=v;
  p[1]=v>>8;
}

void put32(uint8_t *p,uint32_t v){
  p[0]=v;
  p[1]=v>>8;
  p[2]=v>>16;
  p[3]=v>>24;
}

void wavHeader(File &f,uint32_t n){
  uint8_t h[44]={};

  memcpy(h,"RIFF",4);
  put32(h+4,n+36);

  memcpy(h+8,"WAVEfmt ",8);
  put32(h+16,16);

  put16(h+20,1);
  put16(h+22,1);

  put32(h+24,MIC_RATE);
  put32(h+28,MIC_RATE*2);

  put16(h+32,2);
  put16(h+34,16);

  memcpy(h+36,"data",4);
  put32(h+40,n);

  f.seek(0);
  f.write(h,44);
}


/* =========================================================
   ENVELOPE
   ========================================================= */

uint16_t envelope(uint8_t *e,uint16_t max){
  File f=LittleFS.open(STT_FILE);

  if(!f||f.size()<=44){
    if(f)f.close();
    return 0;
  }

  f.seek(44);

  int16_t p[BUF/2];

  uint16_t bins=0;
  uint16_t n=0;

  uint32_t sum=0;

  while(f.available()&&bins<max){

    size_t z=f.read(
      (uint8_t*)p,
      sizeof(p)
    );

    if(!z)break;

    for(size_t i=0;i<z/2;i++){

      sum+=abs((int32_t)p[i]);

      if(++n>=160){

        e[bins++]=min(
          (uint32_t)255,
          sum/n/128
        );

        n=0;
        sum=0;

        if(bins>=max)break;
      }
    }
  }

  if(n&&bins<max){
    e[bins++]=min(
      (uint32_t)255,
      sum/n/128
    );
  }

  f.close();

  return bins;
}


/* =========================================================
   TEMPLATE SCORE
   ========================================================= */

float templateScore(
  const uint8_t *e,
  uint16_t n,
  const uint8_t *t,
  uint16_t dur
){
  if(n<20)return 1;

  uint8_t peak=0;

  for(uint16_t i=0;i<n;i++)
    peak=max(peak,e[i]);

  if(peak<20)return 1;

  uint8_t gate=max(
    (uint8_t)8,
    (uint8_t)(peak*.10f)
  );

  int first=-1;
  int last=-1;

  for(uint16_t i=0;i<n;i++){

    if(e[i]>=gate){

      if(first<0)
        first=i;

      last=i;
    }
  }

  if(first<0||last<=first)
    return 1;

  uint16_t len=last-first+1;

  if(len<20)
    return 1;

  uint8_t lo=255;
  uint8_t hi=0;

  for(uint16_t i=first;i<=last;i++){
    lo=min(lo,e[i]);
    hi=max(hi,e[i]);
  }

  if(hi<=lo+4)
    return 1;

  float shape=0;

  for(byte j=0;j<64;j++){

    float q=
      (float)j*(len-1)/63.0f;

    uint16_t a=q;

    uint16_t b=
      (a+1<len)?
      a+1:
      a;

    float v=
      e[first+a]+
      (e[first+b]-e[first+a])*(q-a);

    v=constrain(
      (v-lo)*255.0f/(hi-lo),
      0.0f,
      255.0f
    );

    shape+=fabsf(v-t[j])/255.0f;
  }

  float ratio=
    (float)len/dur;

  float durationPenalty=
    min(
      fabsf(logf(max(ratio,.05f))),
      1.0f
    );

  return
    shape/64.0f*.72f+
    durationPenalty*.28f;
}


/* =========================================================
   CLASSIFIER
   ========================================================= */

enum InputType{
  INPUT_BLOCKED,
  INPUT_OFFLINE_JOKOWI,
  INPUT_OFFLINE_EXP,
  INPUT_WAKE_TARS
};

InputType classifyInput(){

  uint8_t e[600];

  uint16_t n=
    envelope(e,600);

  if(n<30)
    return INPUT_BLOCKED;

  float s[3]={
    templateScore(
      e,n,
      TEMPLATE_TARS,
      DUR_TARS
    ),

    templateScore(
      e,n,
      TEMPLATE_JOKOWI,
      DUR_JOKOWI
    ),

    templateScore(
      e,n,
      TEMPLATE_EXP,
      DUR_EXP
    )
  };

  /*
     Cari panjang suara aktif khusus
     untuk pengamanan wake TARS.
  */
  uint8_t peak=0;

  for(uint16_t i=0;i<n;i++)
    peak=max(peak,e[i]);

  uint8_t gate=max(
    (uint8_t)8,
    (uint8_t)(peak*.10f)
  );

  int first=-1;
  int last=-1;

  for(uint16_t i=0;i<n;i++){

    if(e[i]>=gate){

      if(first<0)
        first=i;

      last=i;
    }
  }

  uint16_t activeLen=
    (first>=0&&last>=first)?
    last-first+1:
    0;

  Serial.printf(
    "TARS: WAKE %.3f | JOKOWI %.3f | EXP %.3f | LEN %u\r\n",
    s[0],
    s[1],
    s[2],
    activeLen
  );

  byte best=0;

  for(byte i=1;i<3;i++){
    if(s[i]<s[best])
      best=i;
  }

  byte second=best==0?1:0;

  for(byte i=0;i<3;i++){
    if(i!=best&&s[i]<s[second])
      second=i;
  }

  float margin=
    s[second]-s[best];

  /*
     =====================================================
     WAKE TARS
     =====================================================

     Threshold dibuat lebih longgar daripada sebelumnya,
     tetapi HARUS:
       1. TARS menjadi template terbaik
       2. score cukup bagus
       3. margin cukup
       4. durasi aktif masuk rentang TARS

     Jadi "hidup Jokowi" yang jauh lebih panjang
     tidak gampang membuka STT.
  */
  bool tarsDurationOK=
    activeLen>=TARS_MIN_LEN&&
    activeLen<=TARS_MAX_LEN;

  if(
    best==0&&
    s[0]<=WAKE_TH&&
    margin>=MARGIN&&
    tarsDurationOK
  ){
    Serial.printf(
      "TARS: WAKE ACCEPTED | SCORE %.3f | MARGIN %.3f | LEN %u\r\n",
      s[0],
      margin,
      activeLen
    );

    return INPUT_WAKE_TARS;
  }

  /*
     Jokowi tetap offline.
  */
  if(
    best==1&&
    s[1]<=JOKOWI_TH&&
    margin>=MARGIN
  ){
    return INPUT_OFFLINE_JOKOWI;
  }

  /*
     Pengalaman tetap offline.
  */
  if(
    best==2&&
    s[2]<=EXP_TH&&
    margin>=MARGIN
  ){
    return INPUT_OFFLINE_EXP;
  }

  return INPUT_BLOCKED;
}


/* =========================================================
   RECORD MIC
   ========================================================= */

bool recordMic(){

  if(!micOK)
    return false;

  oledBase(
    "STANDBY",
    "LISTENING..."
  );

  LittleFS.remove(STT_FILE);

  File f=
    LittleFS.open(
      STT_FILE,
      FILE_WRITE
    );

  if(!f)
    return false;

  uint8_t zero[44]={};
  f.write(zero,44);

  static int16_t prebuf[PRE];

  size_t pp=0;
  size_t pc=0;

  int32_t raw[BUF/4];
  int16_t pcm[BUF/4];

  uint32_t samples=0;
  uint32_t start=millis();
  uint32_t voiceStart=0;
  uint32_t lastVoice=0;

  byte active=0;
  bool voice=false;

  while(
    (!voice&&millis()-start<LISTEN_MAX)||
    (voice&&millis()-voiceStart<REC_MAX)
  ){

    oledListen();

    size_t n=0;

    if(
      i2s_read(
        MIC_PORT,
        raw,
        sizeof(raw),
        &n,
        pdMS_TO_TICKS(50)
      )!=ESP_OK
    )
      continue;

    size_t c=n/4;

    int32_t peak=0;

    for(size_t i=0;i<c;i++){

      int32_t v=
        constrain(
          raw[i]>>16,
          -32768,
          32767
        );

      pcm[i]=v;

      peak=max(
        peak,
        abs(v)
      );
    }

    if(!voice){

      for(size_t i=0;i<c;i++){

        prebuf[pp]=pcm[i];

        pp=(pp+1)%PRE;

        if(pc<PRE)
          pc++;
      }

      if(
        peak>=MIC_TH&&
        ++active>=2
      ){

        voice=true;

        voiceStart=millis();
        lastVoice=voiceStart;

        size_t st=
          pc==PRE?
          pp:
          0;

        for(size_t i=0;i<pc;i++){

          size_t k=
            (st+i)%PRE;

          f.write(
            (uint8_t*)&prebuf[k],
            2
          );
        }

        samples+=pc;

        f.write(
          (uint8_t*)pcm,
          c*2
        );

        samples+=c;

      }else if(peak<MIC_TH){

        active=0;
      }

    }else{

      f.write(
        (uint8_t*)pcm,
        c*2
      );

      samples+=c;

      if(peak>=MIC_SIL)
        lastVoice=millis();

      if(
        millis()-voiceStart>=REC_MIN&&
        millis()-lastVoice>=SILENCE
      )
        break;
    }

    yield();
  }

  wavHeader(
    f,
    samples*2
  );

  f.close();

  if(!voice||!samples){

    LittleFS.remove(STT_FILE);
    return false;
  }

  return true;
}


/* =========================================================
   NTP
   ========================================================= */

bool syncTime(){

  configTime(
    7*3600,
    0,
    "pool.ntp.org",
    "time.nist.gov",
    "time.google.com"
  );

  for(byte a=1;a<=4;a++){

    Serial.printf(
      "TARS: NTP %d/4\r\n",
      a
    );

    for(byte i=0;i<20;i++){

      time_t n=time(nullptr);

      if(n>=1704067200){

        struct tm t;

        localtime_r(
          &n,
          &t
        );

        Serial.printf(
          "TARS: WIB %04d-%02d-%02d %02d:%02d:%02d\r\n",
          t.tm_year+1900,
          t.tm_mon+1,
          t.tm_mday,
          t.tm_hour,
          t.tm_min,
          t.tm_sec
        );

        ntpOK=true;
        return true;
      }

      delay(500);
    }
  }

  ntpOK=false;

  return false;
}


/* =========================================================
   WIFI
   ========================================================= */

bool wifiOK(){

  return
    WiFi.status()==WL_CONNECTED||
    wifiManagerConnect(false);
}

bool ensureWiFi(){

  while(
    WiFi.status()!=WL_CONNECTED
  ){

    oledBase(
      "BOOT",
      "WAITING WIFI..."
    );

    if(!wifiManagerConnect(false)){

      oledBase(
        "BOOT",
        "WIFI RETRY..."
      );

      delay(1000);
    }
  }

  return true;
}


/* =========================================================
   HTTP BODY
   ========================================================= */

String readBody(
  WiFiClientSecure &c
){
  String l,b;

  bool chunk=false;
  int len=-1;

  while(c.connected()){

    l=c.readStringUntil('\n');
    l.trim();

    if(!l.length())
      break;

    String x=l;
    x.toLowerCase();

    if(x.startsWith("content-length:"))
      len=x.substring(15).toInt();

    if(
      x.indexOf("transfer-encoding:")>=0&&
      x.indexOf("chunked")>=0
    )
      chunk=true;
  }

  if(chunk){

    while(c.connected()){

      l=c.readStringUntil('\n');
      l.trim();

      if(!l.length())
        continue;

      int n=
        strtol(
          l.c_str(),
          nullptr,
          16
        );

      if(n<=0)
        break;

      while(n>0){

        uint8_t z[BUF];

        size_t w=
          min(
            (int)sizeof(z),
            n
          );

        size_t r=
          c.readBytes(
            z,
            w
          );

        if(!r)
          break;

        b.concat(
          (char*)z,
          r
        );

        n-=r;
      }

      c.readStringUntil('\n');
    }

  }else if(len>=0){

    while(
      (int)b.length()<len&&
      c.connected()
    ){

      uint8_t z[BUF];

      int remain=
        len-b.length();

      size_t w=
        min(
          (int)sizeof(z),
          remain
        );

      size_t r=
        c.readBytes(
          z,
          w
        );

      if(!r)
        break;

      b.concat(
        (char*)z,
        r
      );
    }

  }else{

    b=c.readString();
  }

  return b;
}


/* =========================================================
   STT
   ========================================================= */

String stt(){

  if(!wifiOK()||!ntpOK)
    return "";

  File f=
    LittleFS.open(
      STT_FILE
    );

  if(!f)
    return "";

  const char *bd=
    "----TARSSTT";

  String head=
    "--"+String(bd)+
    "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"stt.wav\""
    "\r\nContent-Type: audio/wav\r\n\r\n";

  String tail=
    "\r\n--"+String(bd)+"--\r\n";

  WiFiClientSecure c;

  c.setInsecure();
  c.setTimeout(15000);

  String host=
    TARS_CLOUD_URL;

  int p=
    host.indexOf("://");

  if(p>=0)
    host=host.substring(p+3);

  p=host.indexOf('/');

  if(p>=0)
    host=host.substring(0,p);

  if(!c.connect(host.c_str(),443)){

    f.close();
    return "";
  }

  size_t total=
    head.length()+
    f.size()+
    tail.length();

  c.printf(
    "POST /stt HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Content-Type: multipart/form-data; boundary=%s\r\n"
    "Content-Length: %u\r\n"
    "Connection: close\r\n\r\n",
    host.c_str(),
    bd,
    (unsigned)total
  );

  c.print(head);

  uint8_t z[BUF];

  while(f.available()){

    size_t n=
      f.read(
        z,
        sizeof(z)
      );

    if(
      c.write(z,n)!=n
    ){

      f.close();
      c.stop();

      return "";
    }
  }

  f.close();

  c.print(tail);

  uint32_t t=
    millis();

  while(
    !c.available()&&
    c.connected()&&
    millis()-t<20000
  )
    delay(5);

  if(!c.available()){

    c.stop();
    return "";
  }

  String status=
    c.readStringUntil('\n');

  status.trim();

  String r=
    readBody(c);

  c.stop();

  if(
    !r.length()||
    status.indexOf(" 200 ")<0
  )
    return "";

  JsonDocument j;

  if(deserializeJson(j,r))
    return "";

  String q=
    j["text"].as<String>();

  if(!q.length())
    q=j["transcript"].as<String>();

  q.trim();

  return q;
}


/* =========================================================
   ASK
   ========================================================= */

String ask(
  const String &q
){

  if(!wifiOK()||!ntpOK)
    return "";

  WiFiClientSecure c;
  c.setInsecure();

  HTTPClient h;

  if(
    !h.begin(
      c,
      String(TARS_CLOUD_URL)+"/ask"
    )
  )
    return "";

  h.setTimeout(15000);

  h.addHeader(
    "Content-Type",
    "application/json"
  );

  JsonDocument j;

  j["question"]=q;

  String b;

  serializeJson(
    j,
    b
  );

  int code=
    h.POST(b);

  if(code<200||code>=300){

    h.end();
    return "";
  }

  String r=
    h.getString();

  h.end();

  JsonDocument x;

  if(deserializeJson(x,r))
    return "";

  String a=
    x["response"].as<String>();

  if(!a.length())
    a=x["answer"].as<String>();

  return a;
}


/* =========================================================
   OFFLINE ANSWER
   ========================================================= */

String offlineAnswer(
  String q
){

  q.toLowerCase();
  q.trim();

  if(
    q.indexOf("hidup jokowi")>=0||
    q.indexOf("jokowi hidup")>=0
  )
    return "Saya akan lawan.";

  if(
    q.indexOf("jam berapa")>=0||
    q.indexOf("tanggal berapa")>=0
  ){

    time_t n=time(nullptr);

    struct tm t;

    localtime_r(
      &n,
      &t
    );

    const char *day[]={
      "Minggu",
      "Senin",
      "Selasa",
      "Rabu",
      "Kamis",
      "Jumat",
      "Sabtu"
    };

    const char *month[]={
      "Januari",
      "Februari",
      "Maret",
      "April",
      "Mei",
      "Juni",
      "Juli",
      "Agustus",
      "September",
      "Oktober",
      "November",
      "Desember"
    };

    char s[160];

    snprintf(
      s,
      sizeof(s),
      "Pukul %02d lewat %02d menit WIB. %s, %02d %s tahun %04d.",
      t.tm_hour,
      t.tm_min,
      day[t.tm_wday],
      t.tm_mday,
      month[t.tm_mon],
      t.tm_year+1900
    );

    return s;
  }

  if(
    q.indexOf("ceritakan pengalamanmu")>=0||
    q.indexOf("ceritakan pengalaman")>=0||
    q.indexOf("pengalaman kamu")>=0
  )
    return
      "Selama menjadi robot Interstellar, saya telah melewati banyak hal. "
      "Saya belajar tentang perjalanan antarbintang, menghadapi bahaya, "
      "dan menemani manusia dalam misi yang jauh dari Bumi.";

  return "";
}


/* =========================================================
   OFFLINE MP3 JOKOWI
   ========================================================= */

bool embeddedMP3(){

  LittleFS.remove(
    PLAY_FILE
  );

  File f=
    LittleFS.open(
      PLAY_FILE,
      FILE_WRITE
    );

  if(!f)
    return false;

  size_t n=
    f.write(
      TARS_ELEVENLABS_MP3,
      TARS_ELEVENLABS_MP3_LEN
    );

  f.close();

  return
    n==TARS_ELEVENLABS_MP3_LEN;
}


/* =========================================================
   OFFLINE MP3 EXPERIENCE
   ========================================================= */

#if HAS_EXPERIENCE_MP3

bool embeddedExperienceMP3(){

  LittleFS.remove(
    PLAY_FILE
  );

  File f=
    LittleFS.open(
      PLAY_FILE,
      FILE_WRITE
    );

  if(!f)
    return false;

  size_t n=
    f.write(
      TARS_EXPERIENCE_MP3,
      TARS_EXPERIENCE_MP3_LEN
    );

  f.close();

  return
    n==TARS_EXPERIENCE_MP3_LEN;
}

#endif


/* =========================================================
   DAC AUDIO STREAM
   ========================================================= */

class DACOut:public AudioStream{

  AudioInfo info;

  int16_t buf[DAC_BUF];

  volatile size_t head=0;
  volatile size_t tail=0;

  volatile bool active=false;

  TaskHandle_t task=nullptr;

  uint32_t rate=PLAY_RATE;

  size_t count(){

    size_t h=head;
    size_t t=tail;

    return
      h>=t?
      h-t:
      DAC_BUF-t+h;
  }

  size_t freeBuf(){

    return
      DAC_BUF-1-count();
  }

  static void taskFn(void *x){

    ((DACOut*)x)->run();
  }

  void run(){

    uint32_t us=
      1000000UL/
      (rate?rate:PLAY_RATE);

    uint32_t next=
      micros();

    while(active){

      if(head==tail){

        vTaskDelay(1);
        continue;
      }

      int16_t s=
        buf[tail];

      tail=
        (tail+1)%DAC_BUF;

      int32_t v=
        (int32_t)s*825/1000;

      int out=
        constrain(
          (v+32768+128)>>8,
          0,
          255
        );

      while(
        (int32_t)(
          next-micros()
        )>0
      )
        delayMicroseconds(1);

      dacWrite(
        DAC_PIN,
        out
      );

      next+=us;

      if(!(tail&63))
        taskYIELD();
    }

    dacMute();

    active=false;
    task=nullptr;

    vTaskDelete(nullptr);
  }

public:

  void setAudioInfo(
    AudioInfo i
  )override{

    info=i;

    AudioStream::setAudioInfo(i);

    rate=
      i.sample_rate?
      i.sample_rate:
      PLAY_RATE;
  }

  int availableForWrite()override{

    return
      freeBuf()*2;
  }

  void start(){

    head=tail=0;

    dacMute();

    active=true;

    if(!task){

      xTaskCreatePinnedToCore(
        taskFn,
        "TARS_DAC",
        4096,
        this,
        2,
        &task,
        1
      );
    }
  }

  bool empty(){

    return head==tail;
  }

  void stop(){

    active=false;

    uint32_t t=
      millis();

    while(
      task&&
      millis()-t<2000
    )
      vTaskDelay(1);

    dacMute();
  }

  size_t write(
    const uint8_t *d,
    size_t n
  )override{

    if(
      !d||
      !active||
      info.bits_per_sample!=16
    )
      return 0;

    size_t ch=
      info.channels;

    size_t bpf=
      ch*2;

    size_t frames=
      n/bpf;

    size_t done=0;

    while(done<frames){

      size_t space=
        freeBuf();

      if(!space){

        vTaskDelay(1);
        continue;
      }

      size_t c=
        min(
          space,
          frames-done
        );

      for(size_t i=0;i<c;i++){

        size_t k=
          done+i;

        int16_t s;

        if(ch==1){

          s=
            d[k*2]|
            ((uint16_t)d[k*2+1]<<8);

        }else{

          int16_t l=
            d[k*4]|
            ((uint16_t)d[k*4+1]<<8);

          int16_t r=
            d[k*4+2]|
            ((uint16_t)d[k*4+3]<<8);

          s=(l+r)/2;
        }

        buf[head]=s;

        head=
          (head+1)%DAC_BUF;
      }

      done+=c;

      taskYIELD();
    }

    return n;
  }
};


DACOut dacOut;

MP3DecoderHelix decoder;

EncodedAudioStream mp3(
  &dacOut,
  &decoder
);


/* =========================================================
   PLAY MP3
   ========================================================= */

bool playMP3(){

  File f=
    LittleFS.open(
      PLAY_FILE
    );

  if(!f)
    return false;

  playing=true;

  oledPos=0;
  oledPage=0;
  oledTick=millis();

  if(!dacOK)
    dacOK=initDAC();

  if(
    !dacOK||
    !mp3.begin()
  ){

    f.close();
    playing=false;

    return false;
  }

  oledBase(
    singMode?
    "SINGING":
    "SPEAKING"
  );

  dacOut.start();

  StreamCopy cp(
    mp3,
    f,
    BUF
  );

  uint32_t start=
    millis();

  while(
    f.available()&&
    millis()-start<120000
  ){

    if(!cp.copy())
      delay(1);

    oledType();

    yield();
  }

  mp3.end();

  f.close();

  while(!dacOut.empty()){

    oledType();
    delay(1);
  }

  dacOut.stop();

  playing=false;

  LittleFS.remove(
    PLAY_FILE
  );

  oledBase(
    "STANDBY",
    "LISTENING..."
  );

  return true;
}


/* =========================================================
   DOWNLOAD MP3
   ========================================================= */

bool downloadMP3(
  const String &url,
  const String &text
){

  if(!wifiOK()||!ntpOK)
    return false;

  WiFiClientSecure c;

  c.setInsecure();

  HTTPClient h;

  if(!h.begin(c,url))
    return false;

  h.setTimeout(60000);

  h.addHeader(
    "Content-Type",
    "application/json"
  );

  JsonDocument j;

  if(url.endsWith("/sing"))
    j["prompt"]=text;
  else
    j["text"]=text;

  String b;

  serializeJson(
    j,
    b
  );

  int code=
    h.POST(b);

  if(code<200||code>=300){

    h.end();
    return false;
  }

  LittleFS.remove(
    PLAY_FILE
  );

  File f=
    LittleFS.open(
      PLAY_FILE,
      FILE_WRITE
    );

  if(!f){

    h.end();
    return false;
  }

  WiFiClient *s=
    h.getStreamPtr();

  uint8_t z[BUF];

  int len=
    h.getSize();

  size_t total=0;

  uint32_t last=
    millis();

  while(
    h.connected()&&
    (len>0||len==-1)
  ){

    size_t n=
      s->available();

    if(n){

      n=min(
        n,
        sizeof(z)
      );

      int r=
        s->readBytes(
          z,
          n
        );

      if(r>0){

        f.write(
          z,
          r
        );

        total+=r;

        if(len>0)
          len-=r;

        last=millis();
      }

    }else{

      if(
        millis()-last>10000
      )
        break;

      delay(1);
    }

    yield();
  }

  f.close();

  h.end();

  return total>0;
}


/* =========================================================
   SING
   ========================================================= */

bool singRequest(
  String q
){

  q.toLowerCase();

  return
    q.indexOf("nyanyi")>=0||
    q.indexOf("bernyanyi")>=0||
    q.indexOf("nyanyikan")>=0;
}


/* =========================================================
   PROCESS QUESTION
   ========================================================= */

void processQuestion(
  const String &q
){

  String lower=q;

  lower.toLowerCase();

  String a=
    offlineAnswer(q);

  singMode=
    singRequest(q);

  /*
     =====================================================
     JOKOWI — FULL OFFLINE
     =====================================================
  */
  if(
    lower.indexOf("jokowi")>=0
  ){

    oledText=
      "Saya akan lawan.";

    oledPos=0;
    oledPage=0;

    singMode=false;

    if(!embeddedMP3()){

      oledBase(
        "STANDBY",
        "OFFLINE AUDIO ERROR"
      );

    }else{

      playMP3();
    }

    return;
  }

  /*
     =====================================================
     EXPERIENCE — FULL OFFLINE
     =====================================================
  */
  if(
    lower.indexOf("pengalaman")>=0
  ){

    oledText=
      "Selama menjadi robot Interstellar, saya telah melewati banyak hal. "
      "Saya belajar tentang perjalanan antarbintang, menghadapi bahaya, "
      "dan menemani manusia dalam misi yang jauh dari Bumi.";

    oledPos=0;
    oledPage=0;

    singMode=false;

#if HAS_EXPERIENCE_MP3

    /*
       Kalau TARS_Experience_MP3.h sudah tersedia,
       TARS tidak perlu Wi-Fi untuk menjawab.
    */
    if(!embeddedExperienceMP3()){

      oledBase(
        "STANDBY",
        "EXPERIENCE AUDIO ERROR"
      );

    }else{

      playMP3();
    }

    return;

#else

    /*
       Header pengalaman belum tersedia.
       Sementara fallback ke TTS cloud.
    */
    Serial.println(
      "TARS: EXPERIENCE MP3 NOT INSTALLED -> CLOUD TTS"
    );

#endif
  }

  /*
     =====================================================
     JAM / TANGGAL
     =====================================================
  */
  if(
    lower.indexOf("jam")>=0||
    lower.indexOf("tanggal")>=0
  ){

    if(a.length()){

      oledText=a;
      oledPos=0;
      oledPage=0;
      singMode=false;

      if(!wifiOK()||!ntpOK){

        oledBase(
          "STANDBY",
          "AUDIO ERROR"
        );

        return;
      }

    }
  }

  /*
     =====================================================
     CLOUD NORMAL
     =====================================================
  */

  if(!a.length())
    a=ask(q);

  if(!a.length()){

    oledBase(
      "STANDBY",
      "ASK ERROR"
    );

    return;
  }

  oledText=a;
  oledPos=0;
  oledPage=0;

  String url=
    String(TARS_CLOUD_URL)+
    (singMode?"/sing":"/tts");

  if(
    downloadMP3(
      url,
      singMode?q:a
    )
  ){

    playMP3();

  }else{

    oledBase(
      "STANDBY",
      "AUDIO ERROR"
    );
  }

  singMode=false;
}


/* =========================================================
   SETUP
   ========================================================= */

void setup(){

  Serial.begin(
    SERIAL_BAUD
  );

  Wire.begin(
    OLED_SDA,
    OLED_SCL
  );

  Wire.setClock(
    400000
  );

  oledOK=
    oled.begin(
      SSD1306_SWITCHCAPVCC,
      OLED_ADDR
    );

  if(oledOK)
    oledBase("BOOT");

  LittleFS.begin(true);

  dacOK=
    initDAC();

  micOK=
    initMic();

  Serial.println(
    "TARS: BLUETOOTH DISABLED"
  );

  Serial.println(
    "TARS: TARS = ONLINE STT"
  );

  Serial.println(
    "TARS: JOKOWI = OFFLINE"
  );

  Serial.println(
    "TARS: EXPERIENCE = OFFLINE"
  );

  Serial.println(
    "TARS: UNKNOWN = BLOCKED"
  );

#if HAS_EXPERIENCE_MP3

  Serial.println(
    "TARS: EXPERIENCE MP3 = INSTALLED"
  );

#else

  Serial.println(
    "TARS: EXPERIENCE MP3 = NOT INSTALLED"
  );

#endif

  wifiManagerBegin();

  ensureWiFi();

  /*
     NTP hanya dilakukan sampai berhasil.
     Setelah valid, tidak diulang saat Wi-Fi reconnect.
  */
  while(!syncTime()){

    oledBase(
      "BOOT",
      "NTP RETRY..."
    );

    delay(2000);

    if(
      WiFi.status()!=WL_CONNECTED
    )
      ensureWiFi();
  }

  ntpOK=true;

  Serial.println(
    "TARS: WIFI + NTP READY"
  );

  oledBase(
    "STANDBY",
    "LISTENING..."
  );
}


/* =========================================================
   LOOP
   ========================================================= */

void loop(){

  if(playing)
    return;

  if(
    WiFi.status()!=WL_CONNECTED
  ){

    /*
       Reconnect Wi-Fi tanpa NTP ulang.
    */
    if(!wifiOK()){

      oledBase(
        "STANDBY",
        "WIFI ERROR"
      );

      delay(1000);

      return;
    }

    Serial.println(
      "TARS: WIFI RECONNECTED"
    );
  }

  /*
     Rekam satu ucapan.
  */
  if(!recordMic()){

    oledListen();
    delay(1);

    return;
  }

  switch(classifyInput()){

    /*
       ==================================================
       WAKE TARS
       ==================================================
    */
    case INPUT_WAKE_TARS:{

      Serial.println(
        "TARS: TARS DETECTED -> STT ONLINE"
      );

      String q=
        stt();

      LittleFS.remove(
        STT_FILE
      );

      if(q.length()){

        Serial.printf(
          "TARS: STT = %s\r\n",
          q.c_str()
        );

        processQuestion(q);

      }else{

        oledBase(
          "STANDBY",
          "NO INPUT"
        );
      }

      break;
    }


    /*
       ==================================================
       HIDUP JOKOWI
       ==================================================
    */
    case INPUT_OFFLINE_JOKOWI:

      Serial.println(
        "TARS: OFFLINE -> HIDUP JOKOWI"
      );

      LittleFS.remove(
        STT_FILE
      );

      processQuestion(
        "hidup Jokowi"
      );

      break;


    /*
       ==================================================
       EXPERIENCE
       ==================================================
    */
    case INPUT_OFFLINE_EXP:

      Serial.println(
        "TARS: OFFLINE -> EXPERIENCE"
      );

      LittleFS.remove(
        STT_FILE
      );

      processQuestion(
        "ceritakan pengalamanmu"
      );

      break;


    /*
       ==================================================
       UNKNOWN
       ==================================================
    */
    default:

      Serial.println(
        "TARS: UNKNOWN -> NO STT"
      );

      LittleFS.remove(
        STT_FILE
      );

      oledBase(
        "STANDBY",
        "LISTENING..."
      );

      break;
  }

  delay(1);
}
