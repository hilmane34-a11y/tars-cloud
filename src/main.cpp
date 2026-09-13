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
#include "config.h"
#include "wifi_manager.h"

#define MIC_PORT I2S_NUM_1
#define MIC_SCK 18
#define MIC_WS 19
#define MIC_SD 34
#define AUDIO_DAC_PIN 26

const uint32_t MIC_RATE=16000,PLAY_RATE=22050,REC_MAX=5000,REC_MIN=700,
SILENCE=600,LISTEN_MAX=15000,PREROLL_MS=500,OLED_MS=15;
const int32_t MIC_TH=14000,MIC_SIL=12000;
const size_t BUF=1024,DAC_BUF=16384,PRE=MIC_RATE*PREROLL_MS/1000;

const char *STT_FILE="/stt.wav",*PLAY_FILE="/tars.mp3";
const char *OFFLINE_JOKOWI="/offline_jokowi.mp3";
const char *OFFLINE_EXP="/offline_experience.mp3";

/* ===== 3 TEMPLATE SUARA =====
   1 = "TARS"
   2 = "hidup Jokowi"
   3 = "ceritakan pengalamanmu"
*/
const uint8_t TEMPLATE_TARS[64]={
  0,18,105,209,248,253,255,255,255,253,242,229,233,228,225,228,
  214,219,233,230,232,231,215,215,226,224,225,228,224,230,237,226,
  216,212,209,221,235,222,209,211,192,163,146,102,33,9,29,25,
  4,4,4,0,0,5,9,9,9,8,10,11,9,6,5,4
};

const uint8_t TEMPLATE_JOKOWI[64]={
  0,54,192,227,253,144,35,70,237,245,83,6,0,2,7,12,
  115,242,255,227,45,2,0,40,16,85,207,210,203,140,112,120,
  125,125,134,134,112,105,93,90,88,80,76,70,65,69,75,74,
  79,81,78,77,76,66,61,42,36,42,39,31,23,20,16,3
};

const uint8_t TEMPLATE_EXP[64]={
  3,17,186,237,255,229,59,150,120,106,41,1,0,2,175,255,
  229,58,4,1,30,39,206,249,227,84,57,41,10,0,0,47,
  180,73,92,101,197,213,219,215,211,248,219,201,157,69,72,157,
  184,162,67,51,65,70,75,95,132,139,137,119,83,40,30,12
};

const float WAKE_TH=.12f;
const float OFFLINE_JOKOWI_TH=.14f;
const float OFFLINE_EXP_TH=.14f;

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);
bool oledOK,micOK,dacOK,playing,singMode,ntpOK;
String oledText;
size_t oledPos,oledPage;
uint32_t oledTick,dotTick;
uint8_t dots=1;

/* ===== OLED ===== */
void oledHeader(const char*s){
  oled.clearDisplay();oled.setTextColor(SSD1306_WHITE);oled.setTextSize(1);
  oled.setCursor(42,0);oled.print("T A R S");
  oled.drawLine(0,9,127,9,SSD1306_WHITE);
  oled.setCursor(3,14);oled.print(s);
}

void oledBase(const char*a,const String&b=""){
  if(!oledOK)return;
  oledHeader(a);
  if(b.length()){oled.setCursor(3,27);oled.print(b);}
  oled.display();
}

void oledListen(){
  if(!oledOK||millis()-dotTick<350)return;
  dotTick=millis();dots=dots>=4?1:dots+1;
  String s;for(uint8_t i=0;i<dots;i++)s+='.';
  oledBase("STANDBY",s);
}

void oledType(){
  if(!oledOK||oledPos>=oledText.length()||millis()-oledTick<OLED_MS)return;
  oledTick=millis();oledHeader(singMode?"SINGING":"SPEAKING");

  int x=3,y=27;size_t i=oledPage;
  while(i<oledPos){
    char c=oledText[i++];
    if(c=='\n'||x>121){x=3;y+=8;if(c=='\n')continue;}
    if(y>59){oledPage=i-1;oledHeader(singMode?"SINGING":"SPEAKING");
      x=3;y=27;i=oledPage;}
    if(y<=59){oled.setCursor(x,y);oled.write(c);x=oled.getCursorX();}
  }

  uint8_t n=0;
  while(oledPos<oledText.length()&&n<5){
    char c=oledText[oledPos++];
    if(c=='\n'||x>121){x=3;y+=8;if(c=='\n')continue;}
    if(y>59){oledPage=oledPos-1;oledHeader(singMode?"SINGING":"SPEAKING");
      x=3;y=27;continue;}
    oled.setCursor(x,y);oled.write(c);x=oled.getCursorX();n++;
  }
  oled.display();
}

/* ===== DAC ===== */
bool initDAC(){
  pinMode(AUDIO_DAC_PIN,OUTPUT);dacWrite(AUDIO_DAC_PIN,0);return true;
}
inline void dacMute(){dacWrite(AUDIO_DAC_PIN,0);}

/* ===== INMP441 ===== */
bool initMic(){
  i2s_config_t c={};
  c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_RX);
  c.sample_rate=MIC_RATE;
  c.bits_per_sample=I2S_BITS_PER_SAMPLE_32BIT;
  c.channel_format=I2S_CHANNEL_FMT_ONLY_RIGHT;
  c.communication_format=I2S_COMM_FORMAT_I2S;
  c.dma_buf_count=2;c.dma_buf_len=256;
  if(i2s_driver_install(MIC_PORT,&c,0,nullptr)!=ESP_OK)return false;

  i2s_pin_config_t p={
    I2S_PIN_NO_CHANGE,MIC_SCK,MIC_WS,
    I2S_PIN_NO_CHANGE,MIC_SD
  };
  return i2s_set_pin(MIC_PORT,&p)==ESP_OK;
}

/* ===== WAV ===== */
void put16(uint8_t*p,uint16_t v){p[0]=v;p[1]=v>>8;}
void put32(uint8_t*p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}

void wavHeader(File&f,uint32_t n){
  uint8_t h[44]={};
  memcpy(h,"RIFF",4);put32(h+4,n+36);memcpy(h+8,"WAVEfmt ",8);
  put32(h+16,16);put16(h+20,1);put16(h+22,1);put32(h+24,MIC_RATE);
  put32(h+28,MIC_RATE*2);put16(h+32,2);put16(h+34,16);
  memcpy(h+36,"data",4);put32(h+40,n);f.seek(0);f.write(h,44);
}

/* ===== ENVELOPE ===== */
uint16_t envelope(uint8_t*e,uint16_t max){
  File f=LittleFS.open(STT_FILE);
  if(!f||f.size()<=44){if(f)f.close();return 0;}
  f.seek(44);

  int16_t p[BUF/2];
  uint16_t bins=0,n=0;
  uint32_t sum=0;

  while(f.available()&&bins<max){
    size_t z=f.read((uint8_t*)p,sizeof(p));
    if(!z)break;

    for(size_t i=0;i<z/2;i++){
      sum+=abs((int32_t)p[i]);
      if(++n>=160){
        e[bins++]=min((uint32_t)255,sum/n/128);
        n=0;sum=0;
        if(bins>=max)break;
      }
    }
  }

  if(n&&bins<max)e[bins++]=min((uint32_t)255,sum/n/128);
  f.close();
  return bins;
}

/* ===== TEMPLATE SCORE ===== */
float templateScore(const uint8_t*e,uint16_t total,const uint8_t*t,
                    uint16_t minLen,uint16_t maxLen,
                    uint16_t*bestStart=nullptr,uint16_t*bestLen=nullptr){
  if(total<minLen)return 1.0f;
  maxLen=min(maxLen,total);
  float best=1.0f;

  for(uint16_t len=minLen;len<=maxLen;len+=5)
    for(uint16_t s=0;s+len<=total;s++){
      uint8_t lo=255,hi=0;

      for(uint16_t i=0;i<len;i++){
        lo=min(lo,e[s+i]);hi=max(hi,e[s+i]);
      }

      if(hi<=lo+4)continue;

      float d=0;
      for(uint8_t j=0;j<64;j++){
        float q=(float)j*(len-1)/63.0f;
        uint16_t j0=(uint16_t)q,j1=j0+1<len?j0+1:j0;
        float v=e[s+j0]+(e[s+j1]-e[s+j0])*(q-j0);
        v=constrain((v-lo)*255.0f/(hi-lo),0.0f,255.0f);
        d+=fabsf(v-t[j])/255.0f;
      }

      d/=64.0f;
      if(d<best){
        best=d;
        if(bestStart)*bestStart=s;
        if(bestLen)*bestLen=len;
      }
    }

  return best;
}

/* ===== KLASIFIKASI ===== */
enum InputType{
  INPUT_BLOCKED,
  INPUT_OFFLINE_JOKOWI,
  INPUT_OFFLINE_EXP,
  INPUT_WAKE_TARS
};

InputType classifyInput(){
  uint8_t e[600];
  uint16_t n=envelope(e,600);
  if(n<40){
    Serial.println("TARS: AUDIO TOO SHORT");
    return INPUT_BLOCKED;
  }

  uint16_t s=0,l=0;

  /* 1. TARS = SATU-SATUNYA PEMBUKA STT */
  float tars=templateScore(e,n,TEMPLATE_TARS,45,110,&s,&l);
  Serial.printf("TARS: WAKE %.3f / %.3f\r\n",tars,WAKE_TH);

  if(tars<=WAKE_TH){
    Serial.println("TARS: WAKE -> STT ONLINE");
    return INPUT_WAKE_TARS;
  }

  /* 2. HIDUP JOKOWI = OFFLINE */
  float jokowi=templateScore(
    e,n,TEMPLATE_JOKOWI,90,170,&s,&l
  );
  Serial.printf("TARS: JOKOWI %.3f / %.3f\r\n",
                jokowi,OFFLINE_JOKOWI_TH);

  if(jokowi<=OFFLINE_JOKOWI_TH){
    Serial.println("TARS: OFFLINE -> HIDUP JOKOWI");
    return INPUT_OFFLINE_JOKOWI;
  }

  /* 3. CERITAKAN PENGALAMANMU = OFFLINE */
  float exp=templateScore(
    e,n,TEMPLATE_EXP,130,230,&s,&l
  );
  Serial.printf("TARS: EXPERIENCE %.3f / %.3f\r\n",
                exp,OFFLINE_EXP_TH);

  if(exp<=OFFLINE_EXP_TH){
    Serial.println("TARS: OFFLINE -> PENGALAMAN");
    return INPUT_OFFLINE_EXP;
  }

  /* 4. SEMUA LAIN DIBLOKIR */
  Serial.println("TARS: UNKNOWN -> BLOCKED / NO STT");
  return INPUT_BLOCKED;
}

/* ===== RECORD ===== */
bool recordMic(){
  if(!micOK)return false;

  oledBase("STANDBY","LISTENING");
  LittleFS.remove(STT_FILE);

  File f=LittleFS.open(STT_FILE,FILE_WRITE);
  if(!f)return false;

  uint8_t z[44]={};f.write(z,44);
  static int16_t pre[PRE];

  size_t pp=0,pc=0;
  int32_t raw[BUF/4];
  int16_t pcm[BUF/4];
  uint32_t samples=0,start=millis(),vs=0,last=0;
  bool voice=false;
  uint8_t act=0;

  while((!voice&&millis()-start<LISTEN_MAX)||
        (voice&&millis()-vs<REC_MAX)){

    oledListen();

    size_t n=0;
    if(i2s_read(MIC_PORT,raw,sizeof(raw),&n,pdMS_TO_TICKS(50))!=ESP_OK)
      continue;

    size_t c=n/4;
    int32_t bp=0;

    for(size_t i=0;i<c;i++){
      int32_t v=constrain(raw[i]>>16,-32768,32767);
      pcm[i]=v;bp=max(bp,abs(v));
    }

    if(!voice){
      for(size_t i=0;i<c;i++){
        pre[pp]=pcm[i];pp=(pp+1)%PRE;
        if(pc<PRE)pc++;
      }

      if(bp>=MIC_TH&&++act>=2){
        voice=true;vs=millis();last=vs;
        size_t st=pc==PRE?pp:0;

        for(size_t i=0;i<pc;i++){
          size_t k=(st+i)%PRE;
          f.write((uint8_t*)&pre[k],2);
        }

        samples+=pc;
        f.write((uint8_t*)pcm,c*2);
        samples+=c;

      }else if(bp<MIC_TH)act=0;

    }else{
      f.write((uint8_t*)pcm,c*2);
      samples+=c;

      if(bp>=MIC_SIL)last=millis();
      if(millis()-vs>=REC_MIN&&millis()-last>=SILENCE)break;
    }

    yield();
  }

  wavHeader(f,samples*2);
  f.close();

  if(!voice||!samples){
    LittleFS.remove(STT_FILE);
    return false;
  }

  return true;
}

/* ===== NTP ===== */
bool syncTime(){
  configTime(7*3600,0,
             "pool.ntp.org",
             "time.nist.gov",
             "time.google.com");

  for(int a=1;a<=4;a++){
    Serial.printf("TARS: NTP %d/4\r\n",a);

    for(int i=0;i<20;i++){
      time_t n=time(nullptr);

      if(n>=1704067200){
        struct tm t;
        localtime_r(&n,&t);
        Serial.printf(
          "TARS: WIB %04d-%02d-%02d %02d:%02d:%02d\r\n",
          t.tm_year+1900,t.tm_mon+1,t.tm_mday,
          t.tm_hour,t.tm_min,t.tm_sec
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

/* ===== WIFI ===== */
bool wifiOK(){
  return WiFi.status()==WL_CONNECTED||wifiManagerConnect(false);
}

bool ensureWiFi(){
  while(WiFi.status()!=WL_CONNECTED){
    oledBase("BOOT","WAITING WIFI...");
    if(!wifiManagerConnect(false)){
      oledBase("BOOT","WIFI RETRY...");
      delay(1000);
    }
  }
  return true;
}

/* ===== HTTP BODY ===== */
String body(WiFiClientSecure&c){
  String l,b;
  bool chunk=false;
  int len=-1;

  while(c.connected()){
    l=c.readStringUntil('\n');l.trim();
    if(!l.length())break;

    String x=l;x.toLowerCase();
    if(x.startsWith("content-length:"))len=x.substring(15).toInt();
    if(x.indexOf("transfer-encoding:")>=0&&x.indexOf("chunked")>=0)
      chunk=true;
  }

  if(chunk){
    while(c.connected()){
      l=c.readStringUntil('\n');l.trim();
      if(!l.length())continue;

      int n=strtol(l.c_str(),0,16);
      if(n<=0)break;

      while(n>0){
        uint8_t z[BUF];
        size_t w=min((int)sizeof(z),n);
        size_t r=c.readBytes(z,w);
        if(!r)break;
        b.concat((char*)z,r);
        n-=r;
      }
      c.readStringUntil('\n');
    }
  }else if(len>=0){
    while((int)b.length()<len&&c.connected()){
      uint8_t z[BUF];
      int rl=len-b.length();
      size_t w=min((int)sizeof(z),rl);
      size_t r=c.readBytes(z,w);
      if(!r)break;
      b.concat((char*)z,r);
    }
  }else b=c.readString();

  return b;
}

/* ===== STT ONLINE =====
   HANYA dipanggil jika classifyInput() = TARS.
*/
String stt(){
  if(!wifiOK())return "";

  File f=LittleFS.open(STT_FILE);
  if(!f)return "";

  const char*bd="----TARSSTT";
  String a="--"+String(bd)+
    "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"stt.wav\""+
    "\r\nContent-Type: audio/wav\r\n\r\n";
  String e="\r\n--"+String(bd)+"--\r\n";

  WiFiClientSecure c;
  c.setInsecure();c.setTimeout(15000);

  String host=TARS_CLOUD_URL;
  int p=host.indexOf("://");
  if(p>=0)host=host.substring(p+3);
  p=host.indexOf('/');
  if(p>=0)host=host.substring(0,p);

  if(!c.connect(host.c_str(),443)){
    f.close();return "";
  }

  size_t total=a.length()+f.size()+e.length();

  c.printf(
    "POST /stt HTTP/1.1\r\nHost: %s\r\n"
    "Content-Type: multipart/form-data; boundary=%s\r\n"
    "Content-Length: %u\r\nConnection: close\r\n\r\n",
    host.c_str(),bd,(unsigned)total
  );

  c.print(a);

  uint8_t z[BUF];
  while(f.available()){
    size_t n=f.read(z,sizeof(z));
    if(c.write(z,n)!=n){
      f.close();c.stop();return "";
    }
  }

  f.close();c.print(e);

  uint32_t t=millis();
  while(!c.available()&&c.connected()&&millis()-t<20000)delay(5);

  if(!c.available()){
    c.stop();return "";
  }

  String s=c.readStringUntil('\n');s.trim();
  String r=body(c);c.stop();

  if(!r.length()||s.indexOf(" 200 ")<0)return "";

  JsonDocument j;
  if(deserializeJson(j,r))return "";

  String q=j["text"].as<String>();
  if(!q.length())q=j["transcript"].as<String>();
  q.trim();
  return q;
}

/* ===== ASK ===== */
String ask(const String&q){
  if(!wifiOK())return "";

  WiFiClientSecure c;
  c.setInsecure();
  HTTPClient h;

  if(!h.begin(c,String(TARS_CLOUD_URL)+"/ask"))return "";

  h.setTimeout(15000);
  h.addHeader("Content-Type","application/json");

  JsonDocument j;
  j["question"]=q;

  String b;
  serializeJson(j,b);

  int code=h.POST(b);
  if(code<200||code>=300){
    h.end();return "";
  }

  String r=h.getString();
  h.end();

  JsonDocument x;
  if(deserializeJson(x,r))return "";

  return x["response"].as<String>();
}

/* ===== OFFLINE ===== */
String offlineAnswer(String q){
  q.toLowerCase();q.trim();

  if(q.indexOf("hidup jokowi")>=0||
     q.indexOf("jokowi hidup")>=0)
    return "Saya akan lawan.";

  if(q.indexOf("jam berapa")>=0||
     q.indexOf("tanggal berapa")>=0){
    time_t n=time(nullptr);
    struct tm t;
    localtime_r(&n,&t);

    const char*d[]={
      "Minggu","Senin","Selasa","Rabu",
      "Kamis","Jumat","Sabtu"
    };
    const char*m[]={
      "Januari","Februari","Maret","April",
      "Mei","Juni","Juli","Agustus",
      "September","Oktober","November","Desember"
    };

    char s[160];
    snprintf(
      s,sizeof(s),
      "Pukul %02d lewat %02d menit WIB. %s, %02d %s tahun %04d.",
      t.tm_hour,t.tm_min,d[t.tm_wday],t.tm_mday,
      m[t.tm_mon],t.tm_year+1900
    );
    return s;
  }

  if(q.indexOf("ceritakan pengalamanmu")>=0||
     q.indexOf("ceritakan pengalaman")>=0||
     q.indexOf("pengalaman kamu")>=0)
    return "Selama menjadi robot Interstellar, saya telah melewati banyak hal. "
           "Saya belajar tentang perjalanan antarbintang, menghadapi bahaya, "
           "dan menemani manusia dalam misi yang jauh dari Bumi.";

  return "";
}

const char* offlineFile(const String&q){
  String x=q;x.toLowerCase();x.trim();

  if(x.indexOf("hidup jokowi")>=0||
     x.indexOf("jokowi hidup")>=0)
    return OFFLINE_JOKOWI;

  if(x.indexOf("ceritakan pengalamanmu")>=0||
     x.indexOf("ceritakan pengalaman")>=0||
     x.indexOf("pengalaman kamu")>=0)
    return OFFLINE_EXP;

  return nullptr;
}

/* ===== MP3 ===== */
bool playMP3();

bool playOffline(const char*src,const String&text){
  if(!src||!LittleFS.exists(src))return false;

  File a=LittleFS.open(src);
  if(!a)return false;

  LittleFS.remove(PLAY_FILE);
  File b=LittleFS.open(PLAY_FILE,FILE_WRITE);

  if(!b){
    a.close();return false;
  }

  uint8_t z[BUF];
  while(a.available()){
    size_t n=a.read(z,sizeof(z));
    if(n)b.write(z,n);
  }

  a.close();b.close();

  oledText=text;
  oledPos=oledPage=0;
  singMode=false;

  return playMP3();
}

bool downloadMP3(const String&url,const String&text){
  if(!wifiOK())return false;

  WiFiClientSecure c;
  c.setInsecure();
  HTTPClient h;

  if(!h.begin(c,url))return false;

  h.setTimeout(60000);
  h.addHeader("Content-Type","application/json");

  JsonDocument j;
  if(url.endsWith("/sing"))j["prompt"]=text;
  else j["text"]=text;

  String b;
  serializeJson(j,b);

  int code=h.POST(b);
  if(code<200||code>=300){
    h.end();return false;
  }

  LittleFS.remove(PLAY_FILE);
  File f=LittleFS.open(PLAY_FILE,FILE_WRITE);

  if(!f){
    h.end();return false;
  }

  WiFiClient*s=h.getStreamPtr();
  uint8_t z[BUF];
  int len=h.getSize();
  size_t total=0;
  uint32_t t=millis();

  while(h.connected()&&(len>0||len==-1)){
    size_t n=s->available();

    if(n){
      n=min(n,sizeof(z));
      int r=s->readBytes(z,n);

      if(r>0){
        f.write(z,r);
        total+=r;
        if(len>0)len-=r;
        t=millis();
      }
    }else{
      if(millis()-t>10000)break;
      delay(1);
    }

    yield();
  }

  f.close();
  h.end();
  return total>0;
}

/* ===== DAC AUDIO ===== */
class DACOut:public AudioStream{
  AudioInfo info;
  int16_t b[DAC_BUF];
  volatile size_t h=0,t=0;
  volatile bool active=false;
  TaskHandle_t task=nullptr;
  uint32_t rate=PLAY_RATE;

  size_t cnt(){
    size_t x=h,y=t;
    return x>=y?x-y:DAC_BUF-y+x;
  }

  size_t free(){
    return DAC_BUF-1-cnt();
  }

  void run(){
    uint32_t us=1000000UL/(rate?rate:PLAY_RATE);
    uint32_t next=micros();

    while(active){
      if(h==t){vTaskDelay(1);continue;}

      int16_t s=b[t];
      t=(t+1)%DAC_BUF;

      int32_t v=((int32_t)s*825)/1000;
      int o=constrain((v+32768+128)>>8,0,255);

      while((int32_t)(next-micros())>0)
        delayMicroseconds(1);

      dacWrite(AUDIO_DAC_PIN,o);
      next+=us;

      if(!(t&63))taskYIELD();
    }

    dacMute();
    active=false;
    task=nullptr;
    vTaskDelete(nullptr);
  }

  static void fn(void*x){((DACOut*)x)->run();}

public:
  void setAudioInfo(AudioInfo i)override{
    info=i;
    AudioStream::setAudioInfo(i);
    rate=i.sample_rate?i.sample_rate:PLAY_RATE;
  }

  int availableForWrite()override{return free()*2;}

  void start(){
    h=t=0;
    dacMute();
    active=true;

    if(!task)
      xTaskCreatePinnedToCore(
        fn,"TARS_DAC",4096,this,2,&task,1
      );
  }

  bool empty(){return h==t;}

  void stop(){
    active=false;
    uint32_t z=millis();

    while(task&&millis()-z<2000)
      vTaskDelay(1);

    dacMute();
  }

  size_t write(const uint8_t*d,size_t n)override{
    if(!d||!active||info.bits_per_sample!=16)return 0;

    size_t ch=info.channels,bpf=ch*2;
    size_t frames=n/bpf,done=0;

    while(done<frames){
      size_t sp=free();

      if(!sp){
        vTaskDelay(1);
        continue;
      }

      size_t c=min(sp,frames-done);

      for(size_t i=0;i<c;i++){
        size_t k=done+i;
        int16_t s;

        if(ch==1)
          s=d[k*2]|((uint16_t)d[k*2+1]<<8);
        else
          s=((int16_t)(d[k*4]|((uint16_t)d[k*4+1]<<8))+
             (int16_t)(d[k*4+2]|((uint16_t)d[k*4+3]<<8)))/2;

        b[h]=s;
        h=(h+1)%DAC_BUF;
      }

      done+=c;
      taskYIELD();
    }

    return n;
  }
}dacOut;

MP3DecoderHelix decoder;
EncodedAudioStream mp3(&dacOut,&decoder);

/* ===== PLAY ===== */
bool playMP3(){
  File f=LittleFS.open(PLAY_FILE);
  if(!f)return false;

  playing=true;
  oledPos=oledPage=0;
  oledTick=millis();

  if(!dacOK)dacOK=initDAC();

  if(!dacOK){
    f.close();playing=false;return false;
  }

  if(!mp3.begin()){
    f.close();playing=false;return false;
  }

  oledBase(singMode?"SINGING":"SPEAKING");
  dacOut.start();

  StreamCopy cp(mp3,f,BUF);
  uint32_t st=millis();

  while(f.available()&&millis()-st<120000){
    if(!cp.copy())delay(1);
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
  LittleFS.remove(PLAY_FILE);
  oledBase("STANDBY","LISTENING...");
  return true;
}

/* ===== SING ===== */
bool singRequest(String q){
  q.toLowerCase();
  return q.indexOf("nyanyi")>=0||
         q.indexOf("bernyanyi")>=0||
         q.indexOf("nyanyikan")>=0;
}

/* ===== PROCESS ===== */
void processQuestion(const String&q){
  const char*local=offlineFile(q);
  String answer=offlineAnswer(q);
  singMode=singRequest(q);

  if(local){
    if(!playOffline(local,answer))
      oledBase("STANDBY","OFFLINE AUDIO ERROR");
    singMode=false;
    return;
  }

  if(!answer.length())answer=ask(q);

  if(!answer.length()){
    oledBase("STANDBY","ASK ERROR");
    return;
  }

  oledText=answer;
  oledPos=oledPage=0;

  String url=String(TARS_CLOUD_URL)+(singMode?"/sing":"/tts");

  if(downloadMP3(url,singMode?q:answer))
    playMP3();
  else
    oledBase("STANDBY","AUDIO ERROR");

  singMode=false;
}

/* ===== SETUP ===== */
void setup(){
  Serial.begin(SERIAL_BAUD);

  Wire.begin(OLED_SDA,OLED_SCL);
  Wire.setClock(400000);

  oledOK=oled.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR);
  if(oledOK)oledBase("BOOT");

  LittleFS.begin(true);
  dacOK=initDAC();
  micOK=initMic();

  Serial.println("TARS: BLUETOOTH DISABLED");
  Serial.println("TARS: 3 VOICE TEMPLATES ENABLED");
  Serial.println("TARS: TARS = ONLINE STT");
  Serial.println("TARS: HIDUP JOKOWI = OFFLINE");
  Serial.println("TARS: EXPERIENCE = OFFLINE");
  Serial.println("TARS: UNKNOWN = BLOCKED");

  wifiManagerBegin();
  ensureWiFi();

  while(!syncTime()){
    oledBase("BOOT","NTP RETRY...");
    delay(2000);
    if(WiFi.status()!=WL_CONNECTED)ensureWiFi();
  }

  ntpOK=true;

  Serial.println("TARS: WIFI + NTP READY");
  Serial.println("TARS: ENTERING OFFLINE STANDBY");

  oledBase("STANDBY","LISTENING...");
}

/* ===== LOOP ===== */
void loop(){
  if(playing)return;

  if(WiFi.status()!=WL_CONNECTED){
    if(!wifiOK()){
      oledBase("STANDBY","WIFI ERROR");
      delay(1000);
      return;
    }
    Serial.println("TARS: WIFI RECONNECTED - NTP NOT REPEATED");
  }

  if(recordMic()){
    InputType type=classifyInput();

    switch(type){

      case INPUT_WAKE_TARS:{
        Serial.println("TARS: TARS DETECTED -> STT ONLINE");

        String q=stt();
        LittleFS.remove(STT_FILE);

        if(q.length()){
          Serial.printf("TARS: STT = %s\r\n",q.c_str());
          processQuestion(q);
        }else{
          oledBase("STANDBY","NO INPUT");
        }
        break;
      }

      case INPUT_OFFLINE_JOKOWI:
        Serial.println("TARS: OFFLINE COMMAND -> HIDUP JOKOWI");
        LittleFS.remove(STT_FILE);
        processQuestion("hidup Jokowi");
        break;

      case INPUT_OFFLINE_EXP:
        Serial.println("TARS: OFFLINE COMMAND -> EXPERIENCE");
        LittleFS.remove(STT_FILE);
        processQuestion("ceritakan pengalamanmu");
        break;

      default:
        Serial.println("TARS: BLOCKED -> NO STT");
        LittleFS.remove(STT_FILE);
        oledBase("STANDBY","LISTENING...");
        break;
    }

  }else{
    oledListen();
  }

  delay(1);
}
