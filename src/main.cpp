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
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebSocketsClient.h>
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "AudioTools/AudioCodecs/CodecWAV.h"
#include "config.h"
#include "wifi_manager.h"

#define MIC_PORT I2S_NUM_1
#define MIC_SCK 18
#define MIC_WS 19
#define MIC_SD 16
#define AUDIO_DAC_PIN 26

/* TARS PIN PLAN — hardware modules reserved, not initialized until installed */
#define OV_D0 36
#define OV_D1 39
#define OV_D2 34
#define OV_D3 35
#define OV_D4 32
#define OV_D5 33
#define OV_D6 25
#define OV_D7 27
#define OV_XCLK 4
#define OV_PCLK 14
#define OV_VSYNC 13
#define OV_HREF -1
#define OV_SIOD 21
#define OV_SIOC 22
#define OV_RESET -1
#define OV_PWDN -1

#define DFPLAYER_RX 17
#define DFPLAYER_TX -1

/* Motor/servo controller reserve: GPIO21/22 can later be shared as I2C bus. */
#define MOTOR_CTRL_PIN 23

const uint32_t MIC_RATE=16000,RECORD_MIN_MS=500,SILENCE_MS=1000,PREROLL_MS=250;
const uint32_t OLED_TYPE_MS=39,OLED_WAVE_MS=70,AUDIO_IDLE_MS=2500,OLED_PAGE_MS=2200;
const uint32_t STREAM_EOF_IDLE_MS=5000,OFFLINE_MAX_MS=4000,ALARM_DURATION_MS=120000;
const int32_t MIC_THRESHOLD=,7000,MIC_SILENCE=4500;
const size_t BUF=256,PREROLL_SAMPLES=MIC_RATE*PREROLL_MS/1000;
const int MP3_COPY_BUFFER=512;
const float MP3_VOLUME=.60f;
const size_t AUDIO_RING_SIZE=8192,AUDIO_PREBUFFER=2048;
const char*STT_HOST="tars-cloud-v1.hilmane34.workers.dev";

enum TarsMode:uint8_t{MODE_OFFLINE,MODE_ONLINE};
TarsMode tarsMode=MODE_OFFLINE;

bool alarmRunning=false,greetingPlaying=false;int alarmLastDay=-1;uint8_t lastGreetingPeriod=255;
extern const uint8_t alarm_start[] asm("_binary_src_alarm_mp3_start");
extern const uint8_t alarm_end[] asm("_binary_src_alarm_mp3_end");

#define MP3SYM(n) \
extern const uint8_t n##_start[] asm("_binary_src_" #n "_mp3_start"); \
extern const uint8_t n##_end[] asm("_binary_src_" #n "_mp3_end");
MP3SYM(follow)MP3SYM(mundur)MP3SYM(maju)MP3SYM(online)MP3SYM(offline)MP3SYM(angkat)
MP3SYM(hari)MP3SYM(pagi)MP3SYM(siang)MP3SYM(sore)MP3SYM(malam)

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);
AnalogAudioStream analog;MP3DecoderHelix codec;WAVDecoder wav;WebSocketsClient sttWS;
bool oledOK=false,micOK=false,dacOK=false,playing=false,ntpOK=false;
bool sttConnected=false,sttReady=false,sttDone=false,sttError=false;
volatile bool ntpSyncEvent=false;
volatile uint8_t oledSpecial=0;
volatile uint32_t oledDeadUntil=0,oledDoorStart=0;
String sttFinal,sttPartial,oledText,oledStatus="READY";
uint32_t oledTypePos=0,oledLastType=0,oledLastWave=0,oledPage=0,oledLastPage=0;
static int32_t rawBuf[BUF/4];
static int16_t pcmBuf[BUF/4],preBuf[PREROLL_SAMPLES],sendBuf[256];

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

class AudioRingStream:public Stream{
 uint8_t b[AUDIO_RING_SIZE];volatile size_t h=0,t=0,n=0;
 volatile bool done=false,stopFlag=false,running=false;
 TaskHandle_t task=nullptr;portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
 WiFiClient*src=nullptr;int expected=-1,received=0;uint32_t lastRx=0;bool gotData=false;
 static void entry(void*p){((AudioRingStream*)p)->rx();vTaskDelete(nullptr);}
 void rx(){
  uint8_t tmp[1024];lastRx=millis();gotData=false;
  while(!stopFlag){
   int av=src?src->available():0;
   if(av){
    size_t free=AUDIO_RING_SIZE-available();if(!free){vTaskDelay(1);continue;}
    size_t want=min((size_t)av,sizeof(tmp));want=min(want,free);
    int r=src->read(tmp,want);
    if(r>0){push(tmp,r);received+=r;lastRx=millis();gotData=true;
     if(expected>=0&&received>=expected)break;}
   }else{
    if(expected>=0&&received>=expected)break;
    if(expected<0&&gotData&&millis()-lastRx>=STREAM_EOF_IDLE_MS)break;
    if(src&&!src->connected()&&gotData)break;
    vTaskDelay(1);
   }
  }
  portENTER_CRITICAL(&mux);done=true;running=false;portEXIT_CRITICAL(&mux);task=nullptr;
 }
 size_t push(const uint8_t*p,size_t x){
  if(!p||!x)return 0;portENTER_CRITICAL(&mux);
  size_t free=AUDIO_RING_SIZE-n;x=min(x,free);size_t z=min(x,AUDIO_RING_SIZE-h);
  memcpy(b+h,p,z);if(x>z)memcpy(b,p+z,x-z);
  h=(h+x)%AUDIO_RING_SIZE;n+=x;portEXIT_CRITICAL(&mux);return x;
 }
 public:
 void start(WiFiClient&s,int len=-1){
  stop();portENTER_CRITICAL(&mux);h=t=n=0;done=false;stopFlag=false;running=true;portEXIT_CRITICAL(&mux);
  src=&s;expected=len;received=0;lastRx=millis();gotData=false;
  xTaskCreatePinnedToCore(entry,"TARS_RX",3072,this,2,&task,0);
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
  if(!p||!x)return 0;portENTER_CRITICAL(&mux);size_t take=min((size_t)n,x);
  if(take){size_t z=min(take,AUDIO_RING_SIZE-t);memcpy(p,b+t,z);
   if(take>z)memcpy(p+z,b,take-z);t=(t+take)%AUDIO_RING_SIZE;n-=take;}
  portEXIT_CRITICAL(&mux);return take;
 }
 int peek()override{portENTER_CRITICAL(&mux);int r=n?b[t]:-1;portEXIT_CRITICAL(&mux);return r;}
 void flush()override{portENTER_CRITICAL(&mux);h=t=n=0;portEXIT_CRITICAL(&mux);}
 size_t write(uint8_t)override{return 0;}size_t write(const uint8_t*,size_t)override{return 0;}
}audioRing;

class PCMProbeStream:public AudioStream{
 AudioStream*out;uint64_t decBytes=0,dacBytes=0;
 public:
 PCMProbeStream(AudioStream&o):out(&o){}
 bool begin()override{return true;}void end()override{}
 void reset(){decBytes=dacBytes=0;}
 void setAudioInfo(AudioInfo x)override{
  AudioStream::setAudioInfo(x);out->setAudioInfo(x);
  Serial.printf("TARS: PCM->DAC %lu Hz / %d ch / %d bit\n",
   (unsigned long)x.sample_rate,x.channels,x.bits_per_sample);
 }
 size_t write(const uint8_t*p,size_t n)override{
  if(!p||!n)return 0;decBytes+=n;size_t d=0;
  while(d<n){size_t w=out->write(p+d,n-d);if(w)d+=w;else{delay(1);yield();}}
  dacBytes+=d;return d;
 }
 int availableForWrite()override{return out->availableForWrite();}
 void report(){Serial.printf("TARS: PCM BYTES=%llu DAC=%llu\n",
  (unsigned long long)decBytes,(unsigned long long)dacBytes);}
};

PCMProbeStream pcmProbe(analog);
ResampleStream mp3Resample(pcmProbe),wavResample(pcmProbe);
EncodedAudioStream dec(&mp3Resample,&codec),wavDec(&wavResample,&wav);
StreamCopy copier(MP3_COPY_BUFFER);

/* OLED */
void oledSetStatus(const String&s){
 oledStatus=s;oledText="";oledTypePos=0;oledPage=0;oledLastPage=millis();
}
void oledSetListening(){oledSetStatus("LISTENING");}
void oledStartSpeak(const String&s){
 oledStatus="SPEAKING";oledText=s;oledTypePos=0;oledPage=0;
 oledLastType=millis();oledLastPage=millis();
}
void oledShowText(const String&s,const String&status){
 oledStatus=status;oledText=s;oledTypePos=s.length();oledPage=0;
 oledLastType=millis();oledLastPage=millis();
}

void drawSpecialOLED(uint8_t m){
 oled.clearDisplay();oled.setTextColor(SSD1306_WHITE);
 oled.drawLine(15,55,8,37,SSD1306_WHITE);oled.drawLine(8,37,8,22,SSD1306_WHITE);
 oled.drawLine(8,22,4,17,SSD1306_WHITE);oled.drawLine(8,22,8,14,SSD1306_WHITE);oled.drawLine(8,22,12,15,SSD1306_WHITE);
 oled.drawLine(113,55,120,37,SSD1306_WHITE);oled.drawLine(120,37,120,22,SSD1306_WHITE);
 oled.drawLine(120,22,124,17,SSD1306_WHITE);oled.drawLine(120,22,120,14,SSD1306_WHITE);oled.drawLine(120,22,116,15,SSD1306_WHITE);
 if(m==1){
  oled.fillCircle(42,25,8,SSD1306_WHITE);oled.fillCircle(86,25,8,SSD1306_WHITE);
  oled.drawLine(45,44,83,44,SSD1306_WHITE);
 }else{
  oled.drawLine(34,18,49,32,SSD1306_WHITE);oled.drawLine(49,18,34,32,SSD1306_WHITE);
  oled.drawLine(79,18,94,32,SSD1306_WHITE);oled.drawLine(94,18,79,32,SSD1306_WHITE);
  oled.drawCircle(64,45,7,SSD1306_WHITE);oled.fillRect(61,49,6,4,SSD1306_BLACK);
  oled.drawLine(64,52,64,57,SSD1306_WHITE);oled.drawLine(64,57,69,57,SSD1306_WHITE);
  uint32_t e=millis()-oledDoorStart;int bx=5+(int)((e/35U>48U)?48U:e/35U);
  oled.drawLine(bx-10,27,bx-2,27,SSD1306_WHITE);
  oled.drawLine(bx-8,30,bx-2,30,SSD1306_WHITE);
  oled.fillCircle(bx,27,3,SSD1306_WHITE);
  if(e>1700){
   oled.drawLine(53,23,58,28,SSD1306_WHITE);
   oled.drawLine(58,23,53,28,SSD1306_WHITE);
  }
 }
 oled.display();
}

void oledTask(void*){
 for(;;){
  if(!oledOK){vTaskDelay(50);continue;}
  uint32_t now=millis();
  if(oledSpecial){
   if(oledSpecial==2&&now>=oledDeadUntil){
    oledSpecial=0;oledSetStatus(tarsMode==MODE_ONLINE?"LISTENING":"READY");
   }else{drawSpecialOLED(oledSpecial);vTaskDelay(20);continue;}
  }
  if(oledText.length()&&oledTypePos<oledText.length()&&now-oledLastType>=OLED_TYPE_MS)
   oledTypePos++,oledLastType=now;
  if(now-oledLastWave>=OLED_WAVE_MS){
   oledLastWave=now;oled.clearDisplay();oled.setTextColor(SSD1306_WHITE);
   oled.setTextSize(2);oled.setCursor(36,0);oled.print("TARS");
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
    if(oledStatus=="SPEAKING"&&now-oledLastPage>=OLED_PAGE_MS){
     if(next)oledPage++;oledLastPage=now;
    }
   }
   if(oledStatus=="LISTENING"){
    int x=64+(int)(sin(now/120.0)*25);oled.drawCircle(x,56,4,SSD1306_WHITE);
   }else if(oledStatus=="SPEAKING"){
    int w=8+(now/40)%18;oled.fillRect(64-w/2,51,w,6,SSD1306_WHITE);
   }
   oled.display();
  }
  vTaskDelay(10);
 }
}

/* DAC */
bool initDAC(){
 auto cfg=analog.defaultConfig(TX_MODE);cfg.sample_rate=22050;cfg.channels=1;cfg.bits_per_sample=16;
 if(!analog.begin(cfg)){Serial.println("TARS: DAC ERROR");return false;}
 dec.addNotifyAudioChange(mp3Resample);mp3Resample.addNotifyAudioChange(pcmProbe);
 wavDec.addNotifyAudioChange(wavResample);wavResample.addNotifyAudioChange(pcmProbe);
 Serial.println("TARS: DAC GPIO26 RIGHT READY");Serial.println("TARS: AUDIO 22050 Hz / 16 bit");return true;
}

/* MIC */
bool initMic(){
 i2s_config_t c={};c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_RX);
 c.sample_rate=MIC_RATE;c.bits_per_sample=I2S_BITS_PER_SAMPLE_32BIT;
 c.channel_format=I2S_CHANNEL_FMT_ONLY_RIGHT;c.communication_format=I2S_COMM_FORMAT_STAND_I2S;
 c.intr_alloc_flags=ESP_INTR_FLAG_LEVEL1;c.dma_buf_count=2;c.dma_buf_len=256;
 c.use_apll=false;c.tx_desc_auto_clear=false;c.fixed_mclk=0;
 if(i2s_driver_install(MIC_PORT,&c,0,nullptr)!=ESP_OK)return false;
 i2s_pin_config_t p={};p.bck_io_num=MIC_SCK;p.ws_io_num=MIC_WS;
 p.data_out_num=I2S_PIN_NO_CHANGE;p.data_in_num=MIC_SD;
 if(i2s_set_pin(MIC_PORT,&p)!=ESP_OK)return false;i2s_zero_dma_buffer(MIC_PORT);
 Serial.println("TARS: INMP441 RIGHT READY");return true;
}

bool wifiOK(){return WiFi.status()==WL_CONNECTED||(wifiManagerConnect(false)&&WiFi.status()==WL_CONNECTED);}

bool bootWiFi(){
 oledSetStatus("WIFI CONNECTING");Serial.println("TARS: WIFI CONNECTING...");
 uint32_t st=millis();
 while(WiFi.status()!=WL_CONNECTED&&millis()-st<30000){wifiManagerConnect(false);delay(100);yield();}
 if(WiFi.status()!=WL_CONNECTED){Serial.println("TARS: WIFI BOOT FAILED");oledSetStatus("WIFI FAILED");return false;}
 Serial.print("TARS: WIFI IP=");Serial.println(WiFi.localIP());oledSetStatus("WIFI CONNECTED");delay(1200);return true;
}

/* NTP */
void ntpCallback(struct timeval*){ntpSyncEvent=true;}

bool syncTime(){
 if(ntpOK)return true;
 ntpSyncEvent=false;sntp_set_time_sync_notification_cb(ntpCallback);
 sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
 Serial.println("TARS: NTP START");
 configTime(7*3600,0,"pool.ntp.org","time.google.com","time.cloudflare.com");
 for(int a=1;a<=4;a++){
  Serial.printf("TARS: NTP %d/4\n",a);oledSetStatus("NTP "+String(a)+"/4");uint32_t st=millis();
  while(millis()-st<10000){
   if(ntpSyncEvent||sntp_get_sync_status()==SNTP_SYNC_STATUS_COMPLETED){
    time_t now=time(nullptr);
    if(now>=1704067200){
     struct tm t;localtime_r(&now,&t);
     Serial.printf("TARS: NTP VALID %04d-%02d-%02d %02d:%02d:%02d\n",
      t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec);
     ntpOK=true;oledSetStatus("NTP OK");return true;
    }
   }
   delay(100);yield();
  }
  if(a<4){Serial.println("TARS: NTP RETRY");ntpSyncEvent=false;sntp_restart();delay(1000);}
 }
 Serial.println("TARS: NTP FAILED 4/4");oledSetStatus("NTP FAILED");delay(1500);return false;
}

/* ONLINE STT */
void sttEvent(WStype_t type,uint8_t*payload,size_t length){
 if(type==WStype_CONNECTED){
  sttConnected=true;Serial.println("TARS: STT WS CONNECTED");oledSetStatus("STT CONNECTED");return;
 }
 if(type==WStype_DISCONNECTED){
  sttConnected=false;if(!sttDone)sttError=true;
  Serial.println("TARS: STT WS DISCONNECTED");oledSetStatus("STT DISCONNECTED");return;
 }
 if(type==WStype_ERROR){sttError=true;Serial.println("TARS: STT WS ERROR");oledSetStatus("STT ERROR");return;}
 if(type!=WStype_TEXT)return;
 String msg;msg.reserve(length+1);for(size_t i=0;i<length;i++)msg+=(char)payload[i];
 JsonDocument j;if(deserializeJson(j,msg))return;String t=j["type"].as<String>();
 if(t=="ready"){sttReady=true;Serial.println("TARS: STT REALTIME READY");oledSetStatus("STT READY");}
 else if(t=="partial"){sttPartial=j["text"].as<String>();sttPartial.trim();if(sttPartial.length()){Serial.print("TARS: STT PARTIAL = ");Serial.println(sttPartial);}}
 else if(t=="final"){sttFinal=j["text"].as<String>();sttFinal.trim();sttDone=true;Serial.print("TARS: YOU SAID = ");Serial.println(sttFinal);}
 else if(t=="error"){sttError=true;sttDone=true;String e=j["error"].as<String>();Serial.print("TARS: STT ERROR = ");Serial.println(e);oledSetStatus("STT ERROR");}
}

bool startSTT(){
 if(tarsMode!=MODE_ONLINE||!wifiOK())return false;
 sttConnected=sttReady=sttDone=sttError=false;sttFinal="";sttPartial="";
 sttWS.disconnect();sttWS.onEvent(sttEvent);sttWS.setReconnectInterval(0);
 sttWS.enableHeartbeat(15000,5000,2);sttWS.beginSSL(STT_HOST,443,"/stt");
 uint32_t st=millis();
 while(!sttReady&&!sttError&&millis()-st<20000){sttWS.loop();delay(2);yield();}
 if(!sttReady){Serial.println("TARS: STT REALTIME TIMEOUT");sttWS.disconnect();return false;}
 return true;
}

String stopSTT(uint32_t samples){
 if(!sttConnected)return "";
 JsonDocument j;j["type"]="end";j["timestamp"]=(double)samples/MIC_RATE;
 String msg;serializeJson(j,msg);sttWS.sendTXT(msg);Serial.println("TARS: STT END SENT");
 uint32_t st=millis();
 while(!sttDone&&!sttError&&millis()-st<6000){sttWS.loop();delay(2);yield();}
 String r=sttFinal;sttWS.disconnect();return r;
}

String recordRealtime(){
 if(tarsMode!=MODE_ONLINE||!micOK||!startSTT())return "";
 oledSetListening();size_t prePos=0,preCount=0;
 uint32_t voiceStart=0,lastVoice=0,samples=0;bool voice=false;
 Serial.println("TARS: REALTIME LISTENING");
 for(;;){
  sttWS.loop();if(sttError)break;size_t bytes=0;
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
    if(nsend&&!sttError)sttWS.sendBIN((uint8_t*)sendBuf,nsend*2);
    samples+=preCount;Serial.printf("TARS: VOICE PEAK=%ld RMS=%lu\n",(long)peak,(unsigned long)rms);
   }
  }else{
   if(!sttWS.sendBIN((uint8_t*)pcmBuf,count*2)){Serial.println("TARS: STT PCM SEND FAILED");sttError=true;break;}
   samples+=count;if(peak>=MIC_SILENCE||rms>=1800)lastVoice=millis();
   if(millis()-voiceStart>=RECORD_MIN_MS&&millis()-lastVoice>=SILENCE_MS)break;
  }
  yield();
 }
 if(!voice||sttError){sttWS.disconnect();if(!voice)Serial.println("TARS: MIC AUDIO TOO LOW");return "";}
 return stopSTT(samples);
}

/* OFFLINE STT */
String normCmd(String s){
 s.toLowerCase();
 for(size_t i=0;i<s.length();i++)if(ispunct((unsigned char)s[i]))s.setCharAt(i,' ');
 while(s.indexOf("  ")>=0)s.replace("  "," ");
 s.trim();return s;
}

bool startOfflineSTT(){
 if(!wifiOK()||!micOK)return false;
 sttConnected=sttReady=sttDone=sttError=false;sttFinal="";sttPartial="";
 sttWS.disconnect();sttWS.onEvent(sttEvent);sttWS.setReconnectInterval(0);
 sttWS.enableHeartbeat(15000,5000,2);sttWS.beginSSL(STT_HOST,443,"/stt");
 uint32_t st=millis();
 while(!sttReady&&!sttError&&millis()-st<20000){sttWS.loop();delay(2);yield();}
 if(!sttReady){Serial.println("TARS: OFFLINE STT REALTIME TIMEOUT");sttWS.disconnect();return false;}
 return true;
}

String stopOfflineSTT(uint32_t samples){
 if(!sttConnected)return "";
 JsonDocument j;j["type"]="end";j["timestamp"]=(double)samples/MIC_RATE;
 String msg;serializeJson(j,msg);sttWS.sendTXT(msg);Serial.println("TARS: OFFLINE STT END SENT");
 uint32_t st=millis();
 while(!sttDone&&!sttError&&millis()-st<6000){sttWS.loop();delay(2);yield();}
 String r=sttFinal;sttWS.disconnect();return r;
}

String recordOffline(){
 if(!micOK||!startOfflineSTT())return "";
 oledSetStatus("READY");size_t prePos=0,preCount=0;
 uint32_t voiceStart=0,lastVoice=0,samples=0;bool voice=false;
 Serial.println("TARS: OFFLINE REALTIME LISTENING");
 for(;;){
  sttWS.loop();if(sttError)break;size_t bytes=0;
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
    if(nsend&&!sttError)sttWS.sendBIN((uint8_t*)sendBuf,nsend*2);
    samples+=preCount;Serial.printf("TARS: OFFLINE VOICE PEAK=%ld RMS=%lu\n",(long)peak,(unsigned long)rms);
   }
  }else{
   if(!sttWS.sendBIN((uint8_t*)pcmBuf,count*2)){Serial.println("TARS: OFFLINE STT PCM SEND FAILED");sttError=true;break;}
   samples+=count;if(peak>=MIC_SILENCE||rms>=1800)lastVoice=millis();
   if(millis()-voiceStart>=RECORD_MIN_MS&&millis()-lastVoice>=SILENCE_MS)break;
  }
  yield();
 }
 if(!voice||sttError){sttWS.disconnect();if(!voice)Serial.println("TARS: OFFLINE MIC AUDIO TOO LOW");return "";}
 return stopOfflineSTT(samples);
}

/* TIME GREETING */
bool playLocalMP3(const uint8_t*,const uint8_t*,const String&,bool=false);
uint8_t greetingPeriod(){
 if(!ntpOK)return 255;
 time_t now=time(nullptr);if(now<1704067200)return 255;
 struct tm t;localtime_r(&now,&t);
 if(t.tm_hour>=5&&t.tm_hour<11)return 0;
 if(t.tm_hour>=11&&t.tm_hour<15)return 1;
 if(t.tm_hour>=15&&t.tm_hour<20)return 2;
 return 3;
}
const char* greetingText(uint8_t p){
 switch(p){
  case 0:return "Emm..., Selamat pagi, tuan.";
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
void checkTimeGreeting(){
 if(!ntpOK||playing||alarmRunning||greetingPlaying)return;
 uint8_t p=greetingPeriod();
 if(p==255||p==lastGreetingPeriod)return;
 greetingPlaying=true;
 Serial.printf("TARS: TIME GREETING PERIOD=%u\n",p);
 Serial.println(String("TARS: ")+greetingText(p));
 oledShowText(greetingText(p),"SALAM");
 bool ok=playTimeGreeting(p);
 if(ok)lastGreetingPeriod=p;
 greetingPlaying=false;
 oledSetStatus(ok?(tarsMode==MODE_ONLINE?"LISTENING":"READY"):"AUDIO ERROR");
}

/* LOCAL MP3 */
bool playLocalMP3(const uint8_t*a,const uint8_t*z,const String&text,bool keepSpecial=false){
 if(!dacOK)dacOK=initDAC();if(!dacOK)return false;
 localMP3.begin(a,z);playing=true;pcmProbe.reset();if(!keepSpecial)oledStartSpeak(text);
 dec.begin();AudioInfo src=codec.audioInfo();bool ok=mp3Resample.begin(src,22050);
 if(ok){copier.begin(dec,localMP3);while(localMP3.available()>0)copier.copy();mp3Resample.flush();mp3Resample.end();}
 dec.end();pcmProbe.report();playing=false;
 if(!keepSpecial)oledSetStatus(tarsMode==MODE_ONLINE?"LISTENING":"READY");return ok;
}

/* ASK */
String ask(const String&q){
 if(!wifiOK())return "";
 WiFiClientSecure c;c.setInsecure();HTTPClient h;
 if(!h.begin(c,String(TARS_CLOUD_URL)+"/ask"))return "";
 h.setTimeout(12000);h.addHeader("Content-Type","application/json");
 JsonDocument j;j["question"]=q;String body;serializeJson(j,body);
 uint32_t st=millis();int code=h.POST(body);
 Serial.printf("TARS: ASK HTTP=%d TIME=%lu ms\n",code,(unsigned long)(millis()-st));
 if(code<200||code>=300){h.end();return "";}
 String r=h.getString();h.end();JsonDocument x;if(deserializeJson(x,r))return "";
 String s=x["response"].as<String>();s.trim();
 Serial.printf("TARS: ASK RESPONSE LENGTH=%u\n",(unsigned)s.length());
 Serial.println("TARS: ASK RESPONSE START");Serial.println(s);
 Serial.println("TARS: ASK RESPONSE END");return s;
}

/* TTS / AUDIO */
bool streamAudio(const String&url,const String&text){
 if(!wifiOK())return false;
 WiFiClientSecure c;c.setInsecure();c.setTimeout(20000);HTTPClient h;uint32_t total=millis();
 if(!h.begin(c,url))return false;h.setTimeout(20000);h.addHeader("Content-Type","application/json");
 const char*keys[]={"Content-Type","X-TARS-TTS","X-TARS-TTS-FORMAT"};h.collectHeaders(keys,3);
 JsonDocument j;j["text"]=text;String body;serializeJson(j,body);
 uint32_t st=millis();int code=h.POST(body);
 Serial.printf("TARS: AUDIO HTTP=%d TIME=%lu ms\n",code,(unsigned long)(millis()-st));
 if(code<200||code>=300){h.end();return false;}
 String ct=h.header("Content-Type"),fmt=h.header("X-TARS-TTS"),engine=h.header("X-TARS-TTS-FORMAT");ct.toLowerCase();
 Serial.println("TARS: TTS CONTENT-TYPE="+ct);Serial.println("TARS: TTS STATUS="+engine);Serial.println("TARS: TTS FORMAT="+fmt);
 WiFiClient*stream=h.getStreamPtr();if(!stream){h.end();return false;}
 if(!dacOK)dacOK=initDAC();if(!dacOK){h.end();return false;}
 int contentLen=h.getSize();Serial.printf("TARS: AUDIO CONTENT-LENGTH=%d\n",contentLen);
 audioRing.start(*stream,contentLen);playing=true;
 bool isWav=ct.indexOf("wav")>=0||fmt.equalsIgnoreCase("WAV");
 size_t target=AUDIO_PREBUFFER;if(contentLen>0)target=min(target,(size_t)contentLen);
 uint32_t ps=millis();
 while(audioRing.available()<(int)target&&!audioRing.finished()){
  if(millis()-ps>10000){Serial.println("TARS: AUDIO PREBUFFER TIMEOUT");audioRing.stop();h.end();playing=false;return false;}
  delay(1);yield();
 }
 if(!audioRing.available()){Serial.println("TARS: AUDIO EMPTY");audioRing.stop();h.end();playing=false;return false;}
 Serial.printf("TARS: AUDIO PREBUFFER=%d/%u\n",audioRing.available(),(unsigned)target);
 pcmProbe.reset();bool started=false,start=millis();

 if(!isWav){
  Serial.println("TARS: MP3 AUTO FORMAT");dec.begin();AudioInfo src=codec.audioInfo();
  if(!mp3Resample.begin(src,22050)){dec.end();audioRing.stop();h.end();playing=false;return false;}
  Serial.printf("TARS: MP3 SOURCE %lu Hz / %d ch / %d bit\n",(unsigned long)src.sample_rate,src.channels,src.bits_per_sample);
  copier.begin(dec,audioRing);
  while(true){
   int before=audioRing.available();bool copied=copier.copy();int after=audioRing.available();
   if(copied&&!started){started=true;oledStartSpeak(text);Serial.printf("TARS: AUDIO START +%lu ms\n",(unsigned long)(millis()-total));}
   if(audioRing.finished()&&!audioRing.available())break;if(before==after)delay(1);
  }
  mp3Resample.flush();mp3Resample.end();dec.end();
 }else{
  Serial.println("TARS: WAV AUTO FORMAT");wavDec.begin();AudioInfo src=wav.audioInfo();
  if(!wavResample.begin(src,22050)){wavDec.end();audioRing.stop();h.end();playing=false;return false;}
  oledStartSpeak(text);copier.begin(wavDec,audioRing);
  while(true){
   int before=audioRing.available();bool copied=copier.copy();int after=audioRing.available();
   if(copied&&!started){started=true;Serial.printf("TARS: AUDIO START +%lu ms\n",(unsigned long)(millis()-total));}
   if(audioRing.finished()&&!audioRing.available())break;if(before==after)delay(1);
  }
  wavResample.flush();wavResample.end();wavDec.end();
 }
 audioRing.stop();pcmProbe.report();h.end();playing=false;
 Serial.printf("TARS: AUDIO STREAM=%lu ms\n",(unsigned long)(millis()-start));
 Serial.printf("TARS: AUDIO TOTAL=%lu ms\n",(unsigned long)(millis()-total));
 oledSetListening();return started;
}

/* STATUS */
bool isStatusQuery(const String&q){
 String s=normCmd(q);
 s.replace("statuse","status");
 if(s=="status"||s=="tars status"||s=="status tars"||
    s=="cek status"||s=="tars cek status"||s=="cek status tars"||
    s=="status kamu"||s=="tars status kamu"||s=="kondisi kamu"||
    s=="kondisi tars")return true;
 return s.indexOf("cek status")>=0||s.indexOf("status tars")>=0||
        s.indexOf("tars status")>=0||s.indexOf("status kamu")>=0||
        s.indexOf("kondisi kamu")>=0||s.indexOf("kondisi tars")>=0;
}

String systemStatus(){
 String s="DATA STATUS TARS SAAT INI:\n";
 s+="RAM bebas "+String(ESP.getFreeHeap()/1024.0,1)+" KB, minimum "+String(ESP.getMinFreeHeap()/1024.0,1)+" KB, blok terbesar "+String(ESP.getMaxAllocHeap()/1024.0,1)+" KB.\n";
 s+="Flash "+String(ESP.getFlashChipSize()/1024.0/1024.0,1)+" MB, sketch "+String(ESP.getSketchSize()/1024.0,1)+" KB, ruang sketch bebas "+String(ESP.getFreeSketchSpace()/1024.0,1)+" KB.\n";
 s+="LittleFS total "+String(LittleFS.totalBytes()/1024.0,1)+" KB, terpakai "+String(LittleFS.usedBytes()/1024.0,1)+" KB.\n";
 s+="CPU "+String(getCpuFrequencyMhz())+" MHz, uptime "+String(millis()/3600000UL)+" jam "+String((millis()/60000UL)%60)+" menit.\n";
 s+="Suhu ESP32 "+String(temperatureRead(),1)+" C.\n";
 s+="WiFi "+String(WiFi.status()==WL_CONNECTED?"terhubung":"terputus");
 if(WiFi.status()==WL_CONNECTED)s+="; RSSI "+String(WiFi.RSSI())+" dBm; IP "+WiFi.localIP().toString();
 s+=".\nNTP "+String(ntpOK?"valid":"belum valid")+", OLED "+String(oledOK?"aktif":"error")+
   ", mic "+String(micOK?"aktif":"error")+", DAC "+String(dacOK?"aktif":"error")+
   ", audio "+String(playing?"sedang berjalan":"idle")+", STT "+String(sttReady?"ready":(sttConnected?"connected":"idle"))+".";
 return s;
}

/* ALARM */
bool alarmDue(){
 if(!ntpOK||alarmRunning)return false;time_t now=time(nullptr);if(now<1704067200)return false;
 struct tm t;localtime_r(&now,&t);return t.tm_hour==6&&t.tm_min==0&&alarmLastDay!=t.tm_yday;
}

bool playLocalAlarm(){
 if(!dacOK)dacOK=initDAC();if(!dacOK)return false;
 Serial.printf("TARS: OFFLINE ALARM PLAY %u BYTES\n",(unsigned)(alarm_end-alarm_start));
 oledSetStatus("ALARM");playing=true;alarmStream.begin(alarm_start,alarm_end);pcmProbe.reset();
 dec.begin();AudioInfo src=codec.audioInfo();bool ok=mp3Resample.begin(src,22050);
 if(ok){
  copier.begin(dec,alarmStream);oledStartSpeak("Tuan, waktunya bangun.");
  while(alarmStream.available()>0)copier.copy();
  mp3Resample.flush();mp3Resample.end();
 }
 dec.end();pcmProbe.report();playing=false;oledSetStatus(tarsMode==MODE_ONLINE?"LISTENING":"READY");
 Serial.println(ok?"TARS: OFFLINE ALARM DONE":"TARS: OFFLINE ALARM ERROR");return ok;
}

void runAlarm(){
 if(!alarmDue())return;time_t now=time(nullptr);struct tm t;localtime_r(&now,&t);
 alarmLastDay=t.tm_yday;alarmRunning=true;Serial.println("TARS: ALARM 06:00 WIB");uint32_t st=millis();
 while(millis()-st<ALARM_DURATION_MS){if(!playLocalAlarm())break;delay(500);}
 playing=false;alarmRunning=false;oledSetStatus(tarsMode==MODE_ONLINE?"LISTENING":"READY");Serial.println("TARS: ALARM SELESAI");
}

/* OFFLINE COMMANDS */
bool specialActive(){return oledSpecial!=0;}

bool cmdMatch(const String&s,const char*const*v,uint8_t n){
 for(uint8_t i=0;i<n;i++)if(s==v[i])return true;return false;
}

bool isDoorWord(String w){
 w.toLowerCase();w.trim();
 if(w=="dor"||w=="door"||w=="doar")return true;
 return w.length()>=3&&w.length()<=6&&w.startsWith("dor");
}

bool isDoorCmd(const String&s){
 String x=s;x.trim();if(!x.length())return false;
 int p=0,doors=0,tokens=0;
 while(p<x.length()){
  while(p<x.length()&&x[p]==' ')p++;if(p>=x.length())break;
  int e=x.indexOf(' ',p);if(e<0)e=x.length();
  String w=x.substring(p,e);w.trim();tokens++;
  if(isDoorWord(w))doors++;
  else if(w=="tars"&&tokens==1){}
  else return false;
  if(tokens>7)return false;p=e+1;
 }
 return doors>0;
}

bool processOffline(const String&q){
 String s=normCmd(q);
 static const char*online[]={"online","on line","tars online","tars on line","mode online","tars mode online"};
 static const char*ikut[]={"ikut","ikuti","ikut saya","ikuti saya","tars ikut","tars ikuti","ikut terus","ikuti terus","tars ikut saya","tars ikuti saya"};
 static const char*mundur[]={"mundur","tars mundur","mundur tars","surut","tars surut","jalan mundur","balik mundur"};
 static const char*maju[]={"maju","tars maju","maju tars","majulah","tars majulah","jalan maju","terus maju"};
 static const char*angkat[]={"angkat","tars angkat","angkat tangan","tars angkat tangan","angkatlah","tangan","angkat tangan tars"};
 static const char*hari[]={"hari","hari ini","kata hari","kata hari ini","kata kata","kata kata hari ini","kata kata hari","kata kata hari ini tars","kata hari ini tars","tars hari ini","tars kata hari ini"};

 if(cmdMatch(s,hari,sizeof(hari)/sizeof(*hari))){
  oledShowText("HARI INI","OFFLINE");playLocalMP3(hari_start,hari_end,"Kata-kata hari ini, tuan.");oledSetStatus("READY");return true;
 }
 if(cmdMatch(s,online,sizeof(online)/sizeof(*online))){
  Serial.println("TARS: SWITCH OFFLINE -> ONLINE");oledShowText("ONLINE","OFFLINE");tarsMode=MODE_ONLINE;
  playLocalMP3(online_start,online_end,"Mode online aktif, tuan");Serial.println("TARS: MODE ONLINE");return true;
 }
 if(cmdMatch(s,ikut,sizeof(ikut)/sizeof(*ikut))){
  oledShowText("IKUTI","OFFLINE");playLocalMP3(follow_start,follow_end,"Baik, tuan.");oledSetStatus("READY");return true;
 }
 if(cmdMatch(s,mundur,sizeof(mundur)/sizeof(*mundur))){
  oledShowText("MUNDUR","OFFLINE");playLocalMP3(mundur_start,mundur_end,"Siap, tuan.");oledSetStatus("READY");return true;
 }
 if(cmdMatch(s,maju,sizeof(maju)/sizeof(*maju))){
  oledShowText("MAJU","OFFLINE");playLocalMP3(maju_start,maju_end,"Siap, tuan.");oledSetStatus("READY");return true;
 }
 if(cmdMatch(s,angkat,sizeof(angkat)/sizeof(*angkat))){
  oledShowText("ANGKAT","OFFLINE");oledSpecial=1;playLocalMP3(angkat_start,angkat_end,"Ampun... tuan.",true);return true;
 }
 if(isDoorCmd(s)){
  oledShowText("DOOR","OFFLINE");oledSpecial=2;oledDoorStart=millis();oledDeadUntil=millis()+2200;
  Serial.print("TARS: DOOR CMD = ");Serial.println(q);return true;
 }
 Serial.print("TARS: OFFLINE REJECTED = ");Serial.println(q);oledSetStatus("READY");return true;
}

/* PROCESS */
void processQuestion(const String&q){
 String nq=normCmd(q);
 if(tarsMode==MODE_OFFLINE){processOffline(q);return;}

 if(nq=="offline"||nq=="off line"||nq=="tars offline"||nq=="tars off line"||
    nq=="mode offline"||nq=="mode off line"||nq=="tars mode offline"||nq=="tars mode off line"){
  Serial.println("TARS: SWITCH ONLINE -> OFFLINE");tarsMode=MODE_OFFLINE;sttWS.disconnect();
  sttConnected=sttReady=sttDone=sttError=false;oledShowText("OFFLINE","ONLINE");
  playLocalMP3(offline_start,offline_end,"Mode offline aktif, tuan");oledSetStatus("READY");
  Serial.println("TARS: MODE OFFLINE");return;
 }

 oledShowText(q,"STT");Serial.println("TARS: STT FINAL DISPLAY");delay(800);
 bool status=isStatusQuery(q);
 String answer;
 if(status){
  Serial.println("TARS: STATUS COMMAND DETECTED");
  answer=systemStatus();
 }else{
  Serial.println("TARS: STT->ASK DELAY DONE");
  answer=ask(q);
 }
 if(!answer.length()){oledSetStatus(status?"STATUS ERROR":"ASK ERROR");return;}
 oledShowText(answer,"ASK");Serial.println("TARS: ASK RESULT DISPLAY");delay(800);
 Serial.println("TARS: ASK->TTS DELAY DONE");
 uint32_t st=millis();bool ok=streamAudio(String(TARS_CLOUD_URL)+"/tts",answer);
 Serial.printf("TARS: AUDIO FUNCTION TIME=%lu ms\n",(unsigned long)(millis()-st));oledSetStatus(ok?"LISTENING":"AUDIO ERROR");
}

/* SETUP */
void setup(){
 Serial.begin(SERIAL_BAUD);Wire.begin(OLED_SDA,OLED_SCL);Wire.setClock(400000);
 oledOK=oled.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR);
 if(oledOK){
  oled.clearDisplay();oled.setTextColor(SSD1306_WHITE);oled.setTextSize(2);oled.setCursor(36,0);oled.print("TARS");
  oled.setTextSize(1);oled.setCursor(3,27);oled.print("BOOT");oled.display();
 }
 dacOK=initDAC();micOK=initMic();
 Serial.printf("TARS: DAC=%s MIC=%s\n",dacOK?"READY":"ERROR",micOK?"READY":"ERROR");
 if(!LittleFS.begin(true))Serial.println("TARS: LITTLEFS ERROR");
 else Serial.printf("TARS: LITTLEFS READY %u/%u KB\n",(unsigned)(LittleFS.usedBytes()/1024),(unsigned)(LittleFS.totalBytes()/1024));
 Serial.println("TARS: DAC GPIO26 INTERNAL DAC RIGHT");
 Serial.println("TARS: AUDIO MP3/WAV -> 22050Hz/16bit");
 Serial.println("TARS: INMP441 RIGHT GPIO16");
 Serial.printf("TARS: MIC THRESHOLD=%ld SILENCE=%ld\n",(long)MIC_THRESHOLD,(long)MIC_SILENCE);
 Serial.println("TARS: MIC PEAK=12000/8000 RMS=3000/1800 PREROLL=250 ms BUF=256");
 Serial.println("TARS: STT ONLINE REALTIME PCM");
 Serial.println("TARS: STT OFFLINE REALTIME PCM");
 Serial.printf("TARS: OV7670 D0..D7=%d,%d,%d,%d,%d,%d,%d,%d XCLK=%d PCLK=%d VSYNC=%d SCCB=%d/%d\n",OV_D0,OV_D1,OV_D2,OV_D3,OV_D4,OV_D5,OV_D6,OV_D7,OV_XCLK,OV_PCLK,OV_VSYNC,OV_SIOD,OV_SIOC);
 Serial.printf("TARS: DFPLAYER RX=%d TX=%d MOTOR_CTRL=%d\n",DFPLAYER_RX,DFPLAYER_TX,MOTOR_CTRL_PIN);
 Serial.println("TARS: MODE OFFLINE");Serial.println("TARS: BLUETOOTH DISABLED");
 Serial.printf("TARS: AUDIO RING=%u PREBUFFER=%u\n",(unsigned)AUDIO_RING_SIZE,(unsigned)AUDIO_PREBUFFER);
 Serial.printf("TARS: STREAM EOF IDLE=%lu ms\n",(unsigned long)STREAM_EOF_IDLE_MS);
 Serial.println("TARS: OFFLINE ALARM=06:00 WIB");
 if(oledOK)xTaskCreatePinnedToCore(oledTask,"TARS_OLED",4096,nullptr,1,nullptr,0);
 wifiManagerBegin();if(bootWiFi()){if(syncTime())checkTimeGreeting();}oledSetStatus("READY");
}

/* LOOP */
void loop(){
 if(playing){delay(1);return;}
 if(WiFi.status()!=WL_CONNECTED){
  if(!wifiOK()){oledSetStatus("WIFI ERROR");delay(500);return;}
 }
 if(alarmDue()){runAlarm();return;}
 checkTimeGreeting();if(playing){delay(1);return;}
 String q=tarsMode==MODE_ONLINE?recordRealtime():recordOffline();
 if(q.length())processQuestion(q);
 else if(!specialActive())oledSetStatus(tarsMode==MODE_ONLINE?"LISTENING":"READY");
 delay(1);
}
