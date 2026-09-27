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
#include <OV7670.h>
#include <esp_intr_alloc.h>
#include "JPEGEncoderWrapper.h"
#include "mbedtls/base64.h"

#define MIC_PORT I2S_NUM_1
#define MIC_SCK 18
#define MIC_WS 19
#define MIC_SD 16
#define AUDIO_DAC_PIN 26
#define OV_D0 25
#define OV_D1 27
#define OV_D2 33
#define OV_D3 32
#define OV_D4 35
#define OV_D5 34
#define OV_D6 39
#define OV_D7 36
#define OV_XCLK 4
#define OV_PCLK 12
#define OV_VSYNC 13
#define OV_HREF 14
#define OV_SIOD 21
#define OV_SIOC 22
#define MOTOR_ARM_A1 23
#define MOTOR_ARM_A2 5
#define MOTOR_GRIP_B1 2
#define MOTOR_GRIP_B2 15
#define DFPLAYER_RX 17
#define MOTOR_CTRL_PIN 23

const uint32_t MIC_RATE=16000,RECORD_MIN_MS=500,SILENCE_MS=1000,PREROLL_MS=250;
const uint32_t OLED_TYPE_MS=39,OLED_WAVE_MS=70,AUDIO_IDLE_MS=2500,OLED_PAGE_MS=2200;
const uint32_t STREAM_EOF_IDLE_MS=5000,OFFLINE_MAX_MS=4000,ALARM_DURATION_MS=120000;
const int32_t MIC_THRESHOLD=12000,MIC_SILENCE=8000;
const size_t BUF=256,PREROLL_SAMPLES=MIC_RATE*PREROLL_MS/1000;
const int MP3_COPY_BUFFER=512;
const float MP3_VOLUME=.67f;
const size_t AUDIO_RING_SIZE=8192,AUDIO_PREBUFFER=2048;
const char*STT_HOST="tars-cloud-v1.hilmane34.workers.dev";
static uint8_t visionJpeg[32768];

Adafruit_SSD1306 display(128,64,&Wire,-1);
WebSocketsClient sttWS;
OV7670*camera=nullptr;
bool cameraReady=false,cameraActive=false;
bool micOK=false,sttConnected=false,sttReady=false,sttDone=false,sttError=false;
bool playing=false,motorBusy=false,oledOK=false;
String sttText,oledText,oledStatus;
enum TarsMode{MODE_ONLINE,MODE_OFFLINE};
TarsMode tarsMode=MODE_ONLINE;

enum Behavior{BEH_IDLE,BEH_LISTEN,BEH_ANGRY,BEH_SAD,BEH_LOVE,BEH_WAKE};
Behavior behavior=BEH_IDLE;

AudioTools::AudioInfo audioInfo(22050,2,16);
I2SStream audio;
MP3DecoderHelix mp3;
WAVDecoder wav;
StreamCopy copier;
AnalogAudioStream analog;
uint32_t audioLast=0;
String visionObject="";

struct MemMP3Stream:public Stream{
 uint8_t*data=nullptr;size_t len=0,pos=0;
 void set(uint8_t*d,size_t l){data=d;len=l;pos=0;}
 int available(){return pos<len?(int)(len-pos):0;}
 int read(){return pos<len?data[pos++]:-1;}
 int peek(){return pos<len?data[pos]:-1;}
 void flush(){}
 size_t write(uint8_t){return 0;}
 size_t write(const uint8_t*,size_t){return 0;}
};
MemMP3Stream memStream;

struct AudioRingStream:public Stream{
 uint8_t b[AUDIO_RING_SIZE];size_t r=0,w=0,n=0;
 void reset(){r=w=n=0;}
 int available(){return n;}
 int read(){if(!n)return -1;uint8_t v=b[r];r=(r+1)%AUDIO_RING_SIZE;n--;return v;}
 int peek(){return n?b[r]:-1;}
 void flush(){}
 size_t write(uint8_t v){if(n>=AUDIO_RING_SIZE)return 0;b[w]=v;w=(w+1)%AUDIO_RING_SIZE;n++;return 1;}
 size_t write(const uint8_t*d,size_t l){size_t k=0;while(k<l&&n<AUDIO_RING_SIZE)write(d[k++]);return k;}
};
AudioRingStream audioRing;

struct PCMProbeStream:public Stream{
 Stream*src=nullptr;
 void set(Stream*s){src=s;}
 int available(){return src?src->available():0;}
 int read(){return src?src->read():-1;}
 int peek(){return src?src->peek():-1;}
 void flush(){if(src)src->flush();}
 size_t write(uint8_t){return 0;}
 size_t write(const uint8_t*,size_t){return 0;}
};
PCMProbeStream pcmProbe;

void motorStop(){
 digitalWrite(MOTOR_ARM_A1,LOW);digitalWrite(MOTOR_ARM_A2,LOW);
 digitalWrite(MOTOR_GRIP_B1,LOW);digitalWrite(MOTOR_GRIP_B2,LOW);
}
void armDown(){digitalWrite(MOTOR_ARM_A1,HIGH);digitalWrite(MOTOR_ARM_A2,LOW);}
void armUp(){digitalWrite(MOTOR_ARM_A1,LOW);digitalWrite(MOTOR_ARM_A2,HIGH);}
void gripOpen(){digitalWrite(MOTOR_GRIP_B1,HIGH);digitalWrite(MOTOR_GRIP_B2,LOW);}
void gripClose(){digitalWrite(MOTOR_GRIP_B1,LOW);digitalWrite(MOTOR_GRIP_B2,HIGH);}

void oledShowText(const String&a,const String&b=""){
 oledText=a;oledStatus=b;
}
void oledSetStatus(const String&s){oledStatus=s;}

void drawEye(int x,int y,int w,int h,bool pupil=true){
 display.fillRoundRect(x,y,w,h,8,SSD1306_WHITE);
 if(pupil)display.fillCircle(x+w/2,y+h/2,3,SSD1306_BLACK);
}
void drawFace(){
 if(!oledOK)return;
 display.clearDisplay();
 if(behavior==BEH_WAKE){
  drawEye(22,20,34,26);drawEye(72,20,34,26);
 }else if(behavior==BEH_LISTEN){
  drawEye(18,17,45,32);drawEye(78,25,25,20);
  display.drawCircle(64,52,5,SSD1306_WHITE);
 }else if(behavior==BEH_ANGRY){
  display.fillTriangle(15,19,52,13,52,25,SSD1306_WHITE);
  display.fillTriangle(76,25,113,13,113,19,SSD1306_WHITE);
  display.fillCircle(34,25,4,SSD1306_BLACK);display.fillCircle(94,25,4,SSD1306_BLACK);
 }else if(behavior==BEH_SAD){
  drawEye(20,20,36,24);drawEye(72,20,36,24);
  display.drawLine(55,51,64,47,SSD1306_WHITE);
  display.drawLine(64,47,73,51,SSD1306_WHITE);
 }else if(behavior==BEH_LOVE){
  display.fillTriangle(20,23,29,14,38,23,SSD1306_WHITE);
  display.fillTriangle(29,32,38,23,20,23,SSD1306_WHITE);
  display.fillTriangle(90,23,99,14,108,23,SSD1306_WHITE);
  display.fillTriangle(99,32,108,23,90,23,SSD1306_WHITE);
 }else{
  drawEye(18,19,42,27);drawEye(68,19,42,27);
 }
 display.display();
}

void oledTask(void*){
 uint32_t last=0;
 for(;;){
  if(millis()-last>=OLED_WAVE_MS){last=millis();drawFace();}
  vTaskDelay(10/portTICK_PERIOD_MS);
 }
}

void stopCamera(){
 if(!camera)return;
 delete camera;
 camera=nullptr;
 cameraReady=false;
 cameraActive=false;
 delay(20);
}

bool initCamera(){
 if(camera)return true;
 Serial.println("TARS: OV7670 INIT...");
 camera=new OV7670(
  OV_XCLK,OV_SIOD,OV_SIOC,
  OV_D7,OV_D6,OV_D5,OV_D4,OV_D3,OV_D2,OV_D1,OV_D0,
  OV_PCLK,OV_HREF,OV_VSYNC,
  OV7670::Mode::QQVGA_RGB565
 );
 if(!camera){
  Serial.println("TARS: OV7670 ERROR");
  return false;
 }
 cameraReady=true;
 Serial.println("TARS: OV7670 READY 160x120");
 return true;
}
void startCamera(){
 if(!camera)initCamera();
 cameraActive=cameraReady;
}
void testCameraFrame(){
 if(!camera)return;
 camera->oneFrame();
 Serial.println(camera->frame?"TARS: OV7670 LIVE":"TARS: OV7670 FRAME ERROR");
}

void drawCameraOLED(){
 if(!camera||!camera->frame)return;
 uint8_t*src=(uint8_t*)camera->frame;
 display.clearDisplay();
 for(int y=0;y<64;y++){
  int sy=y*120/64;
  for(int x=0;x<128;x++){
   int sx=x*160/128;
   size_t p=(sy*160+sx)*2;
   uint16_t c=((uint16_t)src[p]<<8)|src[p+1];
   uint8_t r=((c>>11)&31)<<3,g=((c>>5)&63)<<2,b=(c&31)<<3;
   uint8_t lum=(r*30+g*59+b*11)/100;
   if(lum>110)display.drawPixel(x,y,SSD1306_WHITE);
  }
 }
 display.display();
}

void initDAC(){
 analog.setPins(AUDIO_DAC_PIN);
 if(!analog.begin(audioInfo)){
  Serial.println("TARS: DAC ERROR");
  return;
 }
 Serial.println("TARS: DAC READY");
}
void audioStart(){
 stopCamera();
 delay(20);
 initDAC();
}
void audioStop(){
 analog.end();
 delay(20);
 startCamera();
}

bool initMic(){
 i2s_config_t c={};
 c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_RX);
 c.sample_rate=MIC_RATE;
 c.bits_per_sample=I2S_BITS_PER_SAMPLE_32BIT;
 c.channel_format=I2S_CHANNEL_FMT_ONLY_RIGHT;
 c.communication_format=I2S_COMM_FORMAT_I2S;
 c.intr_alloc_flags=ESP_INTR_FLAG_LEVEL1;
 c.dma_buf_count=4;c.dma_buf_len=BUF;
 c.use_apll=false;c.tx_desc_auto_clear=false;c.fixed_mclk=0;
 i2s_pin_config_t p={};
 p.bck_io_num=MIC_SCK;p.ws_io_num=MIC_WS;p.data_out_num=I2S_PIN_NO_CHANGE;p.data_in_num=MIC_SD;
 if(i2s_driver_install(MIC_PORT,&c,0,nullptr)!=ESP_OK)return false;
 if(i2s_set_pin(MIC_PORT,&p)!=ESP_OK){i2s_driver_uninstall(MIC_PORT);return false;}
 i2s_zero_dma_buffer(MIC_PORT);
 return true;
}

bool wifiOK(){
 return WiFi.status()==WL_CONNECTED;
}

bool syncTime(){
 configTime(7*3600,0,"pool.ntp.org","time.nist.gov","time.google.com");
 for(int i=0;i<4;i++){
  time_t now=time(nullptr);
  if(now>1700000000){Serial.println("TARS: NTP VALID");return true;}
  delay(1500);
 }
 Serial.println("TARS: NTP FAILED");
 return false;
}

void checkTimeGreeting(){
 static bool done=false;
 if(done)return;
 time_t now=time(nullptr);
 if(now<1700000000)return;
 struct tm t;localtime_r(&now,&t);
 if(t.tm_hour<6||t.tm_hour>9)return;
 done=true;
}

String normCmd(const String&s){
 String x=s;x.toLowerCase();x.trim();
 String o="";
 for(size_t i=0;i<x.length();i++){
  char c=x[i];
  if((c>='a'&&c<='z')||(c>='0'&&c<='9')||c==' ')o+=c;
 }
 while(o.indexOf("  ")>=0)o.replace("  "," ");
 return o;
}

bool specialActive(){return playing||motorBusy;}

void behaviorReset(){
 behavior=BEH_IDLE;
}

bool isStatusQuery(const String&q){
 String s=normCmd(q);
 return s=="status"||s=="tars status"||s=="cek status"||s=="cek keadaan"||
        s=="bagaimana keadaan tars";
}

String systemStatus(){
 String s="Aku TARS. ";
 s+=tarsMode==MODE_ONLINE?"Mode online. ":"Mode offline. ";
 s+=wifiOK()?"WiFi terhubung. ":"WiFi tidak terhubung. ";
 s+=micOK?"Mikrofon siap. ":"Mikrofon bermasalah. ";
 return s;
}

void sttEvent(WStype_t type,uint8_t*payload,size_t length){
 switch(type){
  case WStype_CONNECTED:
   sttConnected=true;
   sttWS.sendTXT("{\"type\":\"StartRecognition\",\"transcription_config\":{\"language\":\"id\",\"operating_point\":\"standard\",\"enable_partials\":true,\"max_delay\":0.7,\"diarization\":\"none\"},\"audio_format\":{\"type\":\"raw\",\"encoding\":\"pcm_s16le\",\"sample_rate\":16000,\"channels\":1}}");
   break;
  case WStype_TEXT:{
   String s((char*)payload);
   JsonDocument d;
   if(deserializeJson(d,s))break;
   String typeS=d["message"].as<String>();
   if(typeS=="RecognitionStarted"||typeS=="AddPartialTranscript")sttReady=true;
   if(typeS=="AddTranscript"){
    String x=d["results"][0]["alternatives"][0]["content"].as<String>();
    if(x.length())sttText=x;
   }
   if(typeS=="EndOfTranscript")sttDone=true;
   break;
  }
  case WStype_DISCONNECTED:sttConnected=false;break;
  case WStype_ERROR:sttError=true;break;
  default:break;
 }
}

bool startSTT(){
 sttText="";sttConnected=false;sttReady=false;sttDone=false;sttError=false;
 sttWS.beginSSL(STT_HOST,443,"/stt");
 sttWS.onEvent(sttEvent);
 sttWS.setReconnectInterval(3000);
 uint32_t t=millis();
 while(millis()-t<15000&&!sttReady&&!sttError){
  sttWS.loop();delay(2);
 }
 return sttReady;
}

void stopSTT(){
 if(sttConnected){
  sttWS.sendTXT("{\"message\":\"EndOfStream\"}");
  delay(200);
  sttWS.disconnect();
 }
 sttConnected=sttReady=sttDone=sttError=false;
}

String recordRealtime(){
 if(!micOK||!wifiOK())return "";
 if(!startSTT())return "";
 behavior=BEH_LISTEN;
 uint32_t start=millis(),lastVoice=millis(),speechStart=0;
 int32_t peak=0;
 int32_t buf[BUF];
 size_t got=0;
 bool speaking=false;
 while(millis()-start<12000){
  sttWS.loop();
  size_t bytes=0;
  if(i2s_read(MIC_PORT,buf,sizeof(buf),&bytes,20)!=ESP_OK)continue;
  got=bytes/sizeof(int32_t);
  for(size_t i=0;i<got;i++){
   int32_t v=abs(buf[i]>>16);
   if(v>peak)peak=v;
   if(v>=MIC_THRESHOLD){
    if(!speaking){speaking=true;speechStart=millis();}
    lastVoice=millis();
   }
   if(speaking&&millis()-lastVoice>=SILENCE_MS&&millis()-speechStart>=RECORD_MIN_MS){
    sttWS.sendTXT("{\"message\":\"ForceEndOfUtterance\"}");
    delay(50);
    sttWS.sendTXT("{\"message\":\"EndOfStream\"}");
    break;
   }
   int16_t pcm=(int16_t)(buf[i]>>16);
   if(sttConnected)sttWS.sendBIN((uint8_t*)&pcm,2);
  }
  if(sttDone)break;
 }
 stopSTT();
 behaviorReset();
 Serial.printf("TARS: MIC PEAK=%ld\n",(long)peak);
 return sttText;
}

String recordOffline(){
 if(!micOK)return "";
 behavior=BEH_LISTEN;
 uint32_t start=millis(),lastVoice=millis(),speechStart=0;
 bool speaking=false;
 int32_t buf[BUF];
 while(millis()-start<OFFLINE_MAX_MS){
  size_t bytes=0;
  if(i2s_read(MIC_PORT,buf,sizeof(buf),&bytes,20)!=ESP_OK)continue;
  for(size_t i=0;i<bytes/4;i++){
   int32_t v=abs(buf[i]>>16);
   if(v>=MIC_THRESHOLD){
    if(!speaking){speaking=true;speechStart=millis();}
    lastVoice=millis();
   }
  }
  if(speaking&&millis()-lastVoice>=SILENCE_MS&&millis()-speechStart>=RECORD_MIN_MS)break;
 }
 behaviorReset();
 return "";
}

bool isVisionCmd(const String&q){
 String s=normCmd(q);
 if(s.indexOf("lihat")>=0||s.indexOf("lihatkan")>=0)return true;
 if(s.indexOf("kamera")>=0)return true;
 if(s.indexOf("apa ini")>=0||s.indexOf("benda apa")>=0)return true;
 if(s.indexOf("apa itu")>=0||s.indexOf("yang saya")>=0)return true;
 if(s.indexOf("ambil ")>=0||s.indexOf("ambilin ")>=0||s.indexOf("ambilkan ")>=0)return true;
 if(s.indexOf("warna")>=0||s.indexOf("bentuk")>=0)return true;
 return false;
}

String base64Encode(const uint8_t*d,size_t n){
 static const char B64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
 String o;o.reserve((n*4)/3+8);
 for(size_t i=0;i<n;i+=3){
  uint32_t v=(uint32_t)d[i]<<16;
  if(i+1<n)v|=(uint32_t)d[i+1]<<8;
  if(i+2<n)v|=d[i+2];
  o+=B64[(v>>18)&63];o+=B64[(v>>12)&63];
  o+=i+1<n?B64[(v>>6)&63]:'=';
  o+=i+2<n?B64[v&63]:'=';
 }
 return o;
}

String visionAsk(const String&q){
 if(!wifiOK())return "";
 if(!camera)initCamera();
 if(!camera)return "";
 cameraActive=false;
 camera->oneFrame();
 if(!camera->frame){
  Serial.println("TARS: VISION FRAME ERROR");
  restoreCameraAfterDAC();
  return "";
 }
 size_t jl=0;
 bool ok=I2SCamera::encodeFrameToJPEG(visionJpeg,&jl,55)&&jl>0&&jl<=sizeof(visionJpeg);
 if(!ok){
  Serial.println("TARS: VISION JPEG ERROR");
  restoreCameraAfterDAC();
  return "";
 }
 String b64=base64Encode(visionJpeg,jl);
 restoreCameraAfterDAC();
 WiFiClientSecure c;c.setInsecure();
 HTTPClient h;
 if(!h.begin(c,String(TARS_CLOUD_URL)+"/vision"))return "";
 h.addHeader("Content-Type","application/json");
 JsonDocument d;
 d["question"]=q;
 d["image"]="data:image/jpeg;base64,"+b64;
 String body;serializeJson(d,body);
 int code=h.POST(body);
 String out=code>0?h.getString():"";
 h.end();
 if(code!=200){
  Serial.printf("TARS: VISION HTTP=%d\n",code);
  return "";
 }
 JsonDocument r;
 if(!deserializeJson(r,out)){
  if(r["answer"])return r["answer"].as<String>();
  if(r["response"])return r["response"].as<String>();
 }
 return out;
}

void restoreCameraAfterDAC(){
 if(!camera)initCamera();
 cameraActive=cameraReady;
}

void runPickObject(const String&object){
 if(motorBusy)return;
 motorBusy=true;behavior=BEH_IDLE;motorStop();
 visionObject=object;
 oledShowText("AMBIL "+object,"VISION");
 armDown();delay(1200);motorStop();delay(150);
 gripOpen();delay(600);motorStop();delay(150);
 gripClose();delay(800);motorStop();delay(150);
 armUp();delay(1200);motorStop();
 motorBusy=false;behaviorReset();
 oledSetStatus(tarsMode==MODE_ONLINE?"LISTENING":"READY");
}

void runPickPaper(){runPickObject("KERTAS");}

bool isPickPaperCmd(const String&q){
 String s=normCmd(q);
 return s=="ambil kertas"||s=="ambilin kertas"||s=="ambilkan kertas"||
        s=="tars ambil kertas"||s=="tars ambilin kertas"||s=="tars ambilkan kertas";
}

String ask(const String&q){
 if(!wifiOK())return "";
 WiFiClientSecure c;c.setInsecure();
 HTTPClient h;
 if(!h.begin(c,String(TARS_CLOUD_URL)+"/ask"))return "";
 h.addHeader("Content-Type","application/json");
 JsonDocument d;d["question"]=q;
 String body;serializeJson(d,body);
 int code=h.POST(body);
 String out=code>0?h.getString():"";
 h.end();
 if(code!=200)return "";
 JsonDocument r;
 if(!deserializeJson(r,out)){
  if(r["answer"])return r["answer"].as<String>();
  if(r["response"])return r["response"].as<String>();
 }
 return out;
}

bool streamAudio(const String&url,const String&text){
 if(!wifiOK())return false;
 audioStart();
 WiFiClientSecure c;c.setInsecure();
 HTTPClient h;
 if(!h.begin(c,url)){audioStop();return false;}
 h.addHeader("Content-Type","application/json");
 JsonDocument d;d["text"]=text;
 String body;serializeJson(d,body);
 int code=h.POST(body);
 if(code!=200){h.end();audioStop();return false;}
 WiFiClient*stream=h.getStreamPtr();
 uint8_t buf[MP3_COPY_BUFFER];
 uint32_t last=millis();
 while(h.connected()||stream->available()){
  int n=stream->readBytes(buf,sizeof(buf));
  if(n>0){
   last=millis();
   analog.write(buf,n);
  }else if(millis()-last>STREAM_EOF_IDLE_MS)break;
  delay(1);
 }
 h.end();
 audioStop();
 return true;
}

void playLocalMP3(const uint8_t*start,const uint8_t*end,const String&fallback=""){
 size_t len=end-start;
 if(!len)return;
 audioStart();
 memStream.set((uint8_t*)start,len);
 MP3DecoderHelix decoder;
 decoder.begin();
 StreamCopy cp(audio,memStream);
 uint32_t t=millis();
 while(memStream.available()&&millis()-t<30000){
  cp.copy();
  delay(1);
 }
 audioStop();
}

bool isOfflineCommand(const String&q){
 String s=normCmd(q);
 return s=="online"||s=="tars online"||s=="mode online"||
        s=="mode online kan"||s=="kembali online";
}

void processOffline(const String&q){
 String s=normCmd(q);
 if(isOfflineCommand(q)){
  tarsMode=MODE_ONLINE;
  oledShowText("ONLINE","MODE");
  oledSetStatus("LISTENING");
  return;
 }
 if(s.indexOf("ambil ")==0){
  runPickObject(s.substring(6));
  return;
 }
 oledShowText(q,"OFFLINE");
 oledSetStatus("READY");
}

bool alarmDue(){
 return false;
}
void runAlarm(){
}

void processQuestion(const String&q){
 String nq=normCmd(q);
 if(tarsMode==MODE_OFFLINE){processOffline(q);return;}
 if(nq=="offline"||nq=="off line"||nq=="tars offline"||
    nq=="tars off line"||nq=="mode offline"||
    nq=="mode off line"||nq=="tars mode offline"||
    nq=="tars mode off line"){
  Serial.println("TARS: SWITCH ONLINE -> OFFLINE");
  tarsMode=MODE_OFFLINE;
  sttWS.disconnect();
  sttConnected=sttReady=sttDone=sttError=false;
  oledShowText("OFFLINE","ONLINE");
  oledSetStatus("READY");
  Serial.println("TARS: MODE OFFLINE");
  return;
 }
 oledShowText(q,"STT");
 delay(300);
 bool status=isStatusQuery(q);
 String answer;
 if(isVisionCmd(q)){
  Serial.println("TARS: VISION REQUEST");
  oledSetStatus("VISION");
  answer=visionAsk(q);
 }else if(status){
  answer=systemStatus();
 }else{
  answer=ask(q);
 }
 if(!answer.length()){
  oledSetStatus("ERROR");
  restoreCameraAfterDAC();
  behaviorReset();
  return;
 }
 oledShowText(answer,"ASK");
 delay(300);
 bool ok=streamAudio(String(TARS_CLOUD_URL)+"/tts",answer);
 oledSetStatus(ok?"LISTENING":"AUDIO ERROR");
 behaviorReset();
}

void setup(){
 Serial.begin(115200);
 delay(300);
 pinMode(MOTOR_ARM_A1,OUTPUT);pinMode(MOTOR_ARM_A2,OUTPUT);
 pinMode(MOTOR_GRIP_B1,OUTPUT);pinMode(MOTOR_GRIP_B2,OUTPUT);
 motorStop();
 Wire.begin(21,22);
 oledOK=display.begin(SSD1306_SWITCHCAPVCC,0x3C);
 if(oledOK){
  display.clearDisplay();display.display();
  behavior=BEH_WAKE;drawFace();delay(400);
  behavior=BEH_IDLE;
 }
 micOK=initMic();
 if(!LittleFS.begin(true))Serial.println("TARS: LITTLEFS ERROR");
 else Serial.printf("TARS: LITTLEFS READY %u/%u KB\n",
  (unsigned)(LittleFS.usedBytes()/1024),(unsigned)(LittleFS.totalBytes()/1024));
 Serial.println("TARS: DAC GPIO26 INTERNAL DAC RIGHT");
 Serial.println("TARS: AUDIO MP3/WAV -> 22050Hz/16bit");
 Serial.println("TARS: INMP441 RIGHT GPIO16");
 Serial.printf("TARS: MIC THRESHOLD=%ld SILENCE=%ld\n",(long)MIC_THRESHOLD,(long)MIC_SILENCE);
 Serial.printf("TARS: OV7670 D0..D7=%d,%d,%d,%d,%d,%d,%d,%d XCLK=%d PCLK=%d VSYNC=%d SCCB=%d/%d\n",
  OV_D0,OV_D1,OV_D2,OV_D3,OV_D4,OV_D5,OV_D6,OV_D7,OV_XCLK,OV_PCLK,OV_VSYNC,OV_SIOD,OV_SIOC);
 Serial.printf("TARS: MOTOR A=%d/%d GRIP=%d/%d\n",
  MOTOR_ARM_A1,MOTOR_ARM_A2,MOTOR_GRIP_B1,MOTOR_GRIP_B2);
 Serial.println("TARS: MODE ONLINE");
 Serial.println("TARS: BLUETOOTH DISABLED");
 wifiManagerBegin();
 if(bootWiFi()){
  if(syncTime())checkTimeGreeting();
 }
 Serial.println("TARS: BOOT CONFIG DONE -> CAMERA MODE");
 cameraReady=initCamera();
 if(cameraReady){testCameraFrame();cameraActive=true;}
 oledText="";oledSetStatus("CAMERA");
 if(oledOK)xTaskCreatePinnedToCore(oledTask,"TARS_OLED",4096,nullptr,1,nullptr,0);
}

void loop(){
 if(playing){delay(1);return;}
 if(!wifiOK()){
  if(!wifiOK()){oledSetStatus("WIFI ERROR");delay(500);return;}
 }
 if(alarmDue()){runAlarm();return;}
 checkTimeGreeting();
 if(playing){delay(1);return;}
 String q=tarsMode==MODE_ONLINE?recordRealtime():recordOffline();
 if(q.length())processQuestion(q);
 else if(!specialActive()&&!cameraActive)
  oledSetStatus(tarsMode==MODE_ONLINE?"LISTENING":"READY");
 delay(1);
}
