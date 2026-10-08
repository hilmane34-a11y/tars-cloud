#include "tts.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "AudioTools.h"
#include "AudioTools/AudioLibs/AudioESP32ULP.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "AudioTools/AudioCodecs/CodecWAV.h"
#include "config.h"
#include "wifi_manager.h"
#include "tars_emotion.h"

extern const uint8_t alarm_start[];
extern const uint8_t alarm_end[];
extern const uint8_t follow_start[];
extern const uint8_t follow_end[];
extern const uint8_t online_start[];
extern const uint8_t online_end[];
extern const uint8_t offline_start[];
extern const uint8_t offline_end[];
extern const uint8_t hari_start[];
extern const uint8_t hari_end[];
extern const uint8_t pagi_start[];
extern const uint8_t pagi_end[];
extern const uint8_t siang_start[];
extern const uint8_t siang_end[];
extern const uint8_t sore_start[];
extern const uint8_t sore_end[];
extern const uint8_t malam_start[];
extern const uint8_t malam_end[];

extern void oledStartSpeak(const String&);
extern void oledSetStatus(const String&);

bool playing=false;
bool dacOK=false;

static const int MP3_COPY_BUFFER=512;
static const size_t AUDIO_RING_SIZE=6144;
static const size_t AUDIO_PREBUFFER=1028;

class MemMP3Stream:public Stream{
public:
  MemMP3Stream():data(nullptr),len(0),pos(0){}
  void begin(const uint8_t*p,const uint8_t*e){data=p;len=(e>p)?(size_t)(e-p):0;pos=0;}
  int available()override{if(!data||pos>=len)return 0;return(int)(len-pos);}
  int read()override{if(!data||pos>=len)return-1;return data[pos++];}
  int read(uint8_t*out,size_t n){
    if(!data||!out||pos>=len)return 0;
    size_t remain=len-pos;if(n>remain)n=remain;
    memcpy(out,data+pos,n);pos+=n;return(int)n;
  }
  int peek()override{if(!data||pos>=len)return-1;return data[pos];}
  void flush()override{}
  size_t write(uint8_t)override{return 0;}
  size_t write(const uint8_t*,size_t)override{return 0;}
  void reset(){pos=0;}
private:
  const uint8_t*data;size_t len,pos;
};

static MemMP3Stream localMP3;
static MemMP3Stream alarmStream;

class AudioRingStream:public Stream{
public:
  AudioRingStream():h(0),t(0),n(0),done(false),stopFlag(false),running(false),task(nullptr),src(nullptr),expected(-1),received(0),lastRx(0),gotData(false){}
  void begin(WiFiClient*client,int expectedLength=-1){
    stop();src=client;expected=expectedLength;received=0;lastRx=millis();gotData=false;done=false;stopFlag=false;running=true;
    h=0;t=0;n=0;
    xTaskCreatePinnedToCore(entry,"tts_rx",3000,this,1,&task,0);
  }
  void stop(){
    stopFlag=true;running=false;
    if(task){
      TaskHandle_t old=task;task=nullptr;
      if(old!=xTaskGetCurrentTaskHandle())vTaskDelete(old);
    }
    src=nullptr;done=true;
  }
  static void entry(void*p){
    AudioRingStream*self=static_cast<AudioRingStream*>(p);
    self->rx();self->task=nullptr;vTaskDelete(nullptr);
  }
  void rx(){
    uint8_t temp[512];
    while(!stopFlag&&src){
      if(!src->connected())break;
      int avail=src->available();
      if(avail<=0){
        if(expected>=0&&received>=expected)break;
        if(gotData&&millis()-lastRx>5000)break;
        vTaskDelay(pdMS_TO_TICKS(2));continue;
      }
      size_t want=(size_t)avail;if(want>sizeof(temp))want=sizeof(temp);
      int got=src->read(temp,want);if(got<=0)continue;
      for(int i=0;i<got;i++){
        while(!stopFlag){
          portENTER_CRITICAL(&mux);bool full=n>=AUDIO_RING_SIZE;portEXIT_CRITICAL(&mux);
          if(!full)break;
          vTaskDelay(pdMS_TO_TICKS(1));
        }
        if(stopFlag)break;
        portENTER_CRITICAL(&mux);
        b[h]=temp[i];h++;if(h>=AUDIO_RING_SIZE)h=0;n++;
        portEXIT_CRITICAL(&mux);
      }
      received+=got;lastRx=millis();gotData=true;
    }
    done=true;running=false;
  }
  int available()override{
    portENTER_CRITICAL(&mux);size_t v=n;portEXIT_CRITICAL(&mux);return(int)v;
  }
  int read()override{
    uint8_t c;if(read(&c,1)!=1)return-1;return c;
  }
  int read(uint8_t*out,size_t len){
    if(!out||!len)return 0;
    size_t got=0;
    while(got<len){
      portENTER_CRITICAL(&mux);
      if(!n){portEXIT_CRITICAL(&mux);break;}
      out[got]=b[t];t++;if(t>=AUDIO_RING_SIZE)t=0;n--;
      portEXIT_CRITICAL(&mux);got++;
    }
    return(int)got;
  }
  int peek()override{
    portENTER_CRITICAL(&mux);
    if(!n){portEXIT_CRITICAL(&mux);return-1;}
    int c=b[t];portEXIT_CRITICAL(&mux);return c;
  }
  void flush()override{}
  size_t write(uint8_t)override{return 0;}
  size_t write(const uint8_t*,size_t)override{return 0;}
  bool finished(){return done&&available()==0;}
  bool hasData(){return available()>0;}
private:
  uint8_t b[AUDIO_RING_SIZE];
  volatile size_t h,t,n;
  volatile bool done,stopFlag,running;
  TaskHandle_t task;
  portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
  WiFiClient*src;
  int expected,received;
  uint32_t lastRx;
  bool gotData;
};

static AudioRingStream audioRing;

class PCMProbeStream:public AudioStream{
public:
  PCMProbeStream(AudioOutput*output):out(output),decBytes(0),dacBytes(0){}
  size_t write(const uint8_t*data,size_t len)override{
    if(!data||!len)return 0;
    decBytes+=len;if(out)dacBytes+=len;
    return out?out->write(data,len):len;
  }
  int available()override{return 0;}
  int read()override{return-1;}
  int peek()override{return-1;}
  void flush()override{}
  uint64_t decodedBytes()const{return decBytes;}
  uint64_t outputBytes()const{return dacBytes;}
private:
  AudioOutput*out;
  uint64_t decBytes,dacBytes;
};

static AudioESP32ULP dac;
static MP3DecoderHelix codec;
static WAVDecoder wav;
static PCMProbeStream pcmProbe(&dac);
static ResampleStream mp3Resample(pcmProbe);
static ResampleStream wavResample(pcmProbe);
static EncodedAudioStream dec(&mp3Resample,&codec);
static EncodedAudioStream wavDec(&wavResample,&wav);
static StreamCopy copier(MP3_COPY_BUFFER);
static bool dacLinksReady=false;

bool initDAC(){
  AudioInfo info(22050,1,16);
  dac.setMonoDAC(ULP_DAC2);
  if(!dac.begin(info)){
    Serial.println("TARS: DAC INIT FAILED");
    dacOK=false;
    return false;
  }
  if(!dacLinksReady){
    dec.addNotifyAudioChange(mp3Resample);
    mp3Resample.addNotifyAudioChange(pcmProbe);
    wavDec.addNotifyAudioChange(wavResample);
    wavResample.addNotifyAudioChange(pcmProbe);
    dacLinksReady=true;
  }
  dacOK=true;
  Serial.println("TARS: DAC GPIO26 ULP DAC2");
  return true;
}

bool audioStart(){
  if(!dacOK&&!initDAC())return false;
  playing=true;
  return true;
}

void audioStop(){
  dac.end();
  playing=false;
  dacOK=false;
}

static bool playMemoryMP3(const uint8_t*start,const uint8_t*end,const String&name){
  if(!start||!end||end<=start)return false;
  if(!audioStart())return false;
  localMP3.begin(start,end);
  oledStartSpeak(name);
  copier.begin(localMP3,dec);
  uint32_t lastData=millis();
  while(localMP3.available()||copier.available()){
    copier.copy();
    if(localMP3.available())lastData=millis();
    if(millis()-lastData>3000)break;
    yield();
  }
  copier.end();
  audioStop();
  return true;
}

bool playLocalMP3(const uint8_t*start,const uint8_t*end,const String&name,bool alarm){
  if(alarm)alarmStream.begin(start,end);
  else localMP3.begin(start,end);
  return playMemoryMP3(start,end,name);
}

bool playLocalAlarm(){
  return playMemoryMP3(alarm_start,alarm_end,"ALARM");
}

bool streamAudio(const String&url,const String&text){
  if(!wifiOK())return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  if(!http.begin(client,url)){
    Serial.println("TARS: TTS HTTP BEGIN FAILED");
    return false;
  }

  http.setTimeout(15000);
  http.addHeader("Content-Type","application/json");

  JsonDocument j;
  j["text"]=text;

  String body;
  serializeJson(j,body);

  int code=http.POST(body);
  body="";

  Serial.printf("TARS: TTS HTTP=%d\n",code);

  if(code<200||code>=300){
    http.end();
    return false;
  }

  int expected=http.getSize();
  WiFiClient*src=http.getStreamPtr();

  if(!src){
    http.end();
    return false;
  }

  if(!audioStart()){
    http.end();
    return false;
  }

  oledStartSpeak(text);
  audioRing.begin(src,expected);

  uint32_t started=millis();
  uint32_t lastAudio=millis();

  while(true){
    if(audioRing.available()){
      size_t copied=copier.copy();
      if(copied)lastAudio=millis();
      if(!copied)vTaskDelay(pdMS_TO_TICKS(1));
    }else{
      if(audioRing.finished()&&millis()-lastAudio>250)break;
      if(millis()-started>20000)break;
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    yield();
  }

  audioRing.stop();
  audioStop();
  http.end();
  playing=false;

  Serial.println("TARS: TTS AUDIO DONE");
  return true;
}
