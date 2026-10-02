#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Wire.h>
#include <time.h>
#include <math.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <esp_sntp.h>
#include <esp_heap_caps.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebSocketsClient.h>
#include <OV7670.h>
#include "vision_live.h"
#include "AudioTools.h"
#include "AudioTools/AudioLibs/AudioESP32ULP.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "AudioTools/AudioCodecs/CodecWAV.h"
#include "config.h"
#include "wifi_manager.h"
#include <string.h>
#include "Log.h"
#include "personality.h"
#include "autonomy.h"
#include "auto_speech.h"
#include "wheels.h"
#include "env.h"

#define MIC_PORT I2S_NUM_1
#define MIC_SCK 18
#define MIC_WS 19
#define MIC_SD 16
#define AUDIO_DAC_PIN 26
#define CAM_XCLK 4
#define CAM_SIOD 21
#define CAM_SIOC 22
#define CAM_D7 36
#define CAM_D6 39
#define CAM_D5 34
#define CAM_D4 35
#define CAM_D3 32
#define CAM_D2 33
#define CAM_D1 27
#define CAM_D0 25
#define CAM_VSYNC 13
#define CAM_HREF 14
#define CAM_PCLK 12

const uint32_t MIC_RATE=16000,RECORD_MIN_MS=500,SILENCE_MS=1000,PREROLL_MS=250,OLED_TYPE_MS=39,OLED_WAVE_MS=70,AUDIO_IDLE_MS=2500,OLED_PAGE_MS=2200,STREAM_EOF_IDLE_MS=5000,OFFLINE_MAX_MS=4000,ALARM_DURATION_MS=120000;
const int32_t MIC_THRESHOLD=12000,MIC_SILENCE=8000;
const size_t BUF=256,PREROLL_SAMPLES=MIC_RATE*PREROLL_MS/1000;
const int MP3_COPY_BUFFER=512;
const size_t AUDIO_RING_SIZE=8192,AUDIO_PREBUFFER=2048;
const char*STT_HOST="tars-cloud-v1.hilmane34.workers.dev";
enum TarsMode:uint8_t{MODE_OFFLINE,MODE_ONLINE};
TarsMode tarsMode=MODE_OFFLINE;

bool alarmRunning=false,greetingPlaying=false;
bool cameraOK=false,cameraLive=false,oledOK=false,micOK=false,dacOK=false;
bool playing=false,ntpOK=false,sttConnected=false,sttReady=false;
bool sttDone=false,sttError=false,sttClosing=false,dacLinksReady=false;
volatile bool ntpSyncEvent=false;
volatile uint8_t oledSpecial=0;
volatile uint32_t oledDeadUntil=0,oledDoorStart=0;
int alarmLastDay=-1;
uint8_t lastGreetingPeriod=255;
String sttFinal,sttPartial,oledText,oledStatus="READY";
uint32_t oledTypePos=0,oledLastType=0,oledLastWave=0;
uint32_t oledPage=0,oledLastPage=0,ramDiagAt=0;
volatile bool pendingVisionCheck=false;
uint32_t lastVisionEventAt=0;

/* STT LIFECYCLE */
const uint32_t STT_NORMAL_COOLDOWN=1000;
const uint32_t STT_ERROR_COOLDOWN=6000;
const uint32_t STT_QUOTA_COOLDOWN=15000;
const uint32_t STT_RECONNECT_GUARD=60000;
const uint32_t STT_IDLE_TIMEOUT_MS=8000;
const uint32_t VISION_EVENT_COOLDOWN_MS=45000;

static int32_t rawBuf[BUF/4];
static int16_t pcmBuf[BUF/4],preBuf[PREROLL_SAMPLES],sendBuf[256];

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);
AudioESP32ULP dac;
MP3DecoderHelix codec;
WAVDecoder wav;
WebSocketsClient sttWS;
OV7670*camera=nullptr;
SemaphoreHandle_t cameraMux=nullptr;

void ramDiag(const char*tag){
 uint32_t f=ESP.getFreeHeap(),m=ESP.getMinFreeHeap(),a=ESP.getMaxAllocHeap();
 uint32_t i=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
 uint32_t frag=f?100-(a*100/f):100;
 Serial.printf("TARS: RAM[%s] free=%u KB min=%u KB max=%u KB intmax=%u KB frag=%u%%\n",tag,f/1024,m/1024,a/1024,i/1024,frag);
}
void ramMonitor(){
 if(millis()-ramDiagAt<5000)return;
 ramDiagAt=millis();ramDiag("MONITOR");
}

extern const uint8_t alarm_start[] asm("_binary_src_alarm_mp3_start");
extern const uint8_t alarm_end[] asm("_binary_src_alarm_mp3_end");
#define MP3SYM(n) extern const uint8_t n##_start[] asm("_binary_src_"#n"_mp3_start");extern const uint8_t n##_end[] asm("_binary_src_"#n"_mp3_end");
MP3SYM(follow) MP3SYM(mundur) MP3SYM(maju) MP3SYM(online)
MP3SYM(offline) MP3SYM(angkat) MP3SYM(hari) MP3SYM(pagi)
MP3SYM(siang) MP3SYM(sore) MP3SYM(malam)

bool wifiOK();
bool visionLiveEnabled(){return tarsMode==MODE_ONLINE;}
String normCmd(String);
String systemStatus();
bool playLocalMP3(const uint8_t*,const uint8_t*,const String&,bool=false);

/* CAMERA DIAGNOSTIC */
void camDiag(const char*tag){
 Serial.printf("TARS: CAM[%s] live=%d ok=%d ptr=%p oled=%d play=%d text=%u RAM=%u/%u KB\n",
 tag,cameraLive,cameraOK,camera,oledOK,playing,(unsigned)oledText.length(),
 ESP.getFreeHeap()/1024,ESP.getMaxAllocHeap()/1024);
}
bool initCamera(){
  if(camera && cameraOK && cameraLive)return true;
  Serial.println("TARS: CAM INIT START");
  ramDiag("CAM-BEFORE-INIT");
  camera=new OV7670(
    OV7670::Mode::QVGA_RGB565,
    CAM_SIOD,CAM_SIOC,CAM_VSYNC,CAM_HREF,
    CAM_XCLK,CAM_PCLK,
    CAM_D0,CAM_D1,CAM_D2,CAM_D3,
    CAM_D4,CAM_D5,CAM_D6,CAM_D7
  );
  if(!camera){
    cameraOK=cameraLive=false;
    Serial.println("TARS: CAM INIT ALLOC FAILED");
    ramDiag("CAM-INIT-FAILED");
    return false;
  }
  Serial.printf("TARS: CAM OBJECT=%p RES=%dx%d\n",
    camera,camera->xres,camera->yres);
  if(camera->xres!=320 || camera->yres!=240){
    delete camera;
    camera=nullptr;
    cameraOK=cameraLive=false;
    ramDiag("CAM-INVALID-RELEASED");
    return false;
  }
  cameraOK=true;
  cameraLive=true;
  ramDiag("CAM-AFTER-INIT");
  camDiag("LIVE");
  return true;
}
void stopCamera(){
  Serial.println("TARS: CAM STOP START");
  camDiag("BEFORE-OFF");
  cameraLive=false;
  if(cameraMux){
    if(xSemaphoreTake(cameraMux,portMAX_DELAY)!=pdTRUE){
      Serial.println("TARS: CAM MUTEX FAILED");
      return;
    }
  }
  if(camera){
    delete camera;
    camera=nullptr;
  }
  cameraOK=false;
  if(cameraMux)xSemaphoreGive(cameraMux);
  delay(50);
  ramDiag("CAM-AFTER-DELETE");
  camDiag("OFF");
  Serial.println("TARS: CAM STOP DONE");
}
bool startCamera(){
  Serial.println("TARS: CAM START REQUEST");
  camDiag("START-BEFORE");
  if(camera && cameraOK && cameraLive){
    Serial.println("TARS: CAM ALREADY LIVE");
    return true;
  }
  cameraLive=false;
  cameraOK=false;
  delay(50);
  bool ok=initCamera();
  if(!ok){
    Serial.println("TARS: CAM RESTART FAILED");
    ramDiag("CAM-RESTART-FAILED");
    return false;
  }
  Serial.println("TARS: CAM RESTART SUCCESS");
  ramDiag("CAM-RESTART-DONE");
  camDiag("START-DONE");
  return true;
}
void drawCameraOLED(){
 if(!cameraLive||!camera||!cameraOK||!oledOK)return;
 static uint8_t preview[128*64];
 memset(preview,0,sizeof(preview));
 if(cameraMux&&xSemaphoreTake(cameraMux,pdMS_TO_TICKS(2500))!=pdTRUE)
   return;
 bool ok=false;
 if(cameraLive&&camera&&cameraOK)
   ok=I2SCamera::capturePreview(preview);
 if(cameraMux)xSemaphoreGive(cameraMux);
 if(ok){
  EnvState environment;
  if(envAnalyze(preview,environment)){
   autonomySetEnvironment(environment);
   Serial.printf(
    "TARS: ENV L=%d C=%d R=%d OBS=%d CONF=%u%%\n",
    environment.leftClear,
    environment.centerClear,
    environment.rightClear,
    environment.obstacle,
    environment.confidence
   );
  }else{
   EnvState invalid = {};
autonomySetEnvironment(invalid);
  }
  oled.clearDisplay();
  for(int y=0;y<64;y++)
   for(int x=0;x<128;x++)
    if(preview[y*128+x])
     oled.drawPixel(x,y,SSD1306_WHITE);
  oled.display();
 }else{
  EnvState invalid = {};
autonomySetEnvironment(invalid);
 }
}

/* VISION */
bool needsVision(String q){
 q.toLowerCase();
 return q.indexOf("ambil")>=0||q.indexOf("lihat")>=0||
 q.indexOf("lihatkan")>=0||q.indexOf("apa ini")>=0||
 q.indexOf("apa itu")>=0||q.indexOf("benda")>=0||
 q.indexOf("objek")>=0||q.indexOf("warna")>=0||
 q.indexOf("yang ada di depan")>=0||q.indexOf("di depan saya")>=0||
 q.indexOf("di depanmu")>=0||q.indexOf("yang terlihat")>=0||
 q.indexOf("terlihat apa")>=0||q.indexOf("lihat apa")>=0;
}

/* MEMORY MP3 */
class MemMP3Stream:public Stream{
 const uint8_t*a=nullptr,*z=nullptr;size_t p=0;
public:
 void begin(const uint8_t*s,const uint8_t*e){a=s;z=e;p=0;}
 int available()override{return a&&z?(int)(z-a-p):0;}
 int read()override{return available()?a[p++]:-1;}
 int read(uint8_t*b,size_t n){n=min(n,(size_t)available());if(n){memcpy(b,a+p,n);p+=n;}return n;}
 int peek()override{return available()?a[p]:-1;}
 void flush()override{}size_t write(uint8_t)override{return 0;}
 size_t write(const uint8_t*,size_t)override{return 0;}
}localMP3,alarmStream;

/* AUDIO RING */
class AudioRingStream:public Stream{
 uint8_t b[AUDIO_RING_SIZE];
 volatile size_t h=0,t=0,n=0;
 volatile bool done=false,stopFlag=false,running=false;
 TaskHandle_t task=nullptr;portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
 WiFiClient*src=nullptr;int expected=-1,received=0;uint32_t lastRx=0;bool gotData=false;
 static void entry(void*p){((AudioRingStream*)p)->rx();vTaskDelete(nullptr);}
 size_t push(const uint8_t*p,size_t x){
  if(!p||!x)return 0;
  portENTER_CRITICAL(&mux);x=min(x,AUDIO_RING_SIZE-n);size_t z=min(x,AUDIO_RING_SIZE-h);
  memcpy(b+h,p,z);if(x>z)memcpy(b,p+z,x-z);h=(h+x)%AUDIO_RING_SIZE;n+=x;portEXIT_CRITICAL(&mux);return x;
 }
 void rx(){
  uint8_t tmp[512];lastRx=millis();gotData=false;
  while(!stopFlag){
   int av=src?src->available():0;
   if(av){
    size_t free=AUDIO_RING_SIZE-available();
    if(!free){vTaskDelay(1);continue;}
    size_t want=min((size_t)av,sizeof(tmp));want=min(want,free);
    int r=src->read(tmp,want);
    if(r>0){push(tmp,r);received+=r;lastRx=millis();gotData=true;if(expected>=0&&received>=expected)break;}
   }else{
    if(expected>=0&&received>=expected)break;
    if(expected<0&&gotData&&millis()-lastRx>=STREAM_EOF_IDLE_MS)break;
    if(src&&!src->connected()&&gotData)break;
    vTaskDelay(1);
   }
  }
  portENTER_CRITICAL(&mux);done=true;running=false;portEXIT_CRITICAL(&mux);
  Serial.printf("TARS: TARS_RX STACK FREE=%u\n",task?uxTaskGetStackHighWaterMark(task):0);task=nullptr;
 }
public:
 void start(WiFiClient&s,int len=-1){
  stop();portENTER_CRITICAL(&mux);h=t=n=0;done=false;stopFlag=false;running=true;portEXIT_CRITICAL(&mux);
  src=&s;expected=len;received=0;lastRx=millis();gotData=false;
  xTaskCreatePinnedToCore(entry,"TARS_RX",3000,this,2,&task,0);
  if(task)Serial.printf("TARS: TARS_RX STACK START FREE=%u\n",uxTaskGetStackHighWaterMark(task));
 }
 void stop(){
  stopFlag=true;uint32_t st=millis();
  while(running&&millis()-st<1500)vTaskDelay(1);
  if(task){vTaskDelete(task);task=nullptr;}
  portENTER_CRITICAL(&mux);running=false;done=true;portEXIT_CRITICAL(&mux);src=nullptr;
 }
 bool finished(){return done&&available()==0;}
 int available()override{portENTER_CRITICAL(&mux);int r=n;portEXIT_CRITICAL(&mux);return r;}
 int read()override{uint8_t c;return read(&c,1)==1?c:-1;}
 int read(uint8_t*p,size_t x){
  if(!p||!x)return 0;
  portENTER_CRITICAL(&mux);size_t take=min((size_t)n,x);
  if(take){size_t z=min(take,AUDIO_RING_SIZE-t);memcpy(p,b+t,z);if(take>z)memcpy(p+z,b,take-z);t=(t+take)%AUDIO_RING_SIZE;n-=take;}
  portEXIT_CRITICAL(&mux);return take;
 }
 int peek()override{portENTER_CRITICAL(&mux);int r=n?b[t]:-1;portEXIT_CRITICAL(&mux);return r;}
 void flush()override{portENTER_CRITICAL(&mux);h=t=n=0;portEXIT_CRITICAL(&mux);}
 size_t write(uint8_t)override{return 0;}
 size_t write(const uint8_t*,size_t)override{return 0;}
}audioRing;

/* AUDIO PIPE */
class PCMProbeStream:public AudioStream{
 AudioOutput*out;uint64_t decBytes=0,dacBytes=0;
public:
 PCMProbeStream(AudioOutput&o):out(&o){}
 bool begin()override{return true;}
 void end()override{}
 void reset(){decBytes=dacBytes=0;}
 void setAudioInfo(audio_tools::AudioInfo x)override{
  AudioStream::setAudioInfo(x);out->setAudioInfo(x);
  Serial.printf("TARS: PCM->DAC %lu Hz / %d ch / %d bit\n",(unsigned long)x.sample_rate,x.channels,x.bits_per_sample);
 }
 size_t write(const uint8_t*p,size_t n)override{
  if(!p||!n)return 0;decBytes+=n;size_t d=0;
  while(d<n){size_t w=out->write(p+d,n-d);if(w)d+=w;else{delay(1);yield();}}
  dacBytes+=d;return d;
 }
 int availableForWrite()override{return out->availableForWrite();}
 void report(){Serial.printf("TARS: PCM BYTES=%llu DAC=%llu\n",(unsigned long long)decBytes,(unsigned long long)dacBytes);}
};

PCMProbeStream pcmProbe(dac);
ResampleStream mp3Resample(pcmProbe),wavResample(pcmProbe);
EncodedAudioStream dec(&mp3Resample,&codec);
EncodedAudioStream wavDec(&wavResample,&wav);
StreamCopy copier(MP3_COPY_BUFFER);

/* OLED */
void oledSetStatus(const String&s){oledStatus=s;oledText="";oledTypePos=0;oledPage=0;oledLastPage=millis();}
void oledSetListening(){oledSetStatus("LISTENING");}
void oledStartSpeak(const String&s){
 oledStatus="SPEAKING";oledText=s;oledTypePos=0;oledPage=0;oledLastType=millis();oledLastPage=millis();
}
void oledShowText(const String&s,const String&status){
 oledStatus=status;oledText=s;oledTypePos=s.length();oledPage=0;oledLastType=millis();oledLastPage=millis();
}
void drawSpecialOLED(uint8_t m){
 oled.clearDisplay();oled.setTextColor(SSD1306_WHITE);
 oled.drawLine(15,55,8,37,1);oled.drawLine(8,37,8,22,1);oled.drawLine(8,22,4,17,1);oled.drawLine(8,22,8,14,1);oled.drawLine(8,22,12,15,1);
 oled.drawLine(113,55,120,37,1);oled.drawLine(120,37,120,22,1);oled.drawLine(120,22,124,17,1);oled.drawLine(120,22,120,14,1);oled.drawLine(120,22,116,15,1);
 if(m==1){oled.fillCircle(42,25,8,1);oled.fillCircle(86,25,8,1);oled.drawLine(45,44,83,44,1);}
 else{
  oled.drawLine(34,18,49,32,1);oled.drawLine(49,18,34,32,1);oled.drawLine(79,18,94,32,1);oled.drawLine(94,18,79,32,1);
  oled.drawCircle(64,45,7,1);oled.fillRect(61,49,6,4,0);oled.drawLine(64,52,64,57,1);oled.drawLine(64,57,69,57,1);
  uint32_t e=millis()-oledDoorStart;int bx=5+(int)((e/35U>48U)?48U:e/35U);
  oled.drawLine(bx-10,27,bx-2,27,1);oled.drawLine(bx-8,30,bx-2,30,1);oled.fillCircle(bx,27,3,1);
  if(e>1700){oled.drawLine(53,23,58,28,1);oled.drawLine(58,23,53,28,1);}
 }
 oled.display();
}
void oledTask(void*){
 for(;;){
  if(!oledOK){vTaskDelay(50);continue;}
  uint32_t now=millis();
  if(oledSpecial){
   if(oledSpecial==2&&now>=oledDeadUntil){oledSpecial=0;oledSetStatus(tarsMode==MODE_ONLINE?"LISTENING":"READY");}
   else{drawSpecialOLED(oledSpecial);vTaskDelay(20);continue;}
  }
  if(cameraLive&&!playing&&!oledText.length()){drawCameraOLED();vTaskDelay(1000);continue;}
  if(oledText.length()&&oledTypePos<oledText.length()&&now-oledLastType>=OLED_TYPE_MS)oledTypePos++,oledLastType=now;
  if(now-oledLastWave>=OLED_WAVE_MS){
   oledLastWave=now;oled.clearDisplay();oled.setTextColor(1);oled.setTextSize(2);oled.setCursor(36,0);oled.print("TARS");
   oled.setTextSize(1);oled.setCursor(3,17);oled.print(oledStatus);
   if(oledText.length()){
    String s=oledText.substring(0,min(oledTypePos,(uint32_t)oledText.length()));
    uint32_t lineNo=0,target=oledPage*4;uint8_t shown=0;String line;bool next=false;
    for(size_t i=0;i<=s.length();i++){
     char c=i<s.length()?s[i]:'\0';
     if(c=='\n'||c=='\0'){
      if(lineNo>=target&&shown<4){oled.setCursor(3,29+shown*8);oled.print(line);shown++;}
      line="";lineNo++;if(shown>=4){next=i<s.length();break;}continue;
     }
     line+=c;
     if(line.length()>=20){
      int cut=line.lastIndexOf(' ');
      if(cut>0){
       String rest=line.substring(cut+1);line=line.substring(0,cut);
       if(lineNo>=target&&shown<4){oled.setCursor(3,29+shown*8);oled.print(line);shown++;}
       line=rest;lineNo++;if(shown>=4){next=i+1<s.length();break;}
      }
     }
    }
    if(oledStatus=="SPEAKING"&&now-oledLastPage>=OLED_PAGE_MS){if(next)oledPage++;oledLastPage=now;}
   }
   if(oledStatus=="LISTENING"){int x=64+(int)(sin(now/120.0)*25);oled.drawCircle(x,56,4,1);}
   else if(oledStatus=="SPEAKING"){int w=8+(now/40)%18;oled.fillRect(64-w/2,51,w,6,1);}
   oled.display();
  }
  vTaskDelay(10);
 }
}

/* DAC */
bool initDAC(){
 AudioInfo info(22050,1,16);dac.setMonoDAC(ULP_DAC2);
 if(!dac.begin(info)){Serial.println("TARS: DAC ERROR");dacOK=false;return false;}
 if(!dacLinksReady){
  dec.addNotifyAudioChange(mp3Resample);mp3Resample.addNotifyAudioChange(pcmProbe);
  wavDec.addNotifyAudioChange(wavResample);wavResample.addNotifyAudioChange(pcmProbe);dacLinksReady=true;
 }
 dacOK=true;Serial.println("TARS: ULP DAC GPIO26 READY");return true;
}
bool audioStart(){
 if(dacOK)return true;
 if(!initDAC()){Serial.println("TARS: DAC START FAILED");return false;}
 Serial.println("TARS: AUDIO ULP DAC -> GPIO26");return true;
}
void audioStop(){if(dacOK){dac.end();dacOK=false;}Serial.println("TARS: ULP DAC OFF");}

/* MIC */
bool initMic(){
 i2s_config_t c={};
 c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_RX);c.sample_rate=MIC_RATE;c.bits_per_sample=I2S_BITS_PER_SAMPLE_32BIT;
 c.channel_format=I2S_CHANNEL_FMT_ONLY_RIGHT;c.communication_format=I2S_COMM_FORMAT_STAND_I2S;c.intr_alloc_flags=ESP_INTR_FLAG_LEVEL1;
 c.dma_buf_count=2;c.dma_buf_len=256;c.use_apll=false;c.tx_desc_auto_clear=false;c.fixed_mclk=0;
 if(i2s_driver_install(MIC_PORT,&c,0,nullptr)!=ESP_OK)return false;
 i2s_pin_config_t p={};p.bck_io_num=MIC_SCK;p.ws_io_num=MIC_WS;p.data_out_num=I2S_PIN_NO_CHANGE;p.data_in_num=MIC_SD;
 if(i2s_set_pin(MIC_PORT,&p)!=ESP_OK){i2s_driver_uninstall(MIC_PORT);return false;}
 i2s_zero_dma_buffer(MIC_PORT);Serial.println("TARS: INMP441 RIGHT READY");return true;
}

/* WIFI / NTP */
bool wifiOK(){return WiFi.status()==WL_CONNECTED||(wifiManagerConnect(false)&&WiFi.status()==WL_CONNECTED);}
void ntpCallback(struct timeval*){ntpSyncEvent=true;}
bool syncTime(){
 if(ntpOK)return true;
 ntpSyncEvent=false;sntp_set_time_sync_notification_cb(ntpCallback);sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
 configTime(7*3600,0,"pool.ntp.org","time.google.com","time.cloudflare.com");
 for(int a=1;a<=4;a++){
  Serial.printf("TARS: NTP %d/4\n",a);oledSetStatus("NTP "+String(a)+"/4");uint32_t st=millis();
  while(millis()-st<10000){
   if(ntpSyncEvent||sntp_get_sync_status()==SNTP_SYNC_STATUS_COMPLETED){
    time_t now=time(nullptr);
    if(now>=1704067200){
     struct tm t;localtime_r(&now,&t);
     Serial.printf("TARS: NTP VALID %04d-%02d-%02d %02d:%02d:%02d\n",t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec);
     ntpOK=true;oledSetStatus("NTP OK");return true;
    }
   }
   delay(100);yield();
  }
  if(a<4){ntpSyncEvent=false;sntp_restart();delay(1000);}
 }
 Serial.println("TARS: NTP FAILED 4/4");oledSetStatus("NTP FAILED");delay(1500);return false;
}

/* STT */
uint32_t sttRetryAt=0;bool sttRetryShown=false;
bool sttCooling(){return millis()<sttRetryAt;}

void sttEvent(WStype_t type,uint8_t*payload,size_t length){
 if(type==WStype_CONNECTED){
  sttConnected=true;sttError=false;Serial.println("TARS: STT WS CONNECTED");oledSetStatus("STT CONNECTED");return;
 }
 if(type==WStype_DISCONNECTED){
  sttConnected=false;if(!sttClosing&&!sttDone)sttError=true;
  Serial.println(sttClosing?"TARS: STT WS DISCONNECTED (NORMAL)":"TARS: STT WS DISCONNECTED");return;
 }
 if(type==WStype_ERROR){
  if(!sttClosing)sttError=true;Serial.println("TARS: STT WS ERROR");oledSetStatus("STT ERROR");return;
 }
 if(type!=WStype_TEXT)return;
 String msg;msg.reserve(length+1);for(size_t i=0;i<length;i++)msg+=(char)payload[i];
 JsonDocument j;if(deserializeJson(j,msg))return;
 String t=j["type"].as<String>();
 if(t=="ready"){
  sttReady=true;sttError=false;sttRetryShown=false;Serial.println("TARS: STT REALTIME READY");oledSetStatus("STT READY");
 }else if(t=="partial"){
  sttPartial=j["text"].as<String>();sttPartial.trim();if(sttPartial.length())Serial.println("TARS: STT PARTIAL = "+sttPartial);
 }else if(t=="final"){
  sttFinal=j["text"].as<String>();sttFinal.trim();sttDone=true;Serial.println("TARS: YOU SAID = "+sttFinal);
 }else if(t=="error"){
  sttError=true;sttDone=true;String e=j["error"].as<String>();
  Serial.println("TARS: STT ERROR = "+e);oledSetStatus("STT ERROR");
 }
}

void closeSTT(){
 sttClosing=true;sttWS.disconnect();sttConnected=false;sttReady=false;
 sttRetryAt=millis()+1800;sttClosing=false;
}

bool startSTT(bool offline=false){
 if(!wifiOK()||!micOK)return false;
 if((int32_t)(millis()-sttRetryAt)<0){
  if(!sttRetryShown){Serial.println("TARS: STT RETRY COOLDOWN");sttRetryShown=true;}return false;
 }
 closeSTT();sttReady=false;sttDone=false;sttError=false;sttFinal="";sttPartial="";
 sttWS.onEvent(sttEvent);sttWS.setReconnectInterval(60000);sttWS.enableHeartbeat(15000,5000,2);
 sttWS.beginSSL(STT_HOST,443,"/stt");
 uint32_t st=millis();
 while(!sttReady&&!sttError&&millis()-st<20000){sttWS.loop();delay(2);yield();}
 if(!sttReady){
  if(sttError)Serial.println(offline?"TARS: OFFLINE STT CONNECT ERROR":"TARS: STT CONNECT ERROR");
  else Serial.println(offline?"TARS: OFFLINE STT REALTIME TIMEOUT":"TARS: STT REALTIME TIMEOUT");
  closeSTT();return false;
 }
 return true;
}

String stopSTT(uint32_t samples,bool offline=false){
 if(!sttConnected&&!sttDone)return "";
 JsonDocument j;j["type"]="end";j["timestamp"]=(double)samples/MIC_RATE;
 String msg;serializeJson(j,msg);
 if(!sttWS.sendTXT(msg)){Serial.println("TARS: STT END SEND FAILED");closeSTT();return "";}
 uint32_t st=millis();
 while(!sttDone&&!sttError&&millis()-st<6000){sttWS.loop();delay(2);yield();}
 String r=sttFinal;closeSTT();return r;
}

String recordSTT(bool offline){
 if(!startSTT(offline))return "";
 if(offline)oledSetStatus("READY");else oledSetListening();
 size_t prePos=0,preCount=0;uint32_t voiceStart=0,lastVoice=0,samples=0;bool voice=false;
 for(;;){
  sttWS.loop();if(sttError)break;
  size_t bytes=0;
  if(i2s_read(MIC_PORT,rawBuf,sizeof(rawBuf),&bytes,pdMS_TO_TICKS(30))!=ESP_OK)continue;
  size_t count=bytes/4;int32_t peak=0;uint64_t sum=0;
  for(size_t i=0;i<count;i++){
   int32_t v=constrain(rawBuf[i]>>16,-32768,32767);pcmBuf[i]=(int16_t)v;
   int32_t a=abs(v);if(a>peak)peak=a;sum+=(uint64_t)a*a;
  }
  uint32_t rms=count?(uint32_t)sqrt((double)sum/count):0;
  if(!voice){
   for(size_t i=0;i<count;i++){preBuf[prePos]=pcmBuf[i];prePos=(prePos+1)%PREROLL_SAMPLES;if(preCount<PREROLL_SAMPLES)preCount++;}
   if(peak>=MIC_THRESHOLD||rms>=3000){
    voice=true;voiceStart=lastVoice=millis();
    size_t start=preCount==PREROLL_SAMPLES?prePos:0,nsend=0;
    for(size_t i=0;i<preCount;i++){
     sendBuf[nsend++]=preBuf[(start+i)%PREROLL_SAMPLES];
     if(nsend==256){if(!sttWS.sendBIN((uint8_t*)sendBuf,nsend*2)){sttError=true;break;}nsend=0;}
    }
    if(nsend&&!sttError&&!sttWS.sendBIN((uint8_t*)sendBuf,nsend*2))sttError=true;
    samples+=preCount;
    Serial.printf("TARS: %s VOICE PEAK=%ld RMS=%lu\n",offline?"OFFLINE":"ONLINE",(long)peak,(unsigned long)rms);
   }
  }else{
   if(!sttWS.sendBIN((uint8_t*)pcmBuf,count*2)){
    Serial.println(offline?"TARS: OFFLINE STT PCM SEND FAILED":"TARS: STT PCM SEND FAILED");sttError=true;break;
   }
   samples+=count;if(peak>=MIC_SILENCE||rms>=1800)lastVoice=millis();
   if(millis()-voiceStart>=RECORD_MIN_MS&&millis()-lastVoice>=SILENCE_MS)break;
  }
  yield();
 }
 if(!voice||sttError){
  closeSTT();if(!voice)Serial.println(offline?"TARS: OFFLINE MIC AUDIO TOO LOW":"TARS: MIC AUDIO TOO LOW");return "";
 }
 return stopSTT(samples,offline);
}

String recordRealtime(){return recordSTT(false);}
String recordOffline(){return recordSTT(true);}

/* COMMAND */
String normCmd(String s){
 s.toLowerCase();
 for(size_t i=0;i<s.length();i++)if(ispunct((unsigned char)s[i]))s.setCharAt(i,' ');
 while(s.indexOf("  ")>=0)s.replace("  "," ");
 s.trim();return s;
}
uint8_t greetingPeriod(){
 if(!ntpOK)return 255;time_t now=time(nullptr);if(now<1704067200)return 255;
 struct tm t;localtime_r(&now,&t);
 if(t.tm_hour>=5&&t.tm_hour<11)return 0;
 if(t.tm_hour>=11&&t.tm_hour<15)return 1;
 if(t.tm_hour>=15&&t.tm_hour<20)return 2;
 return 3;
}
const char*greetingText(uint8_t p){
 switch(p){
  case 0:return "Selamat pagi, tuan.";
  case 1:return "Selamat siang, tuan.";
  case 2:return "Selamat sore, tuan.";
  default:return "Selamat malam, tuan.";
 }
}
bool playTimeGreeting(uint8_t p){
 switch(p){
  case 0:return playLocalMP3(pagi_start,pagi_end,greetingText(p));
  case 1:return playLocalMP3(siang_start,siang_end,greetingText(p));
  case 2:return playLocalMP3(sore_start,sore_end,greetingText(p));
  case 3:return playLocalMP3(malam_start,malam_end,greetingText(p));
 }
 return false;
}

/* LOCAL MP3 */
bool playLocalMP3(const uint8_t*a,const uint8_t*z,const String&text,bool keepSpecial){
 if(!audioStart())return false;
 localMP3.begin(a,z);playing=true;wheelsStop();pcmProbe.reset();
 if(!keepSpecial)oledStartSpeak(text);
 if(!dec.begin()){
  Serial.printf("TARS: HELIX LOCAL START FAILED HEAP=%u MAX=%u\n",ESP.getFreeHeap(),ESP.getMaxAllocHeap());
  ramDiag("LOCAL-HELIX-FAIL");playing=false;audioStop();return false;
 }
 audio_tools::AudioInfo src=codec.audioInfo();bool ok=mp3Resample.begin(src,22050);
 if(ok){copier.begin(dec,localMP3);while(localMP3.available()>0)copier.copy();mp3Resample.flush();mp3Resample.end();}
 dec.end();pcmProbe.report();playing=false;audioStop();
 if(!keepSpecial)oledSetStatus(tarsMode==MODE_ONLINE?"LISTENING":"READY");
 wheelsStop();return ok;
}
void checkTimeGreeting(){
 if(!ntpOK||playing||alarmRunning||greetingPlaying)return;
 uint8_t p=greetingPeriod();if(p==255||p==lastGreetingPeriod)return;
 greetingPlaying=true;String txt=greetingText(p);oledShowText(txt,"SALAM");
 bool ok=playTimeGreeting(p);if(ok)lastGreetingPeriod=p;
 greetingPlaying=false;wheelsStop();oledSetStatus(ok?(tarsMode==MODE_ONLINE?"LISTENING":"READY"):"AUDIO ERROR");
}

/* ASK */
String ask(const String&q){
 if(!wifiOK())return "";
 WiFiClientSecure c;c.setInsecure();HTTPClient h;
 if(!h.begin(c,String(TARS_CLOUD_URL)+"/ask"))return "";
 h.setTimeout(12000);h.addHeader("Content-Type","application/json");
 String body;{JsonDocument j;j["question"]=q;serializeJson(j,body);}
 int code=h.POST(body);body="";Serial.printf("TARS: ASK HTTP=%d\n",code);
 if(code<200||code>=300){h.end();return "";}
 String r=h.getString();h.end();JsonDocument x;if(deserializeJson(x,r))return "";
 String s=x["response"].as<String>();s.trim();
 Serial.printf("TARS: ASK RESPONSE LENGTH=%u\n",(unsigned)s.length());return s;
}

/* TTS */
bool streamAudio(const String&url,const String&text){
 if(!wifiOK())return false;
 ramDiag("PRE-TTS");if(!audioStart())return false;
 WiFiClientSecure c;c.setInsecure();c.setTimeout(20000);HTTPClient h;
 uint32_t total=millis();
 if(!h.begin(c,url)){audioStop();return false;}
 h.setTimeout(20000);h.addHeader("Content-Type","application/json");
 const char*keys[]={"Content-Type","X-TARS-TTS","X-TARS-TTS-FORMAT"};h.collectHeaders(keys,3);
 {
  JsonDocument j;j["text"]=text;String body;serializeJson(j,body);
  int code=h.POST(body);Serial.printf("TARS: AUDIO HTTP=%d\n",code);
  if(code<200||code>=300){h.end();audioStop();return false;}
 }
 String ct=h.header("Content-Type"),fmt=h.header("X-TARS-TTS");ct.toLowerCase();WiFiClient*stream=h.getStreamPtr();
 if(!stream){h.end();audioStop();return false;}
 int contentLen=h.getSize();bool isWav=ct.indexOf("wav")>=0||fmt.equalsIgnoreCase("WAV");
 audioRing.start(*stream,contentLen);playing=true;wheelsStop();
 size_t target=AUDIO_PREBUFFER;if(contentLen>0)target=min(target,(size_t)contentLen);
 uint32_t ps=millis();
 while(audioRing.available()<(int)target&&!audioRing.finished()){
  if(millis()-ps>10000){audioRing.stop();h.end();playing=false;audioStop();return false;}
  delay(1);yield();
 }
 if(!audioRing.available()){audioRing.stop();h.end();playing=false;audioStop();return false;}
 ramDiag("BEFORE-DECODER");pcmProbe.reset();bool started=false;
 if(!isWav){
  if(!dec.begin()){
   ramDiag("HELIX-ALLOC-FAIL");
   Serial.printf("TARS: HELIX START FAILED HEAP=%u MAX=%u INTMAX=%u\n",ESP.getFreeHeap(),ESP.getMaxAllocHeap(),heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
   audioRing.stop();h.end();playing=false;audioStop();return false;
  }
  ramDiag("HELIX-READY");audio_tools::AudioInfo src=codec.audioInfo();
  if(!mp3Resample.begin(src,22050)){dec.end();audioRing.stop();h.end();playing=false;audioStop();return false;}
  copier.begin(dec,audioRing);
  while(true){
   int before=audioRing.available();bool copied=copier.copy();int after=audioRing.available();
   if(copied&&!started){started=true;oledStartSpeak(text);}
   if(audioRing.finished()&&!audioRing.available())break;
   if(before==after)delay(1);
  }
  mp3Resample.flush();mp3Resample.end();dec.end();
 }else{
  wavDec.begin();audio_tools::AudioInfo src=wav.audioInfo();
  if(!wavResample.begin(src,22050)){wavDec.end();audioRing.stop();h.end();playing=false;audioStop();return false;}
  oledStartSpeak(text);copier.begin(wavDec,audioRing);
  while(true){
   int before=audioRing.available();bool copied=copier.copy();int after=audioRing.available();
   if(copied&&!started)started=true;
   if(audioRing.finished()&&!audioRing.available())break;
   if(before==after)delay(1);
  }
  wavResample.flush();wavResample.end();wavDec.end();
 }
 audioRing.stop();pcmProbe.report();h.end();ramDiag("TTS-DONE");
 playing=false;audioStop();wheelsStop();oledSetListening();
 Serial.printf("TARS: AUDIO TOTAL=%lu ms\n",(unsigned long)(millis()-total));return started;
}
/* SHARED ONLINE AI CYCLE */
bool processOnlineRequest(const String &q,bool vision,bool status,bool automatic=false){
  wheelsStop();
  autonomyStop();
  ramDiag("BEFORE-CAMERA-CYCLE");
  // Kunci siklus vision terlebih dahulu
  if(!visionLivePause()){
    Serial.println("TARS: VISION PAUSE FAILED");
    wheelsStop();
    autonomyStop();
    oledSetStatus("CAMERA ERROR");
    return false;
  }
  // Permintaan biasa tidak membutuhkan foto
  // Vision akan mengambil foto sendiri di visionLiveAsk()
  if(!vision){
    stopCamera();
    if(camera || cameraLive || cameraOK){
      Serial.println("TARS: CAMERA OFF FAILED");
      visionLiveResume();
      startCamera();
      oledSetStatus("CAMERA ERROR");
      return false;
    }
  }
  wheelsStop();
  autonomyStop();
  ramDiag("CAMERA-OFF-BEFORE-TLS");
  String answer;
  if(vision) answer=visionLiveAsk(q);
  else if(status) answer=systemStatus();
  else answer=ask(q);
  if(!answer.length()){
    Serial.println("TARS: AI EMPTY RESPONSE");
    wheelsStop();
    autonomyStop();
    // Vision resume menghidupkan kamera jika sebelumnya dimatikan.
    visionLiveResume();
    // Untuk permintaan biasa, kamera dihidupkan di sini.
    if(!vision) startCamera();
    oledSetStatus("AI ERROR");
    ramDiag("AFTER-AI-ERROR");
    return false;
  }
  wheelsStop();
  autonomyStop();
  oledShowText(answer,
    automatic?"AUTO SPEECH":
    vision?"VISION":
    status?"STATUS":"ASK");
  delay(300);
  ramDiag("BEFORE-TTS");
  bool ok=streamAudio(String(TARS_CLOUD_URL)+"/tts",answer);
  // Pastikan seluruh audio sudah berhenti
  audioRing.stop();
  audioStop();
  playing=false;
  wheelsStop();
  autonomyStop();
  ramDiag("AFTER-TTS-AUDIO-OFF");
  // Lepaskan kunci dan pulihkan kamera
  visionLiveResume();
  // Vision resume sudah menghidupkan kamera sendiri.
  // Permintaan biasa perlu menghidupkannya di sini.
  if(!vision) startCamera();
  wheelsStop();
  autonomyStop();
  oledSetStatus(ok?"LISTENING":"AUDIO ERROR");
  if(automatic && ok) autoSpeechDone();
  ramDiag("AFTER-CAMERA-RESTART");
  return ok;
}
/* AUTO SPEECH */
bool autoSpeechCallback(const String &prompt){
  if(tarsMode!=MODE_ONLINE || playing || sttConnected)return false;
  if(!wifiOK())return false;

  return processOnlineRequest(prompt,false,false,true);
}
/* STATUS */
bool isStatusQuery(const String&q){
 String s=normCmd(q);s.replace("statuse","status");
 if(s=="status"||s=="tars status"||s=="status tars"||s=="cek status"||s=="tars cek status"||s=="cek status tars"||s=="status kamu"||s=="tars status kamu"||s=="kondisi kamu"||s=="kondisi tars")return true;
 return s.indexOf("cek status")>=0||s.indexOf("status tars")>=0||s.indexOf("tars status")>=0||s.indexOf("status kamu")>=0||s.indexOf("kondisi kamu")>=0||s.indexOf("kondisi tars")>=0;
}
String systemStatus(){
 String s="DATA STATUS TARS SAAT INI:\n";
 s+="RAM bebas "+String(ESP.getFreeHeap()/1024.0,1)+" KB, minimum "+String(ESP.getMinFreeHeap()/1024.0,1)+" KB, blok terbesar "+String(ESP.getMaxAllocHeap()/1024.0,1)+" KB.\n";
 s+="Flash "+String(ESP.getFlashChipSize()/1048576.0,1)+" MB, sketch "+String(ESP.getSketchSize()/1024.0,1)+" KB, ruang sketch bebas "+String(ESP.getFreeSketchSpace()/1024.0,1)+" KB.\n";
 s+="LittleFS total "+String(LittleFS.totalBytes()/1024.0,1)+" KB, terpakai "+String(LittleFS.usedBytes()/1024.0,1)+" KB.\n";
 s+="CPU "+String(getCpuFrequencyMhz())+" MHz, uptime "+String(millis()/3600000UL)+" jam "+String((millis()/60000UL)%60)+" menit.\n";
 s+="Suhu ESP32 "+String(temperatureRead(),1)+" C.\n";
 s+="WiFi "+String(WiFi.status()==WL_CONNECTED?"terhubung":"terputus");
 if(WiFi.status()==WL_CONNECTED)s+="; RSSI "+String(WiFi.RSSI())+" dBm; IP "+WiFi.localIP().toString();
 s+=".\nNTP "+String(ntpOK?"valid":"belum valid")+
 ", OLED "+String(oledOK?"aktif":"error")+
 ", mic "+String(micOK?"aktif":"error")+
 ", DAC "+String(dacOK?"aktif":"off")+
 ", kamera "+String(cameraLive?"aktif":"off")+
 ", audio "+String(playing?"sedang berjalan":"idle")+
 ", sistem roda siap (tanpa sensor umpan balik gerak)"+
 ", STT "+String(sttReady?"ready":(sttConnected?"connected":"idle"))+".";
 return s;
}

/* COMMAND HELPERS */
bool cmdMatch(const String&s,const char*const*v,uint8_t n){
 for(uint8_t i=0;i<n;i++)if(s==v[i])return true;return false;
}
bool isDoorWord(String w){
 w.toLowerCase();w.trim();
 return w=="dor"||w=="door"||w=="doar"||(w.length()>=3&&w.length()<=6&&w.startsWith("dor"));
}
bool isDoorCmd(const String&s){
 String x=s;x.trim();if(!x.length())return false;
 int p=0,doors=0,tokens=0;
 while(p<x.length()){
  while(p<x.length()&&x[p]==' ')p++;
  if(p>=x.length())break;
  int e=x.indexOf(' ',p);if(e<0)e=x.length();
  String w=x.substring(p,e);tokens++;
  if(isDoorWord(w))doors++;
  else if(w=="tars"&&tokens==1){}
  else return false;
  if(tokens>7)return false;p=e+1;
 }
 return doors>0;
}

/* ALARM */
bool alarmDue(){
 if(!ntpOK||alarmRunning)return false;
 time_t now=time(nullptr);if(now<1704067200)return false;
 struct tm t;localtime_r(&now,&t);
 return t.tm_hour==6&&t.tm_min==0&&alarmLastDay!=t.tm_yday;
}
bool playLocalAlarm(){
 if(!audioStart())return false;
 oledSetStatus("ALARM");playing=true;wheelsStop();alarmStream.begin(alarm_start,alarm_end);pcmProbe.reset();
 if(!dec.begin()){
  Serial.printf("TARS: HELIX ALARM START FAILED HEAP=%u MAX=%u\n",ESP.getFreeHeap(),ESP.getMaxAllocHeap());
  playing=false;audioStop();return false;
 }
 audio_tools::AudioInfo src=codec.audioInfo();bool ok=mp3Resample.begin(src,22050);
 if(ok){
  copier.begin(dec,alarmStream);oledStartSpeak("Tuan, waktunya bangun.");
  while(alarmStream.available()>0)copier.copy();
  mp3Resample.flush();mp3Resample.end();
 }
 dec.end();pcmProbe.report();playing=false;audioStop();wheelsStop();
 oledSetStatus(tarsMode==MODE_ONLINE?"LISTENING":"READY");return ok;
}
void runAlarm(){
 if(!alarmDue())return;
 time_t now=time(nullptr);struct tm t;localtime_r(&now,&t);alarmLastDay=t.tm_yday;
 alarmRunning=true;wheelsStop();uint32_t st=millis();
 while(millis()-st<ALARM_DURATION_MS){if(!playLocalAlarm())break;delay(500);}
 playing=false;alarmRunning=false;wheelsStop();oledSetStatus(tarsMode==MODE_ONLINE?"LISTENING":"READY");
}
bool specialActive(){return oledSpecial!=0;}

/* OFFLINE */
bool processOffline(const String&q){
 String s=normCmd(q);
 static const char*online[]={"online","on line","tars online","tars on line","mode online","tars mode online"};
 static const char*hari[]={"hari","hari ini","kata hari","kata hari ini","kata kata","kata kata hari ini","kata kata hari ini tars","kata hari ini tars","tars hari ini","tars kata hari ini"};
 if(cmdMatch(s,hari,sizeof(hari)/sizeof(*hari))){
  oledShowText("HARI INI","OFFLINE");playLocalMP3(hari_start,hari_end,"Kata-kata hari ini, tuan.");oledSetStatus("READY");return true;
 }
 if(cmdMatch(s,online,sizeof(online)/sizeof(*online))){
  Serial.println("TARS: SWITCH OFFLINE -> ONLINE");oledShowText("ONLINE","OFFLINE");tarsMode=MODE_ONLINE;
  playLocalMP3(online_start,online_end,"Mode online aktif, tuan");Serial.println("TARS: MODE ONLINE");return true;
 }
 Serial.println("TARS: OFFLINE REJECTED = "+q);oledSetStatus("READY");return true;
}

/* PROCESS */
/* PROCESS */
void processQuestion(const String&q){
  String nq=normCmd(q);
  if(tarsMode==MODE_OFFLINE){
    processOffline(q);
    return;
  }
  if(nq=="offline"||nq=="off line"||nq=="tars offline"||
     nq=="tars off line"||nq=="mode offline"||
     nq=="mode off line"||nq=="tars mode offline"||
     nq=="tars mode off line"){
    Serial.println("TARS: SWITCH ONLINE -> OFFLINE");
    closeSTT();
    tarsMode=MODE_OFFLINE;
    sttReady=sttDone=sttError=false;
    oledShowText("OFFLINE","ONLINE");
    playLocalMP3(offline_start,offline_end,"Mode offline aktif, tuan");
    oledSetStatus("READY");
    Serial.println("TARS: MODE OFFLINE");
    return;
  }
  oledShowText(q,"STT");
  delay(300);
  bool vision=needsVision(q);
  bool status=isStatusQuery(q);
  processOnlineRequest(q,vision,status,false);
}
/* SETUP */
void setup(){
 Serial.begin(SERIAL_BAUD);Wire.begin(OLED_SDA,OLED_SCL);Wire.setClock(400000);
 oledOK=oled.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR);
 if(oledOK){
  oled.clearDisplay();oled.setTextColor(SSD1306_WHITE);oled.setTextSize(2);
  oled.setCursor(36,0);oled.print("TARS");oled.setTextSize(1);oled.setCursor(3,27);oled.print("BOOT");oled.display();
 }
 wheelsBegin();cameraMux=xSemaphoreCreateMutex();micOK=initMic();
 if(!LittleFS.begin(true))Serial.println("TARS: LITTLEFS ERROR");
 else Serial.printf("TARS: LITTLEFS READY %u/%u KB\n",(unsigned)(LittleFS.usedBytes()/1024),(unsigned)(LittleFS.totalBytes()/1024));

 Serial.println("TARS: DAC GPIO26 ULP DAC2");
 Serial.println("TARS: AUDIO MP3/WAV -> 22050Hz/16bit");
 Serial.println("TARS: INMP441 RIGHT GPIO16");
 Serial.printf("TARS: MIC THRESHOLD=%ld SILENCE=%ld\n",(long)MIC_THRESHOLD,(long)MIC_SILENCE);
 Serial.println("TARS: MIC PEAK=12000/8000 RMS=3000/1800 PREROLL=250 ms BUF=256");
 Serial.println("TARS: STT ONLINE REALTIME PCM");
 Serial.println("TARS: STT OFFLINE REALTIME PCM");
 Serial.println("TARS: VISION ONLINE ONLY");
 Serial.printf("TARS: OV7670 D0..D7=%d,%d,%d,%d,%d,%d,%d,%d XCLK=%d PCLK=%d VSYNC=%d HREF=%d SCCB=%d/%d\n",
 CAM_D0,CAM_D1,CAM_D2,CAM_D3,CAM_D4,CAM_D5,CAM_D6,CAM_D7,CAM_XCLK,CAM_PCLK,CAM_VSYNC,CAM_HREF,CAM_SIOD,CAM_SIOC);
 Serial.println("TARS: L9110S WHEELS GPIO23/5 LEFT GPIO2/15 RIGHT");
 Serial.println("TARS: GPIO4 RESERVED FOR OV7670 XCLK");
 Serial.println("TARS: BLUETOOTH DISABLED");
 Serial.println("TARS: MODE OFFLINE");
 Serial.printf("TARS: AUDIO RING=%u PREBUFFER=%u\n",(unsigned)AUDIO_RING_SIZE,(unsigned)AUDIO_PREBUFFER);

 ramDiag("BOOT");
 if(oledOK)xTaskCreatePinnedToCore(oledTask,"TARS_OLED",4096,nullptr,1,nullptr,0);
 wifiManagerBegin();
 if(wifiManagerConnect(true))if(syncTime())checkTimeGreeting();

oledSetStatus("READY");

envBegin();
visionLiveBegin();
personalityBegin();
autonomyBegin();
autoSpeechBegin(autoSpeechCallback);
startCamera();
ramDiag("READY");
wheelsStop();

Serial.println("TARS: LIFE READY");
Serial.println("TARS: PERSONALITY READY");
Serial.println("TARS: AUTONOMY READY");
Serial.println("TARS: AUTO SPEECH READY");
}
/* LOOP */
void loop(){
  ramMonitor();
  if(playing){
    wheelsStop();
    personalityUpdate(true,false,false);
    autonomyStop();
    delay(1);
    return;
  }
  if(WiFi.status()!=WL_CONNECTED){
    if(!wifiOK()){
      oledSetStatus("WIFI ERROR");
      wheelsStop();
      autonomyStop();
      delay(500);
      return;
    }
  }
  if(alarmDue()){
    autonomyStop();
    runAlarm();
    return;
  }
  checkTimeGreeting();
  if(playing){
    wheelsStop();
    autonomyStop();
    return;
  }
  if(sttCooling()){
    personalityUpdate(true,false,false);
    autonomyStop();
    if(!specialActive())
      oledSetStatus(tarsMode==MODE_ONLINE?"LISTENING":"READY");
    delay(100);
    return;
  }
  // STT sedang menunggu dan merekam suara pengguna.
  wheelsStop();
  autonomyStop();
  String q=tarsMode==MODE_ONLINE?recordRealtime():recordOffline();
  if(q.length()){
    wheelsStop();
    autonomyStop();
    personalityUpdate(true,false,false);
    processQuestion(q);
  }else{
    personalityUpdate(false,false,false);
    if(!specialActive())
      oledSetStatus(tarsMode==MODE_ONLINE?"LISTENING":"READY");
  }
  // Tidak ada gerakan selama sesi STT.
  wheelsStop();
  autonomyStop();
  // Auto speech hanya boleh dipertimbangkan setelah sesi STT ditutup.
  if(!playing && tarsMode==MODE_ONLINE && !sttConnected){
    autoSpeechUpdate(true,false,false);
  }
  delay(1);
}
