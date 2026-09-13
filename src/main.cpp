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
#define HAS_EXP_MP3 1
#else
#define HAS_EXP_MP3 0
#endif
#include "TARS.h"
#include "HIDUP_JOKOWI.h"
#include "PENGALAMAN.h"
#include "config.h"
#include "wifi_manager.h"

#define I2S_PORT I2S_NUM_1
#define SCK 18
#define WS 19
#define SD 34
#define DAC 26
#define RATE 16000
#define PLAY_RATE 22050
#define REC_SAMPLES 28800
#define FFT_N 256
#define HOP 160
#define MEL 20
#define MFCC 13
#define FRAMES 32
#define THRESHOLD 31.0f
#define MARGIN 3.0f
#define STT_FILE "/stt.wav"
#define PLAY_FILE "/tars.mp3"
#define BUF 1024
#define DAC_BUF 16384
#define PREROLL 500
#define REC_MAX 5000
#define REC_MIN 700
#define SILENCE 600
#define LISTEN_MAX 15000
#define MIC_TH 14000
#define MIC_SIL 12000

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);
bool oledOK=false,micOK=false,dacOK=false,playing=false,singMode=false,ntpOK=false;
String oledText;
size_t oledPos=0,oledPage=0;
uint32_t oledTick=0,dotTick=0;
byte dots=1;

static int16_t audioBuf[REC_SAMPLES];
static float re[FFT_N],im[FFT_N],feat[FRAMES][MFCC];
static float energy[REC_SAMPLES/HOP+2],melFilter[MEL][FFT_N/2+1];
static float dct[MFCC][MEL],win[FFT_N];
static bool tables=false;

enum Command{UNKNOWN,TARS,HIDUP_JOKOWI,PENGALAMAN};
const int8_t* templ[]={TARS_TEMPLATE,HIDUP_JOKOWI_TEMPLATE,PENGALAMAN_TEMPLATE};
const char* names[]={"TARS","HIDUP_JOKOWI","PENGALAMAN"};

void oledHeader(const char*s){
  if(!oledOK)return;
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
  String s;for(byte i=0;i<dots;i++)s+='.';
  oledBase("STANDBY",s);
}
void oledType(){
  if(!oledOK||oledPos>=oledText.length()||millis()-oledTick<15)return;
  oledTick=millis();
  oledHeader(singMode?"SINGING":"SPEAKING");
  int x=3,y=27;size_t i=oledPage;
  while(i<oledPos){
    char c=oledText[i++];
    if(c=='\n'||x>121){x=3;y+=8;if(c=='\n')continue;}
    if(y>59){oledPage=i-1;oledHeader(singMode?"SINGING":"SPEAKING");x=3;y=27;i=oledPage;}
    if(y<=59){oled.setCursor(x,y);oled.write(c);x=oled.getCursorX();}
  }
  byte n=0;
  while(oledPos<oledText.length()&&n<5){
    char c=oledText[oledPos++];
    if(c=='\n'||x>121){x=3;y+=8;if(c=='\n')continue;}
    if(y>59){oledPage=oledPos-1;oledHeader(singMode?"SINGING":"SPEAKING");x=3;y=27;continue;}
    oled.setCursor(x,y);oled.write(c);x=oled.getCursorX();n++;
  }
  oled.display();
}

/* ================= MFCC CLASSIFIER ================= */

float hzMel(float h){return 2595.0f*log10f(1.0f+h/700.0f);}
float melHz(float m){return 700.0f*(powf(10.0f,m/2595.0f)-1.0f);}

void buildTables(){
  for(int n=0;n<FFT_N;n++)win[n]=.5f-.5f*cosf(2*PI*n/FFT_N);
  for(int k=0;k<MFCC;k++)
    for(int m=0;m<MEL;m++)
      dct[k][m]=cosf(PI/MEL*(m+.5f)*k);

  float lo=hzMel(80),hi=hzMel(7600);
  int bin[MEL+2];
  for(int i=0;i<MEL+2;i++){
    float hz=melHz(lo+(hi-lo)*i/(MEL+1));
    bin[i]=constrain((int)floorf((FFT_N+1)*hz/RATE),0,FFT_N/2);
  }

  memset(melFilter,0,sizeof(melFilter));
  for(int m=1;m<=MEL;m++){
    int a=bin[m-1],b=bin[m],c=bin[m+1];
    for(int k=a;k<=b;k++)if(b>a)melFilter[m-1][k]=(float)(k-a)/(b-a);
    for(int k=b;k<=c;k++)if(c>b)melFilter[m-1][k]=(float)(c-k)/(c-b);
  }
  tables=true;
}

void fft(){
  int j=0;
  for(int i=1;i<FFT_N;i++){
    int b=FFT_N>>1;
    while(j&b){j^=b;b>>=1;}
    j^=b;
    if(i<j){
      float x=re[i];re[i]=re[j];re[j]=x;
      x=im[i];im[i]=im[j];im[j]=x;
    }
  }

  for(int len=2;len<=FFT_N;len<<=1){
    float a=-2*PI/len,wr0=cosf(a),wi0=sinf(a);
    for(int i=0;i<FFT_N;i+=len){
      float wr=1,wi=0;
      for(int j=0;j<len/2;j++){
        int u=i+j,v=u+len/2;
        float vr=re[v]*wr-im[v]*wi;
        float vi=re[v]*wi+im[v]*wr;
        float ur=re[u],ui=im[u];
        re[u]=ur+vr;im[u]=ui+vi;
        re[v]=ur-vr;im[v]=ui-vi;
        float nw=wr*wr0-wi*wi0;
        wi=wr*wi0+wi*wr0;wr=nw;
      }
    }
  }
}

bool trimSpeech(int16_t*y,int n,int&first,int&last){
  float mx=0;
  int frames=n/HOP;
  for(int f=0;f<frames;f++){
    int s=f*HOP;float e=0;
    for(int i=0;i<400&&s+i<n;i++){
      float v=(float)y[s+i]/32768.0f;e+=v*v;
    }
    e=sqrtf(e/400.0f);energy[f]=e;if(e>mx)mx=e;
  }
  if(mx<.008f)return false;

  float th=mx*.20f;int a=-1,b=-1;
  for(int f=0;f<frames;f++)if(energy[f]>th){
    if(a<0)a=f;b=f;
  }
  if(a<0||b<=a)return false;
  first=a*HOP;last=min(n,(b+1)*HOP+400);
  return last-first>=2400;
}

bool makeFeatures(int16_t*in,int n){
  int first,last;
  if(!trimSpeech(in,n,first,last))return false;

  int len=last-first;
  if(len<FFT_N)return false;

  int count=(len-FFT_N)/HOP+1;
  if(count<2)return false;
  count=min(count,128);

  static float raw[128][MFCC];

  for(int f=0;f<count;f++){
    int s=first+f*HOP;
    for(int i=0;i<FFT_N;i++){
      float x=(float)in[s+i]/32768.0f;
      float p=i?(float)in[s+i-1]/32768.0f:(s?(float)in[s-1]/32768.0f:0);
      re[i]=(x-.97f*p)*win[i];im[i]=0;
    }
    fft();

    float m[MEL]={};
    for(int k=0;k<=FFT_N/2;k++){
      float p=re[k]*re[k]+im[k]*im[k];
      for(int j=0;j<MEL;j++)m[j]+=p*melFilter[j][k];
    }

    for(int j=0;j<MEL;j++)m[j]=logf(m[j]+1e-8f);
    for(int c=0;c<MFCC;c++){
      float v=0;
      for(int j=0;j<MEL;j++)v+=dct[c][j]*m[j];
      raw[f][c]=v;
    }
  }

  for(int c=0;c<MFCC;c++){
    float mean=0,var=0;
    for(int f=0;f<count;f++)mean+=raw[f][c];
    mean/=count;
    for(int f=0;f<count;f++){
      float d=raw[f][c]-mean;var+=d*d;
    }
    float sd=sqrtf(var/count)+1e-5f;
    for(int f=0;f<count;f++)raw[f][c]=(raw[f][c]-mean)/sd;
  }

  for(int t=0;t<FRAMES;t++){
    float pos=(float)t*(count-1)/(FRAMES-1);
    int a=floorf(pos),b=min(count-1,a+1);float q=pos-a;
    for(int c=0;c<MFCC;c++){
      float v=raw[a][c]*(1-q)+raw[b][c]*q;
      feat[t][c]=constrain(v,-3.0f,3.0f)/3.0f*127.0f;
    }
  }
  return true;
}

float distanceTo(const int8_t*t){
  float sum=0;
  for(int i=0;i<FRAMES*MFCC;i++)
    sum+=fabsf(feat[i/MFCC][i%MFCC]-(float)(int8_t)pgm_read_byte(&t[i]));
  return sum/(FRAMES*MFCC);
}

Command classify(){
  float d[3];
  for(byte i=0;i<3;i++)d[i]=distanceTo(templ[i]);

  byte best=0;
  for(byte i=1;i<3;i++)if(d[i]<d[best])best=i;

  byte second=best?0:1;
  for(byte i=0;i<3;i++)
    if(i!=best&&d[i]<d[second])second=i;

  Serial.printf("KWS: %s %.1f / %.1f / %.1f\r\n",
    names[best],d[0],d[1],d[2]);

  if(d[best]>THRESHOLD||d[second]-d[best]<MARGIN)return UNKNOWN;
  return (Command)(best+1);
}

/* ================= MICROPHONE RECORD ================= */

bool initMic(){
  i2s_config_t c={};
  c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_RX);
  c.sample_rate=RATE;
  c.bits_per_sample=I2S_BITS_PER_SAMPLE_32BIT;
  c.channel_format=I2S_CHANNEL_FMT_ONLY_RIGHT;
  c.communication_format=I2S_COMM_FORMAT_I2S;
  c.dma_buf_count=2;c.dma_buf_len=256;

  if(i2s_driver_install(I2S_PORT,&c,0,nullptr)!=ESP_OK)return false;

  i2s_pin_config_t p={
    I2S_PIN_NO_CHANGE,SCK,WS,I2S_PIN_NO_CHANGE,SD
  };
  return i2s_set_pin(I2S_PORT,&p)==ESP_OK;
}

void put16(uint8_t*p,uint16_t v){p[0]=v;p[1]=v>>8;}
void put32(uint8_t*p,uint32_t v){
  p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;
}

void wavHeader(File&f,uint32_t n){
  uint8_t h[44]={};
  memcpy(h,"RIFF",4);put32(h+4,n+36);
  memcpy(h+8,"WAVEfmt ",8);put32(h+16,16);
  put16(h+20,1);put16(h+22,1);put32(h+24,RATE);
  put32(h+28,RATE*2);put16(h+32,2);put16(h+34,16);
  memcpy(h+36,"data",4);put32(h+40,n);
  f.seek(0);f.write(h,44);
}

bool recordMic(){
  if(!micOK)return false;

  oledBase("STANDBY","LISTENING...");
  LittleFS.remove(STT_FILE);
  File f=LittleFS.open(STT_FILE,FILE_WRITE);
  if(!f)return false;

  uint8_t zero[44]={};f.write(zero,44);

  static int16_t pre[PREROLL*RATE/1000];
  size_t pp=0,pc=0;
  int32_t raw[BUF/4];int16_t pcm[BUF/4];
  uint32_t samples=0,start=millis(),voiceStart=0,lastVoice=0;
  byte active=0;bool voice=false;

  while((!voice&&millis()-start<LISTEN_MAX)||
        (voice&&millis()-voiceStart<REC_MAX)){
    oledListen();

    size_t bytes=0;
    if(i2s_read(I2S_PORT,raw,sizeof(raw),&bytes,
                pdMS_TO_TICKS(50))!=ESP_OK)continue;

    size_t c=bytes/4;int32_t peak=0;

    for(size_t i=0;i<c;i++){
      int32_t v=constrain(raw[i]>>16,-32768,32767);
      pcm[i]=v;peak=max(peak,abs(v));
    }

    if(!voice){
      for(size_t i=0;i<c;i++){
        pre[pp]=pcm[i];pp=(pp+1)% (PREROLL*RATE/1000);
        if(pc<PREROLL*RATE/1000)pc++;
      }

      if(peak>=MIC_TH&&++active>=2){
        voice=true;voiceStart=lastVoice=millis();
        size_t st=pc==(PREROLL*RATE/1000)?pp:0;

        for(size_t i=0;i<pc;i++){
          size_t k=(st+i)%(PREROLL*RATE/1000);
          f.write((uint8_t*)&pre[k],2);
        }
        samples+=pc;
        f.write((uint8_t*)pcm,c*2);samples+=c;
      }else if(peak<MIC_TH)active=0;
    }else{
      f.write((uint8_t*)pcm,c*2);samples+=c;
      if(peak>=MIC_SIL)lastVoice=millis();
      if(millis()-voiceStart>=REC_MIN&&
         millis()-lastVoice>=SILENCE)break;
    }
    yield();
  }

  wavHeader(f,samples*2);f.close();

  if(!voice||!samples){
    LittleFS.remove(STT_FILE);return false;
  }
  return true;
}

/* ================= WIFI + NTP ================= */

bool wifiOK(){
  return WiFi.status()==WL_CONNECTED||wifiManagerConnect(false);
}

void ensureWiFi(){
  while(WiFi.status()!=WL_CONNECTED){
    oledBase("BOOT","WAITING WIFI...");
    if(!wifiManagerConnect(false)){
      oledBase("BOOT","WIFI RETRY...");
      delay(1000);
    }
  }
}

bool syncTime(){
  configTime(7*3600,0,
    "pool.ntp.org","time.nist.gov","time.google.com");

  for(byte a=0;a<4;a++){
    Serial.printf("TARS: NTP %d/4\r\n",a+1);
    for(byte i=0;i<20;i++){
      time_t n=time(nullptr);
      if(n>=1704067200){
        struct tm t;localtime_r(&n,&t);
        Serial.printf("TARS: WIB %04d-%02d-%02d %02d:%02d:%02d\r\n",
          t.tm_year+1900,t.tm_mon+1,t.tm_mday,
          t.tm_hour,t.tm_min,t.tm_sec);
        return ntpOK=true;
      }
      delay(500);
    }
  }
  return ntpOK=false;
}

/* ================= STT ================= */

String stt(){
  if(!wifiOK()||!ntpOK)return "";

  File f=LittleFS.open(STT_FILE);
  if(!f)return "";

  const char*b="----TARSSTT";
  String head="--"+String(b)+
    "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"stt.wav\""
    "\r\nContent-Type: audio/wav\r\n\r\n";
  String tail="\r\n--"+String(b)+"--\r\n";

  WiFiClientSecure c;c.setInsecure();c.setTimeout(15000);

  String host=TARS_CLOUD_URL;
  int p=host.indexOf("://");
  if(p>=0)host=host.substring(p+3);
  p=host.indexOf('/');
  if(p>=0)host=host.substring(0,p);

  if(!c.connect(host.c_str(),443)){
    f.close();return "";
  }

  size_t total=head.length()+f.size()+tail.length();

  c.printf(
    "POST /stt HTTP/1.1\r\nHost: %s\r\n"
    "Content-Type: multipart/form-data; boundary=%s\r\n"
    "Content-Length: %u\r\nConnection: close\r\n\r\n",
    host.c_str(),b,(unsigned)total);

  c.print(head);

  uint8_t z[BUF];
  while(f.available()){
    size_t n=f.read(z,sizeof(z));
    if(c.write(z,n)!=n){
      f.close();c.stop();return "";
    }
  }

  f.close();c.print(tail);

  uint32_t t=millis();
  while(!c.available()&&c.connected()&&millis()-t<20000)delay(5);

  if(!c.available()){c.stop();return "";}

  String status=c.readStringUntil('\n');
  status.trim();

  String body;
  while(c.connected()||c.available())body+=c.readString();
  c.stop();

  if(status.indexOf(" 200 ")<0||!body.length())return "";

  int p1=body.indexOf('{'),p2=body.lastIndexOf('}');
  if(p1<0||p2<p1)return "";
  body=body.substring(p1,p2+1);

  JsonDocument j;
  if(deserializeJson(j,body))return "";

  String q=j["text"].as<String>();
  if(!q.length())q=j["transcript"].as<String>();
  q.trim();return q;
}

/* ================= ASK ================= */

String ask(const String&q){
  if(!wifiOK()||!ntpOK)return "";

  WiFiClientSecure c;c.setInsecure();
  HTTPClient h;

  if(!h.begin(c,String(TARS_CLOUD_URL)+"/ask"))return "";
  h.setTimeout(15000);
  h.addHeader("Content-Type","application/json");

  JsonDocument j;j["question"]=q;
  String body;serializeJson(j,body);

  int code=h.POST(body);
  if(code<200||code>=300){h.end();return "";}

  String r=h.getString();h.end();

  JsonDocument x;
  if(deserializeJson(x,r))return "";

  String a=x["response"].as<String>();
  if(!a.length())a=x["answer"].as<String>();
  return a;
}

/* ================= LOCAL ANSWER ================= */

String localAnswer(String q){
  q.toLowerCase();q.trim();

  if(q.indexOf("hidup jokowi")>=0||
     q.indexOf("jokowi hidup")>=0)
    return "Saya akan lawan.";

  if(q.indexOf("jam berapa")>=0||
     q.indexOf("tanggal berapa")>=0){
    time_t n=time(nullptr);
    struct tm t;localtime_r(&n,&t);

    const char*d[]={
      "Minggu","Senin","Selasa","Rabu",
      "Kamis","Jumat","Sabtu"
    };
    const char*m[]={
      "Januari","Februari","Maret","April","Mei","Juni",
      "Juli","Agustus","September","Oktober","November","Desember"
    };

    char s[160];
    snprintf(s,sizeof(s),
      "Pukul %02d lewat %02d menit WIB. %s, %02d %s tahun %04d.",
      t.tm_hour,t.tm_min,d[t.tm_wday],
      t.tm_mday,m[t.tm_mon],t.tm_year+1900);
    return s;
  }

  if(q.indexOf("ceritakan pengalamanmu")>=0||
     q.indexOf("ceritakan pengalaman")>=0||
     q.indexOf("pengalaman kamu")>=0)
    return "Selama menjadi robot Interstellar, saya telah melewati banyak hal. Saya belajar tentang perjalanan antarbintang, menghadapi bahaya, dan menemani manusia dalam misi yang jauh dari Bumi.";

  return "";
}

/* ================= AUDIO ================= */

bool initDAC(){
  pinMode(DAC,OUTPUT);dacWrite(DAC,0);return true;
}
void dacMute(){dacWrite(DAC,0);}

class DACOut:public AudioStream{
  AudioInfo info;
  int16_t buf[DAC_BUF];
  volatile size_t head=0,tail=0;
  volatile bool active=false;
  TaskHandle_t task=nullptr;
  uint32_t rate=PLAY_RATE;

  size_t count(){
    size_t h=head,t=tail;
    return h>=t?h-t:DAC_BUF-t+h;
  }
  size_t freeBuf(){return DAC_BUF-1-count();}

  static void taskFn(void*x){((DACOut*)x)->run();}

  void run(){
    uint32_t us=1000000UL/(rate?rate:PLAY_RATE),next=micros();

    while(active){
      if(head==tail){vTaskDelay(1);continue;}

      int16_t s=buf[tail];
      tail=(tail+1)%DAC_BUF;

      int32_t v=(int32_t)s*825/1000;
      dacWrite(DAC,constrain((v+32768+128)>>8,0,255));

      while((int32_t)(next-micros())>0)
        delayMicroseconds(1);

      next+=us;
      if(!(tail&63))taskYIELD();
    }

    dacMute();active=false;task=nullptr;
    vTaskDelete(nullptr);
  }

public:
  void setAudioInfo(AudioInfo i)override{
    info=i;AudioStream::setAudioInfo(i);
    rate=i.sample_rate?i.sample_rate:PLAY_RATE;
  }

  int availableForWrite()override{return freeBuf()*2;}

  void start(){
    head=tail=0;dacMute();active=true;
    if(!task)
      xTaskCreatePinnedToCore(
        taskFn,"TARS_DAC",4096,this,2,&task,1);
  }

  bool empty(){return head==tail;}

  void stop(){
    active=false;uint32_t t=millis();
    while(task&&millis()-t<2000)vTaskDelay(1);
    dacMute();
  }

  size_t write(const uint8_t*d,size_t n)override{
    if(!d||!active||info.bits_per_sample!=16)return 0;

    size_t ch=info.channels,frames=n/(ch*2),done=0;

    while(done<frames){
      size_t space=freeBuf();
      if(!space){vTaskDelay(1);continue;}

      size_t c=min(space,frames-done);

      for(size_t i=0;i<c;i++){
        size_t k=done+i;
        int16_t s;

        if(ch==1)
          s=d[k*2]|((uint16_t)d[k*2+1]<<8);
        else{
          int16_t l=d[k*4]|((uint16_t)d[k*4+1]<<8);
          int16_t r=d[k*4+2]|((uint16_t)d[k*4+3]<<8);
          s=(l+r)/2;
        }

        buf[head]=s;
        head=(head+1)%DAC_BUF;
      }

      done+=c;taskYIELD();
    }
    return n;
  }
};

DACOut dacOut;
MP3DecoderHelix decoder;
EncodedAudioStream mp3(&dacOut,&decoder);

bool playMP3(){
  File f=LittleFS.open(PLAY_FILE);
  if(!f)return false;

  playing=true;oledPos=oledPage=0;oledTick=millis();
  if(!dacOK)dacOK=initDAC();

  if(!dacOK||!mp3.begin()){
    f.close();playing=false;return false;
  }

  oledBase(singMode?"SINGING":"SPEAKING");
  dacOut.start();

  StreamCopy cp(mp3,f,BUF);
  uint32_t start=millis();

  while(f.available()&&millis()-start<120000){
    if(!cp.copy())delay(1);
    oledType();yield();
  }

  mp3.end();f.close();

  while(!dacOut.empty()){
    oledType();delay(1);
  }

  dacOut.stop();
  playing=false;
  LittleFS.remove(PLAY_FILE);
  oledBase("STANDBY","LISTENING...");
  return true;
}

bool downloadMP3(const String&url,const String&text){
  if(!wifiOK()||!ntpOK)return false;

  WiFiClientSecure c;c.setInsecure();
  HTTPClient h;

  if(!h.begin(c,url))return false;
  h.setTimeout(60000);
  h.addHeader("Content-Type","application/json");

  JsonDocument j;
  if(url.endsWith("/sing"))j["prompt"]=text;
  else j["text"]=text;

  String body;serializeJson(j,body);

  int code=h.POST(body);
  if(code<200||code>=300){h.end();return false;}

  LittleFS.remove(PLAY_FILE);
  File f=LittleFS.open(PLAY_FILE,FILE_WRITE);

  if(!f){h.end();return false;}

  WiFiClient*s=h.getStreamPtr();
  uint8_t z[BUF];
  int len=h.getSize();
  size_t total=0;
  uint32_t last=millis();

  while(h.connected()&&(len>0||len==-1)){
    size_t n=s->available();

    if(n){
      n=min(n,sizeof(z));
      int r=s->readBytes(z,n);

      if(r>0){
        f.write(z,r);total+=r;
        if(len>0)len-=r;
        last=millis();
      }
    }else{
      if(millis()-last>10000)break;
      delay(1);
    }
    yield();
  }

  f.close();h.end();
  return total>0;
}

/* ================= COMMAND ================= */

bool isSing(const String&q){
  String s=q;s.toLowerCase();
  return s.indexOf("nyanyi")>=0||
         s.indexOf("bernyanyi")>=0||
         s.indexOf("nyanyikan")>=0;
}

void answer(const String&q){
  String l=q;l.toLowerCase();
  singMode=isSing(q);

  /* OFFLINE JOKOWI */
  if(l.indexOf("jokowi")>=0){
    oledText="Saya akan lawan.";
    oledPos=oledPage=0;singMode=false;

    if(embeddedMP3())playMP3();
    else oledBase("STANDBY","AUDIO ERROR");
    return;
  }

  /* OFFLINE EXPERIENCE */
  if(l.indexOf("pengalaman")>=0){
    oledText=
      "Selama menjadi robot Interstellar, saya telah melewati banyak hal. "
      "Saya belajar tentang perjalanan antarbintang, menghadapi bahaya, "
      "dan menemani manusia dalam misi yang jauh dari Bumi.";

    oledPos=oledPage=0;singMode=false;

#if HAS_EXP_MP3
    if(embeddedExperienceMP3())playMP3();
    else oledBase("STANDBY","AUDIO ERROR");
    return;
#else
    Serial.println("TARS: EXPERIENCE MP3 NOT INSTALLED");
#endif
  }

  /* JAM / TANGGAL SELALU LOKAL */
  if(l.indexOf("jam")>=0||l.indexOf("tanggal")>=0){
    String a=localAnswer(q);
    if(a.length()){
      oledText=a;oledPos=oledPage=0;singMode=false;
      if(!wifiOK()||!ntpOK){
        oledBase("STANDBY","TIME ERROR");return;
      }
      if(downloadMP3(String(TARS_CLOUD_URL)+"/tts",a))playMP3();
      else oledBase("STANDBY","AUDIO ERROR");
      return;
    }
  }

  String a=ask(q);
  if(!a.length()){
    oledBase("STANDBY","ASK ERROR");
    return;
  }

  oledText=a;oledPos=oledPage=0;

  String url=String(TARS_CLOUD_URL)+(singMode?"/sing":"/tts");

  if(downloadMP3(url,singMode?q:a))playMP3();
  else oledBase("STANDBY","AUDIO ERROR");

  singMode=false;
}

/* ================= EMBEDDED AUDIO ================= */

bool embeddedMP3(){
  LittleFS.remove(PLAY_FILE);
  File f=LittleFS.open(PLAY_FILE,FILE_WRITE);
  if(!f)return false;

  size_t n=f.write(
    TARS_ELEVENLABS_MP3,
    TARS_ELEVENLABS_MP3_LEN);

  f.close();
  return n==TARS_ELEVENLABS_MP3_LEN;
}

#if HAS_EXP_MP3
bool embeddedExperienceMP3(){
  LittleFS.remove(PLAY_FILE);
  File f=LittleFS.open(PLAY_FILE,FILE_WRITE);
  if(!f)return false;

  size_t n=f.write(
    TARS_EXPERIENCE_MP3,
    TARS_EXPERIENCE_MP3_LEN);

  f.close();
  return n==TARS_EXPERIENCE_MP3_LEN;
}
#endif

/* ================= SETUP ================= */

void setup(){
  Serial.begin(SERIAL_BAUD);

  Wire.begin(OLED_SDA,OLED_SCL);
  Wire.setClock(400000);

  oledOK=oled.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR);
  if(oledOK)oledBase("BOOT");

  LittleFS.begin(true);

  dacOK=initDAC();
  micOK=initMic();
  buildTables();

  Serial.println("TARS: BLUETOOTH DISABLED");
  Serial.println("TARS: MFCC KWS READY");
  Serial.println("TARS: TARS = ONLINE STT");
  Serial.println("TARS: HIDUP_JOKOWI = OFFLINE");
  Serial.println("TARS: PENGALAMAN = OFFLINE");
  Serial.println("TARS: UNKNOWN = BLOCKED");

#if HAS_EXP_MP3
  Serial.println("TARS: EXPERIENCE MP3 = INSTALLED");
#else
  Serial.println("TARS: EXPERIENCE MP3 = NOT INSTALLED");
#endif

  wifiManagerBegin();
  ensureWiFi();

  while(!syncTime()){
    oledBase("BOOT","NTP RETRY...");
    delay(2000);
    if(WiFi.status()!=WL_CONNECTED)ensureWiFi();
  }

  ntpOK=true;
  Serial.println("TARS: WIFI + NTP READY");
  oledBase("STANDBY","LISTENING...");
}

/* ================= LOOP ================= */

void loop(){
  if(playing)return;

  if(WiFi.status()!=WL_CONNECTED){
    if(!wifiOK()){
      oledBase("STANDBY","WIFI ERROR");
      delay(1000);
      return;
    }
    Serial.println("TARS: WIFI RECONNECTED");
  }

  if(!recordMic()){
    oledListen();
    delay(1);
    return;
  }

  if(!makeFeatures(audioBuf,REC_SAMPLES)){
    Serial.println("KWS: SILENCE/NOISE -> IGNORE");
    LittleFS.remove(STT_FILE);
    return;
  }

  Command c=classify();

  switch(c){
    case TARS:{
      Serial.println("TARS: WAKE -> STT ONLINE");

      String q=stt();
      LittleFS.remove(STT_FILE);

      if(q.length()){
        Serial.printf("TARS: STT = %s\r\n",q.c_str());
        answer(q);
      }else{
        oledBase("STANDBY","NO INPUT");
      }
      break;
    }

    case HIDUP_JOKOWI:
      Serial.println("TARS: OFFLINE -> HIDUP JOKOWI");
      LittleFS.remove(STT_FILE);
      answer("hidup Jokowi");
      break;

    case PENGALAMAN:
      Serial.println("TARS: OFFLINE -> PENGALAMAN");
      LittleFS.remove(STT_FILE);
      answer("ceritakan pengalamanmu");
      break;

    default:
      Serial.println("TARS: UNKNOWN -> NO STT");
      LittleFS.remove(STT_FILE);
      oledBase("STANDBY","LISTENING...");
      break;
  }

  delay(1);
}
