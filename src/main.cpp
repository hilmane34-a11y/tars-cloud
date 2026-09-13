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
#include "TARS.h"
#include "HIDUP_JOKOWI.h"
#include "PENGALAMAN.h"
#include "config.h"
#include "wifi_manager.h"

#define I2S_PORT I2S_NUM_1
#define BCLK 18
#define WS   19
#define SD   34
#define DAC  26
#define RATE 16000
#define PLAY_RATE 22050
#define FFT_N 256
#define HOP 160
#define MEL 20
#define MFCC 13
#define KWS_FRAMES 32
#define THRESHOLD 31.0f
#define MARGIN 3.0f
#define STT_FILE "/stt.wav"
#define PLAY_FILE "/tars.mp3"
#define IO_BUF 1024
#define DAC_BUF 1024
#define PREROLL_MS 300
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

static int16_t frameBuf[FFT_N];
static float re[FFT_N],im[FFT_N];
static float kwsFeat[KWS_FRAMES][MFCC];
static int16_t pre[RATE*PREROLL_MS/1000];

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
    if(y>59){oledPage=i-1;i=oledPage;x=3;y=27;oledHeader(singMode?"SINGING":"SPEAKING");}
    if(y<=59){oled.setCursor(x,y);oled.write(c);x=oled.getCursorX();}
  }
  byte n=0;
  while(oledPos<oledText.length()&&n<5){
    char c=oledText[oledPos++];
    if(c=='\n'||x>121){x=3;y+=8;if(c=='\n')continue;}
    if(y>59){oledPage=oledPos-1;x=3;y=27;oledHeader(singMode?"SINGING":"SPEAKING");continue;}
    oled.setCursor(x,y);oled.write(c);x=oled.getCursorX();n++;
  }
  oled.display();
}

float hzMel(float h){return 2595.0f*log10f(1.0f+h/700.0f);}

void fft(){
  int j=0;
  for(int i=1;i<FFT_N;i++){
    int b=FFT_N>>1;
    while(j&b){j^=b;b>>=1;}j^=b;
    if(i<j){
      float x=re[i];re[i]=re[j];re[j]=x;
      x=im[i];im[i]=im[j];im[j]=x;
    }
  }
  for(int len=2;len<=FFT_N;len<<=1){
    float a=-2.0f*PI/len,wr0=cosf(a),wi0=sinf(a);
    for(int i=0;i<FFT_N;i+=len){
      float wr=1,wi=0;
      for(int j=0;j<len/2;j++){
        int u=i+j,v=u+len/2;
        float vr=re[v]*wr-im[v]*wi,vi=re[v]*wi+im[v]*wr;
        float ur=re[u],ui=im[u];
        re[u]=ur+vr;im[u]=ui+vi;
        re[v]=ur-vr;im[v]=ui-vi;
        float nw=wr*wr0-wi*wi0;
        wi=wr*wi0+wi*wr0;wr=nw;
      }
    }
  }
}

void makeMFCCFrame(const int16_t*src,float*out){
  for(int i=0;i<FFT_N;i++){
    float x=(float)src[i]/32768.0f;
    float p=i?(float)src[i-1]/32768.0f:0;
    re[i]=(x-.97f*p)*(.5f-.5f*cosf(2.0f*PI*i/(FFT_N-1)));
    im[i]=0;
  }
  fft();

  float melEnergy[MEL]={};
  float lo=hzMel(80),hi=hzMel(7600);

  for(int k=0;k<=FFT_N/2;k++){
    float m=hzMel((float)k*RATE/FFT_N);
    float p=re[k]*re[k]+im[k]*im[k];

    for(int band=0;band<MEL;band++){
      float l=lo+(hi-lo)*band/(MEL+1);
      float c=lo+(hi-lo)*(band+1)/(MEL+1);
      float r=lo+(hi-lo)*(band+2)/(MEL+1);
      float w=0;
      if(m>l&&m<c)w=(m-l)/(c-l);
      else if(m>=c&&m<r)w=(r-m)/(r-c);
      if(w>0)melEnergy[band]+=p*w;
    }
  }

  for(int m=0;m<MEL;m++)melEnergy[m]=logf(melEnergy[m]+1e-8f);

  for(int c=0;c<MFCC;c++){
    float v=0;
    for(int m=0;m<MEL;m++)
      v+=melEnergy[m]*cosf(PI/MEL*(m+.5f)*c);
    out[c]=v;
  }
}

void normalizeFeatures(){
  for(int c=0;c<MFCC;c++){
    float mean=0,var=0;
    for(int f=0;f<KWS_FRAMES;f++)mean+=kwsFeat[f][c];
    mean/=KWS_FRAMES;

    for(int f=0;f<KWS_FRAMES;f++){
      float d=kwsFeat[f][c]-mean;
      var+=d*d;
    }

    float sd=sqrtf(var/KWS_FRAMES)+1e-5f;

    for(int f=0;f<KWS_FRAMES;f++)
      kwsFeat[f][c]=constrain(
        (kwsFeat[f][c]-mean)/sd,-3.0f,3.0f
      )/3.0f*127.0f;
  }
}

bool readMic(int16_t*pcm,size_t maxN,size_t&n,int32_t&peak){
  int32_t raw[IO_BUF/4];
  size_t bytes=0;

  if(i2s_read(I2S_PORT,raw,sizeof(raw),&bytes,pdMS_TO_TICKS(50))!=ESP_OK)
    return false;

  n=min(bytes/4,maxN);
  peak=0;

  for(size_t i=0;i<n;i++){
    pcm[i]=constrain(raw[i]>>16,-32768,32767);
    peak=max(peak,abs((int)pcm[i]));
  }
  return true;
}

bool recordKWS(){
  if(!micOK)return false;

  int16_t pcm[IO_BUF/4];
  const size_t PN=sizeof(pre)/2;
  size_t pp=0,pc=0,frames=0;
  bool voice=false;
  byte trigger=0;
  uint32_t start=millis();

  while(millis()-start<LISTEN_MAX){
    oledListen();

    size_t n;int32_t peak;
    if(!readMic(pcm,IO_BUF/4,n,peak))continue;

    if(!voice){
      for(size_t i=0;i<n;i++){
        pre[pp]=pcm[i];
        pp=(pp+1)%PN;
        if(pc<PN)pc++;
      }

      if(peak>=MIC_TH){
        if(++trigger>=2){
          voice=true;
          size_t st=pc==PN?pp:0;

          if(pc>=FFT_N){
            size_t mf=min((pc-FFT_N)/HOP+1,(size_t)KWS_FRAMES);

            for(size_t f=0;f<mf&&frames<KWS_FRAMES;f++){
              size_t base=(st+f*HOP)%PN;
              for(int j=0;j<FFT_N;j++)
                frameBuf[j]=pre[(base+j)%PN];
              makeMFCCFrame(frameBuf,kwsFeat[frames++]);
            }
          }
        }
      }else trigger=0;
    }else{
      size_t off=0;
      while(off+FFT_N<=n&&frames<KWS_FRAMES){
        for(int j=0;j<FFT_N;j++)frameBuf[j]=pcm[off+j];
        makeMFCCFrame(frameBuf,kwsFeat[frames++]);
        off+=HOP;
      }
      if(frames>=KWS_FRAMES)break;
    }
    yield();
  }

  if(frames<8)return false;

  while(frames<KWS_FRAMES){
    for(int c=0;c<MFCC;c++)
      kwsFeat[frames][c]=kwsFeat[frames-1][c];
    frames++;
  }

  normalizeFeatures();
  return true;
}

float distanceTo(const int8_t*t){
  float s=0;
  for(int i=0;i<KWS_FRAMES*MFCC;i++){
    int8_t v=(int8_t)pgm_read_byte(&t[i]);
    s+=fabsf(kwsFeat[i/MFCC][i%MFCC]-(float)v);
  }
  return s/(KWS_FRAMES*MFCC);
}

Command classify(){
  float d[3];
  for(byte i=0;i<3;i++)d[i]=distanceTo(templ[i]);

  byte best=0;
  for(byte i=1;i<3;i++)
    if(d[i]<d[best])best=i;

  byte second=best?0:1;
  for(byte i=0;i<3;i++)
    if(i!=best&&d[i]<d[second])second=i;

  Serial.printf("KWS: %s %.1f/%.1f/%.1f\r\n",
                names[best],d[0],d[1],d[2]);

  if(d[best]>THRESHOLD||d[second]-d[best]<MARGIN)
    return UNKNOWN;

  return (Command)(best+1);
}

bool initMic(){
  i2s_config_t c={};
  c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_RX);
  c.sample_rate=RATE;
  c.bits_per_sample=I2S_BITS_PER_SAMPLE_32BIT;
  c.channel_format=I2S_CHANNEL_FMT_ONLY_RIGHT;
  c.communication_format=I2S_COMM_FORMAT_I2S;
  c.dma_buf_count=2;
  c.dma_buf_len=256;

  if(i2s_driver_install(I2S_PORT,&c,0,nullptr)!=ESP_OK)
    return false;

  i2s_pin_config_t p={};
  p.mck_io_num=I2S_PIN_NO_CHANGE;
  p.bck_io_num=BCLK;
  p.ws_io_num=WS;
  p.data_out_num=I2S_PIN_NO_CHANGE;
  p.data_in_num=SD;

  return i2s_set_pin(I2S_PORT,&p)==ESP_OK;
}

bool recordSTT(){
  LittleFS.remove(STT_FILE);
  File f=LittleFS.open(STT_FILE,FILE_WRITE);
  if(!f)return false;

  uint8_t z44[44]={};
  f.write(z44,44);

  int16_t pcm[IO_BUF/4];
  const size_t PN=sizeof(pre)/2;
  size_t pp=0,pc=0,samples=0;
  bool voice=false;
  byte trigger=0;
  uint32_t start=millis(),vs=0,last=0;

  oledBase("LISTENING","SPEAK NOW");

  while((!voice&&millis()-start<LISTEN_MAX)||
        (voice&&millis()-vs<REC_MAX)){

    size_t n;int32_t peak;
    if(!readMic(pcm,IO_BUF/4,n,peak))continue;

    if(!voice){
      for(size_t i=0;i<n;i++){
        pre[pp]=pcm[i];
        pp=(pp+1)%PN;
        if(pc<PN)pc++;
      }

      if(peak>=MIC_TH){
        if(++trigger>=2){
          voice=true;
          vs=last=millis();
          size_t st=pc==PN?pp:0;

          for(size_t i=0;i<pc;i++){
            int16_t s=pre[(st+i)%PN];
            f.write((uint8_t*)&s,2);
            samples++;
          }

          f.write((uint8_t*)pcm,n*2);
          samples+=n;
        }
      }else trigger=0;
    }else{
      f.write((uint8_t*)pcm,n*2);
      samples+=n;

      if(peak>=MIC_SIL)last=millis();
      if(millis()-vs>=REC_MIN&&millis()-last>=SILENCE)break;
    }

    yield();
  }

  uint8_t h[44]={};
  auto p16=[](uint8_t*p,uint16_t v){p[0]=v;p[1]=v>>8;};
  auto p32=[](uint8_t*p,uint32_t v){
    p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;
  };

  memcpy(h,"RIFF",4);
  p32(h+4,samples*2+36);
  memcpy(h+8,"WAVEfmt ",8);
  p32(h+16,16);
  p16(h+20,1);
  p16(h+22,1);
  p32(h+24,RATE);
  p32(h+28,RATE*2);
  p16(h+32,2);
  p16(h+34,16);
  memcpy(h+36,"data",4);
  p32(h+40,samples*2);

  f.seek(0);
  f.write(h,44);
  f.close();

  if(!voice||!samples){
    LittleFS.remove(STT_FILE);
    return false;
  }

  return true;
}

void ensureWiFi(){
  while(WiFi.status()!=WL_CONNECTED){
    oledBase("BOOT","WAITING WIFI...");
    if(!wifiManagerConnect(false))delay(1000);
  }
}

bool syncTime(){
  configTime(7*3600,0,
             "pool.ntp.org",
             "time.nist.gov",
             "time.google.com");

  for(byte a=0;a<4;a++){
    Serial.printf("TARS: NTP %d/4\r\n",a+1);

    for(byte i=0;i<20;i++){
      if(time(nullptr)>=1704067200)return true;
      delay(500);
    }
  }

  return false;
}

bool wifiOK(){
  if(WiFi.status()==WL_CONNECTED)return true;
  return wifiManagerConnect(false);
}

String stt(){
  if(!wifiOK()||!ntpOK)return "";

  File f=LittleFS.open(STT_FILE);
  if(!f)return "";

  String boundary="----TARSSTT";
  String head="--"+boundary+
    "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"stt.wav\""
    "\r\nContent-Type: audio/wav\r\n\r\n";
  String tail="\r\n--"+boundary+"--\r\n";

  WiFiClientSecure c;
  c.setInsecure();
  c.setTimeout(15000);

  String host=TARS_CLOUD_URL;
  int p=host.indexOf("://");
  if(p>=0)host=host.substring(p+3);
  p=host.indexOf('/');
  if(p>=0)host=host.substring(0,p);

  if(!c.connect(host.c_str(),443)){
    f.close();
    return "";
  }

  size_t total=head.length()+f.size()+tail.length();

  c.printf(
    "POST /stt HTTP/1.1\r\nHost: %s\r\n"
    "Content-Type: multipart/form-data; boundary=%s\r\n"
    "Content-Length: %u\r\nConnection: close\r\n\r\n",
    host.c_str(),boundary.c_str(),(unsigned)total
  );

  c.print(head);

  uint8_t z[IO_BUF];

  while(f.available()){
    size_t n=f.read(z,sizeof(z));
    if(c.write(z,n)!=n){
      f.close();c.stop();return "";
    }
  }

  f.close();
  c.print(tail);

  uint32_t t=millis();
  while(!c.available()&&c.connected()&&millis()-t<20000)
    delay(5);

  if(!c.available()){
    c.stop();
    return "";
  }

  String status=c.readStringUntil('\n');
  status.trim();

  String body;
  while(c.connected()||c.available())
    body+=c.readString();

  c.stop();

  if(status.indexOf(" 200 ")<0)return "";

  int a=body.indexOf('{'),b=body.lastIndexOf('}');
  if(a<0||b<a)return "";

  JsonDocument j;
  if(deserializeJson(j,body.substring(a,b+1)))return "";

  String q=j["text"].as<String>();
  if(!q.length())q=j["transcript"].as<String>();

  q.trim();
  return q;
}

String ask(const String&q){
  if(!wifiOK()||!ntpOK)return "";

  WiFiClientSecure c;
  c.setInsecure();

  HTTPClient h;
  if(!h.begin(c,String(TARS_CLOUD_URL)+"/ask"))return "";

  h.setTimeout(15000);
  h.addHeader("Content-Type","application/json");

  JsonDocument j;
  j["question"]=q;

  String body;
  serializeJson(j,body);

  int code=h.POST(body);
  if(code<200||code>=300){
    h.end();
    return "";
  }

  String r=h.getString();
  h.end();

  JsonDocument x;
  if(deserializeJson(x,r))return "";

  String a=x["response"].as<String>();
  if(!a.length())a=x["answer"].as<String>();

  return a;
}

bool initDAC(){
  pinMode(DAC,OUTPUT);
  dacWrite(DAC,0);
  return true;
}

void dacMute(){dacWrite(DAC,0);}

class DACOut:public AudioStream{
  AudioInfo info;
  int16_t buf[DAC_BUF];
  volatile size_t head=0,tail=0;
  volatile bool active=false;
  TaskHandle_t task=nullptr;
  uint32_t rate=PLAY_RATE;

  size_t count(){return head>=tail?head-tail:DAC_BUF-tail+head;}
  size_t freeBuf(){return DAC_BUF-1-count();}

  static void taskFn(void*x){((DACOut*)x)->run();}

  void run(){
    uint32_t us=1000000UL/(rate?rate:PLAY_RATE),next=micros();

    while(active){
      if(head==tail){
        vTaskDelay(1);
        continue;
      }

      int16_t s=buf[tail];
      tail=(tail+1)%DAC_BUF;

      int32_t v=(int32_t)s*825/1000;
      dacWrite(DAC,constrain((v+32768+128)>>8,0,255));

      while((int32_t)(next-micros())>0)
        delayMicroseconds(1);

      next+=us;
    }

    dacMute();
    active=false;
    task=nullptr;
    vTaskDelete(nullptr);
  }

public:
  void setAudioInfo(AudioInfo i)override{
    info=i;
    AudioStream::setAudioInfo(i);
    rate=i.sample_rate?i.sample_rate:PLAY_RATE;
  }

  int availableForWrite()override{return freeBuf()*2;}

  void start(){
    head=tail=0;
    dacMute();
    active=true;

    if(!task)
      xTaskCreatePinnedToCore(
        taskFn,"TARS_DAC",2048,this,2,&task,1
      );
  }

  bool empty(){return head==tail;}

  void stop(){
    active=false;
    uint32_t t=millis();

    while(task&&millis()-t<2000)
      vTaskDelay(1);

    dacMute();
  }

  size_t write(const uint8_t*d,size_t n)override{
    if(!d||!active||info.bits_per_sample!=16)return 0;

    size_t ch=info.channels;
    size_t frames=n/(ch*2),done=0;

    while(done<frames){
      size_t space=freeBuf();

      if(!space){
        vTaskDelay(1);
        continue;
      }

      size_t c=min(space,frames-done);

      for(size_t i=0;i<c;i++){
        size_t k=done+i;
        int16_t s;

        if(ch==1){
          s=d[k*2]|((uint16_t)d[k*2+1]<<8);
        }else{
          int16_t l=d[k*4]|((uint16_t)d[k*4+1]<<8);
          int16_t r=d[k*4+2]|((uint16_t)d[k*4+3]<<8);
          s=(l+r)/2;
        }

        buf[head]=s;
        head=(head+1)%DAC_BUF;
      }

      done+=c;
      taskYIELD();
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

  playing=true;
  oledPos=oledPage=0;
  oledTick=millis();

  if(!dacOK)dacOK=initDAC();

  if(!dacOK||!mp3.begin()){
    f.close();
    playing=false;
    return false;
  }

  oledBase(singMode?"SINGING":"SPEAKING");
  dacOut.start();

  StreamCopy cp(mp3,f,IO_BUF);
  uint32_t t=millis();

  while(f.available()&&millis()-t<120000){
    cp.copy();
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

bool downloadMP3(const String&url,const String&text){
  if(!wifiOK()||!ntpOK)return false;

  WiFiClientSecure c;
  c.setInsecure();

  HTTPClient h;
  if(!h.begin(c,url))return false;

  h.setTimeout(60000);
  h.addHeader("Content-Type","application/json");

  JsonDocument j;

  if(url.endsWith("/sing"))j["prompt"]=text;
  else j["text"]=text;

  String body;
  serializeJson(j,body);

  int code=h.POST(body);

  if(code<200||code>=300){
    h.end();
    return false;
  }

  LittleFS.remove(PLAY_FILE);

  File f=LittleFS.open(PLAY_FILE,FILE_WRITE);
  if(!f){
    h.end();
    return false;
  }

  WiFiClient*s=h.getStreamPtr();
  uint8_t z[IO_BUF];
  int len=h.getSize();
  size_t total=0;
  uint32_t last=millis();

  while(h.connected()&&(len>0||len==-1)){
    size_t n=s->available();

    if(n){
      n=min(n,sizeof(z));
      int r=s->readBytes(z,n);

      if(r>0){
        f.write(z,r);
        total+=r;
        if(len>0)len-=r;
        last=millis();
      }
    }else{
      if(millis()-last>10000)break;
      delay(1);
    }

    yield();
  }

  f.close();
  h.end();

  return total>0;
}

bool embeddedMP3(){
  LittleFS.remove(PLAY_FILE);

  File f=LittleFS.open(PLAY_FILE,FILE_WRITE);
  if(!f)return false;

  size_t n=f.write(
    TARS_ELEVENLABS_MP3,
    TARS_ELEVENLABS_MP3_LEN
  );

  f.close();

  return n==TARS_ELEVENLABS_MP3_LEN;
}

bool isSing(const String&q){
  String s=q;
  s.toLowerCase();

  return s.indexOf("nyanyi")>=0||
         s.indexOf("bernyanyi")>=0||
         s.indexOf("nyanyikan")>=0;
}

String localTimeText(){
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

  char s[150];

  snprintf(
    s,sizeof(s),
    "Pukul %02d lewat %02d menit WIB. %s, %02d %s tahun %04d.",
    t.tm_hour,t.tm_min,d[t.tm_wday],
    t.tm_mday,m[t.tm_mon],t.tm_year+1900
  );

  return String(s);
}

void answer(const String&q){
  String l=q;
  l.toLowerCase();

  if(l.indexOf("jokowi")>=0){
    singMode=false;
    oledText="Saya akan lawan.";
    oledPos=oledPage=0;

    if(embeddedMP3())playMP3();
    else oledBase("STANDBY","AUDIO ERROR");

    return;
  }

  if(l.indexOf("pengalaman")>=0){
    singMode=false;
    oledText="Aku adalah TARS, AI yang dikembangkan oleh Ilman.";
    oledPos=oledPage=0;
    oledBase("STANDBY",oledText);
    return;
  }

  if(l.indexOf("jam")>=0||
     l.indexOf("waktu")>=0||
     l.indexOf("tanggal")>=0||
     l.indexOf("hari")>=0){

    if(!ntpOK){
      oledBase("STANDBY","TIME ERROR");
      return;
    }

    singMode=false;
    oledText=localTimeText();
    oledPos=oledPage=0;
    oledBase("TIME",oledText);
    return;
  }

  singMode=isSing(q);

  if(singMode){
    if(downloadMP3(String(TARS_CLOUD_URL)+"/sing",q))
      playMP3();
    else
      oledBase("STANDBY","AUDIO ERROR");

    singMode=false;
    return;
  }

  String a=ask(q);

  if(!a.length()){
    oledBase("STANDBY","ASK ERROR");
    return;
  }

  oledText=a;
  oledPos=oledPage=0;

  if(downloadMP3(String(TARS_CLOUD_URL)+"/tts",a))
    playMP3();
  else
    oledBase("STANDBY","AUDIO ERROR");

  singMode=false;
}

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
  Serial.println("TARS: LOW DRAM KWS");
  Serial.println("TARS: TARS=ONLINE / JOKOWI=OFFLINE / EXPERIENCE=OFFLINE");

  wifiManagerBegin();
  ensureWiFi();

  while(!syncTime()){
    oledBase("BOOT","NTP RETRY...");
    delay(2000);
    ensureWiFi();
  }

  ntpOK=true;
  oledBase("STANDBY","LISTENING...");
}

void loop(){
  if(playing)return;

  if(WiFi.status()!=WL_CONNECTED)
    ensureWiFi();

  if(!recordKWS()){
    oledListen();
    return;
  }

  switch(classify()){

    case TARS:{
      Serial.println("TARS: WAKE -> STT");

      if(!recordSTT()){
        oledBase("STANDBY","NO INPUT");
        break;
      }

      String q=stt();
      LittleFS.remove(STT_FILE);

      if(q.length()){
        Serial.printf("STT: %s\r\n",q.c_str());
        answer(q);
      }else{
        oledBase("STANDBY","NO INPUT");
      }
      break;
    }

    case HIDUP_JOKOWI:
      Serial.println("TARS: HIDUP JOKOWI -> OFFLINE");
      answer("hidup Jokowi");
      break;

    case PENGALAMAN:
      Serial.println("TARS: PENGALAMAN -> OFFLINE");
      answer("ceritakan pengalamanmu");
      break;

    default:
      Serial.println("TARS: UNKNOWN -> BLOCKED");
      LittleFS.remove(STT_FILE);
      oledBase("STANDBY","LISTENING...");
      break;
  }

  delay(1);
}
