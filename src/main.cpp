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

const uint32_t MIC_RATE=16000,PLAY_RATE=22050;
const uint32_t RECORD_MAX_MS=5000,RECORD_MIN_MS=700,SILENCE_MS=1500;
const int32_t MIC_THRESHOLD=12000,MIC_SILENCE=10000;
const size_t BUF=1024,DAC_BUF=16384;
const char *STT_FILE="/stt.wav";

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);
bool oledOK=false,micOK=false,dacOK=false,playing=false;
String oledText;size_t oledPos=0;
uint32_t oledTick=0,dotTick=0;uint8_t dotState=1;

int16_t dacRing[DAC_BUF];
volatile size_t dacHead=0,dacTail=0;
SemaphoreHandle_t dacMutex=nullptr;
TaskHandle_t dacTaskHandle=nullptr;
volatile bool dacRunning=false;
uint32_t dacCalls=0,dacSamples=0;int32_t dacPeak=0;
uint32_t ditherSeed=0x12345678UL;

void oledHeader(const char*s){
  if(!oledOK)return;
  oled.clearDisplay();oled.setTextSize(1);oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(42,0);oled.print("T A R S");oled.drawLine(3,9,124,9,SSD1306_WHITE);
  oled.setCursor(3,14);oled.print(s);oled.display();
}

void oledListening(){
  if(!oledOK||millis()-dotTick<350)return;
  dotTick=millis();if(++dotState>4)dotState=1;
  oled.clearDisplay();oled.setTextSize(1);oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(42,0);oled.print("T A R S");oled.drawLine(3,9,124,9,SSD1306_WHITE);
  oled.setCursor(3,14);oled.print("LISTENING");oled.setCursor(3,27);
  for(uint8_t i=0;i<dotState;i++)oled.print(".");
  oled.display();
}

void oledType(){
  if(!oledOK||!oledText.length()||millis()-oledTick<35)return;
  oledTick=millis();static int x=3,y=27;static size_t last=0;
  if(oledPos==0){x=3;y=27;last=0;oledHeader(playing?"SPEAKING":"READY");}
  if(oledPos<oledText.length())oledPos++;
  while(last<oledPos){
    char c=oledText[last++];
    if(c=='\n')x=3,y+=8;
    else{
      if(x>121)x=3,y+=8;
      if(y>59){oledHeader(playing?"SPEAKING":"READY");x=3;y=27;}
      oled.setCursor(x,y);oled.write(c);x=oled.getCursorX();
    }
    if(y>59){oledHeader(playing?"SPEAKING":"READY");x=3;y=27;}
  }
  oled.display();
}

static inline uint8_t pcmToDAC(int32_t s){
  int32_t v=(s*2)/3;
  ditherSeed=ditherSeed*1664525UL+1013904223UL;
  int32_t a=(ditherSeed>>24)&255;
  ditherSeed=ditherSeed*1664525UL+1013904223UL;
  int32_t b=(ditherSeed>>24)&255;
  v+=(a-b)>>1;
  if(v>32767)v=32767;if(v<-32768)v=-32768;
  return (uint8_t)((v+32768+128)>>8);
}

size_t dacAvailable(){
  size_t h=dacHead,t=dacTail;
  return h>=t?h-t:DAC_BUF-t+h;
}
size_t dacFree(){return DAC_BUF-1-dacAvailable();}

void dacTask(void*){
  uint32_t last=micros(),frac=0;
  while(dacRunning){
    if(!dacAvailable()){dacWrite(AUDIO_DAC_PIN,0);last=micros();taskYIELD();continue;}
    xSemaphoreTake(dacMutex,portMAX_DELAY);
    int16_t s=dacRing[dacTail++];if(dacTail>=DAC_BUF)dacTail=0;
    xSemaphoreGive(dacMutex);

    uint32_t e=micros()-last;if(e<45)delayMicroseconds(45-e);last=micros();
    frac+=1000000UL;uint32_t us=frac/PLAY_RATE;frac%=PLAY_RATE;
    if(us)delayMicroseconds(us-1);

    int32_t p=s<0?-s:s;if(p>dacPeak)dacPeak=p;
    dacWrite(AUDIO_DAC_PIN,pcmToDAC(s));dacCalls++;dacSamples++;
    if(!(dacSamples&1023))taskYIELD();
  }
  dacWrite(AUDIO_DAC_PIN,0);dacTaskHandle=nullptr;vTaskDelete(nullptr);
}

bool initDAC(){
  dacWrite(AUDIO_DAC_PIN,0);dacMutex=xSemaphoreCreateMutex();
  if(!dacMutex)return false;
  dacOK=true;Serial.println("TARS: DAC GPIO26 READY");return true;
}

bool startDAC(){
  if(!dacOK||dacTaskHandle)return dacOK;
  xSemaphoreTake(dacMutex,portMAX_DELAY);
  dacHead=dacTail=0;dacCalls=dacSamples=0;dacPeak=0;
  xSemaphoreGive(dacMutex);dacRunning=true;dacWrite(AUDIO_DAC_PIN,0);
  if(xTaskCreatePinnedToCore(dacTask,"TARS_DAC",4096,nullptr,2,&dacTaskHandle,1)!=pdPASS){
    dacRunning=false;dacTaskHandle=nullptr;return false;
  }
  return true;
}

size_t writeDAC(const int16_t*p,size_t n){
  size_t w=0;
  while(w<n){
    size_t f=dacFree();if(!f){vTaskDelay(1);continue;}
    size_t k=min(n-w,f);
    xSemaphoreTake(dacMutex,portMAX_DELAY);
    for(size_t i=0;i<k;i++){dacRing[dacHead++]=p[w+i];if(dacHead>=DAC_BUF)dacHead=0;}
    xSemaphoreGive(dacMutex);w+=k;
  }
  return w;
}

void waitDACDrain(){
  uint32_t t=millis();
  while(dacAvailable()&&millis()-t<10000)delay(1);
  delay(5);
}

void stopDAC(){
  dacRunning=false;uint32_t t=millis();
  while(dacTaskHandle&&millis()-t<2000)delay(1);
  dacWrite(AUDIO_DAC_PIN,0);
  Serial.printf("TARS: DAC CALL=%lu PCM=%lu PEAK=%ld\n",
    (unsigned long)dacCalls,(unsigned long)dacSamples,(long)dacPeak);
}

bool initMic(){
  i2s_config_t c={};
  c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_RX);
  c.sample_rate=MIC_RATE;c.bits_per_sample=I2S_BITS_PER_SAMPLE_32BIT;
  c.channel_format=I2S_CHANNEL_FMT_ONLY_RIGHT;
  c.communication_format=I2S_COMM_FORMAT_I2S;
  c.intr_alloc_flags=ESP_INTR_FLAG_LEVEL1;c.dma_buf_count=4;c.dma_buf_len=256;
  if(i2s_driver_install(MIC_PORT,&c,0,nullptr)!=ESP_OK)return false;

  i2s_pin_config_t p={};
  p.mck_io_num=I2S_PIN_NO_CHANGE;p.bck_io_num=MIC_SCK;
  p.ws_io_num=MIC_WS;p.data_out_num=I2S_PIN_NO_CHANGE;p.data_in_num=MIC_SD;
  if(i2s_set_pin(MIC_PORT,&p)!=ESP_OK)return false;
  i2s_zero_dma_buffer(MIC_PORT);micOK=true;
  Serial.printf("TARS: INMP441 READY THRESHOLD=%ld\n",(long)MIC_THRESHOLD);
  return true;
}

bool recordMic(){
  if(!micOK)return false;
  File f=LittleFS.open(STT_FILE,"w");if(!f)return false;
  uint8_t h[44]={};f.write(h,44);
  uint32_t start=millis(),speechStart=0,lastVoice=0,total=0;
  bool speech=false;int16_t pcm[BUF/2];

  Serial.println("TARS: LISTENING");

  while(millis()-start<RECORD_MAX_MS){
    size_t br=0;
    if(i2s_read(MIC_PORT,pcm,sizeof(pcm),&br,pdMS_TO_TICKS(100))!=ESP_OK)continue;
    size_t n=br/2;if(!n)continue;
    int32_t peak=0;

    for(size_t i=0;i<n;i++){int32_t v=pcm[i];if(v<0)v=-v;if(v>peak)peak=v;}
    Serial.printf("TARS: MIC PEAK=%ld\n",(long)peak);

    if(!speech){
      if(peak<MIC_THRESHOLD)continue;
      speech=true;speechStart=millis();lastVoice=speechStart;
      Serial.println("TARS: SPEECH START");
    }

    if(peak>=MIC_SILENCE)lastVoice=millis();
    f.write((uint8_t*)pcm,br);total+=n;

    uint32_t now=millis();
    if(now-speechStart>=RECORD_MIN_MS&&now-lastVoice>=SILENCE_MS){
      Serial.printf("TARS: SPEECH END AFTER %lu ms SILENCE\n",
        (unsigned long)(now-lastVoice));break;
    }
  }

  if(!speech){
    f.close();LittleFS.remove(STT_FILE);
    Serial.println("TARS: MIC AUDIO TOO LOW");return false;
  }

  uint32_t ds=total*2,fs=ds+36,br=MIC_RATE*2;
  memcpy(h,"RIFF",4);
  h[4]=(uint8_t)fs;h[5]=(uint8_t)(fs>>8);h[6]=(uint8_t)(fs>>16);h[7]=(uint8_t)(fs>>24);
  memcpy(h+8,"WAVEfmt ",8);h[16]=16;h[20]=1;h[22]=1;
  h[24]=(uint8_t)MIC_RATE;h[25]=(uint8_t)(MIC_RATE>>8);
  h[26]=(uint8_t)(MIC_RATE>>16);h[27]=(uint8_t)(MIC_RATE>>24);
  h[28]=(uint8_t)br;h[29]=(uint8_t)(br>>8);h[30]=(uint8_t)(br>>16);h[31]=(uint8_t)(br>>24);
  h[32]=2;h[34]=16;memcpy(h+36,"data",4);
  h[40]=(uint8_t)ds;h[41]=(uint8_t)(ds>>8);h[42]=(uint8_t)(ds>>16);h[43]=(uint8_t)(ds>>24);
  f.seek(0);f.write(h,44);f.close();

  Serial.printf("TARS: RECORD TIME=%lu ms\n",(unsigned long)(millis()-start));
  return true;
}

bool syncTime(){
  configTime(7*3600,0,"pool.ntp.org","time.nist.gov","time.google.com");
  for(int i=0;i<4;i++){
    struct tm t;
    if(getLocalTime(&t,5000)){
      Serial.printf("TARS: NTP OK %04d-%02d-%02d %02d:%02d:%02d\n",
        t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec);
      return true;
    }
    Serial.printf("TARS: NTP RETRY %d/4\n",i+1);
  }
  Serial.println("TARS: NTP FAILED");return false;
}

bool wifiOK(){
  if(!wifiManagerBegin()||!wifiManagerConnect(true))return false;
  Serial.printf("TARS: WIFI READY %s\n",WiFi.localIP().toString().c_str());
  return syncTime();
}

String stt(){
  File f=LittleFS.open(STT_FILE,"r");if(!f)return "";
  WiFiClientSecure c;c.setInsecure();HTTPClient h;
  if(!h.begin(c,String(TARS_CLOUD_URL)+"/stt")){f.close();return "";}
  h.addHeader("Content-Type","audio/wav");h.addHeader("X-Filename","stt.wav");

  size_t sz=f.size();uint8_t*d=(uint8_t*)malloc(sz);
  if(!d){f.close();h.end();return "";}
  f.read(d,sz);f.close();

  int code=h.POST(d,sz);free(d);
  Serial.printf("TARS: STT HTTP = %d\n",code);
  if(code!=200){Serial.println(h.getString());h.end();return "";}
  String r=h.getString();h.end();

  JsonDocument j;if(deserializeJson(j,r))return "";
  String text=j["text"]|"";text.trim();
  Serial.printf("TARS: YOU SAID = %s\n",text.c_str());
  return text;
}

String ask(const String&q){
  WiFiClientSecure c;c.setInsecure();HTTPClient h;
  if(!h.begin(c,String(TARS_CLOUD_URL)+"/ask"))return "";
  h.addHeader("Content-Type","application/json");

  JsonDocument j;j["text"]=q;String body;serializeJson(j,body);
  uint32_t t=millis();int code=h.POST(body);
  Serial.printf("TARS: ASK HTTP = %d\n",code);
  Serial.printf("TARS: ASK TIME=%lu ms\n",(unsigned long)(millis()-t));

  if(code!=200){Serial.println(h.getString());h.end();return "";}
  String r=h.getString();h.end();JsonDocument d;
  if(deserializeJson(d,r))return "";
  String a=d["answer"]|d["text"]|r;a.trim();
  Serial.printf("TARS: ANSWER = %s\n",a.c_str());return a;
}

bool downloadMP3(const String&text){
  WiFiClientSecure c;c.setInsecure();HTTPClient h;
  if(!h.begin(c,String(TARS_CLOUD_URL)+"/tts"))return false;
  h.addHeader("Content-Type","application/json");

  JsonDocument j;j["text"]=text;String body;serializeJson(j,body);
  uint32_t t=millis();int code=h.POST(body);
  Serial.printf("TARS: AUDIO HTTP = %d\n",code);
  Serial.printf("TARS: TTS TIME=%lu ms\n",(unsigned long)(millis()-t));

  if(code!=200){Serial.println(h.getString());h.end();return false;}
  File f=LittleFS.open(MP3_FILE,"w");if(!f){h.end();return false;}

  WiFiClient*s=h.getStreamPtr();uint8_t b[1024];int left=h.getSize();
  while(h.connected()){
    size_t a=s->available();
    if(a){
      size_t n=s->readBytes(b,min(a,sizeof(b)));if(n)f.write(b,n);
      if(left>0){left-=n;if(left<=0)break;}
    }else delay(1);
  }
  f.close();h.end();

  File x=LittleFS.open(MP3_FILE,"r");if(!x)return false;
  Serial.printf("TARS: MP3 BYTES = %u\n",(unsigned)x.size());x.close();return true;
}

class DACStream:public Print{
public:
  size_t write(const uint8_t*b,size_t n)override{
    if(!b||!n)return 0;return writeDAC((const int16_t*)b,n/2)*2;
  }
  size_t write(uint8_t v)override{return write(&v,1);}
};

bool playMP3(){
  File f=LittleFS.open(MP3_FILE,"r");if(!f)return false;
  Serial.printf("TARS: PLAY MP3 SIZE=%u\n",(unsigned)f.size());

  DACStream out;MP3DecoderHelix decoder(out);
  if(!decoder.begin()){f.close();Serial.println("TARS: MP3 DECODER ERROR");return false;}

  startDAC();playing=true;oledHeader("SPEAKING");
  uint8_t buf[2048];uint32_t start=millis();

  while(f.available()){
    size_t n=f.read(buf,sizeof(buf));if(!n)break;
    decoder.write(buf,n);oledType();yield();
  }

  decoder.end();waitDACDrain();stopDAC();playing=false;f.close();
  Serial.printf("TARS: PLAYBACK TIME=%lu ms\n",(unsigned long)(millis()-start));
  Serial.println("TARS: PLAYBACK DONE");return true;
}

void processQuestion(){
  uint32_t start=millis();
  if(!recordMic()){oledHeader("READY");return;}

  String q=stt();
  if(!q.length()){Serial.println("TARS: STT NO TEXT");oledHeader("READY");return;}
  Serial.printf("TARS: ASK = %s\n",q.c_str());

  String a=ask(q);
  if(!a.length()){oledHeader("READY");return;}

  oledText=a;oledPos=0;oledTick=0;Serial.println("TARS: TTS");

  if(!downloadMP3(a)){
    Serial.println("TARS: TTS DOWNLOAD FAILED");oledHeader("READY");return;
  }

  playMP3();oledPos=oledText.length();oledHeader("READY");
  Serial.printf("TARS: TOTAL PROCESS=%lu ms\n",(unsigned long)(millis()-start));
}

void setup(){
  Serial.begin(SERIAL_BAUD);delay(1000);
  Serial.println("\n========== TARS CLOUD V1 ==========");

  Wire.begin(OLED_SDA,OLED_SCL);
  if(oled.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR)){oledOK=true;oledHeader("BOOTING");}

  if(!LittleFS.begin(true))Serial.println("TARS: LITTLEFS ERROR");
  else Serial.println("TARS: LITTLEFS READY");

  if(!initMic())Serial.println("TARS: MIC ERROR");
  if(!initDAC())Serial.println("TARS: DAC ERROR");

  oledHeader("WIFI");
  if(wifiOK()){Serial.println("TARS: WIFI + NTP READY");oledHeader("READY");}
  else{Serial.println("TARS: WIFI/NTP FAILED");oledHeader("WIFI ERROR");}

  Serial.println("TARS: SYSTEM READY");
}

void loop(){
  if(!playing)oledListening();
  processQuestion();
  delay(10);
}
