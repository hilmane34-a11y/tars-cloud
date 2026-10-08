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
#include "tars_emotion.h"

extern const uint8_t follow_start[] asm("_binary_src_follow_mp3_start");
extern const uint8_t follow_end[] asm("_binary_src_follow_mp3_end");
extern const uint8_t online_start[] asm("_binary_src_online_mp3_start");
extern const uint8_t online_end[] asm("_binary_src_online_mp3_end");
extern const uint8_t offline_start[] asm("_binary_src_offline_mp3_start");
extern const uint8_t offline_end[] asm("_binary_src_offline_mp3_end");
extern const uint8_t hari_start[] asm("_binary_src_hari_mp3_start");
extern const uint8_t hari_end[] asm("_binary_src_hari_mp3_end");
extern const uint8_t pagi_start[] asm("_binary_src_pagi_mp3_start");
extern const uint8_t pagi_end[] asm("_binary_src_pagi_mp3_end");
extern const uint8_t siang_start[] asm("_binary_src_siang_mp3_start");
extern const uint8_t siang_end[] asm("_binary_src_siang_mp3_end");
extern const uint8_t sore_start[] asm("_binary_src_sore_mp3_start");
extern const uint8_t sore_end[] asm("_binary_src_sore_mp3_end");
extern const uint8_t malam_start[] asm("_binary_src_malam_mp3_start");
extern const uint8_t malam_end[] asm("_binary_src_malam_mp3_end");
extern const uint8_t alarm_start[] asm("_binary_src_alarm_mp3_start");
extern const uint8_t alarm_end[] asm("_binary_src_alarm_mp3_end");

extern void oledStartSpeak(const String&);
extern void oledSetStatus(const String&);

bool playing=false,dacOK=false;
static const int MP3_COPY_BUFFER=512;
static const size_t AUDIO_RING_SIZE=6144,AUDIO_PREBUFFER=1028;

class MemMP3Stream:public Stream{
  const uint8_t*data;size_t len,pos;
public:
  MemMP3Stream():data(nullptr),len(0),pos(0){}
  void begin(const uint8_t*p,const uint8_t*e){data=p;len=(p&&e&&e>p)?(size_t)(e-p):0;pos=0;}
  int available()override{return(!data||pos>=len)?0:(int)(len-pos);}
  int read()override{return(!data||pos>=len)?-1:data[pos++];}
  int read(uint8_t*out,size_t n){if(!data||!out||pos>=len)return 0;n=min(n,len-pos);memcpy(out,data+pos,n);pos+=n;return(int)n;}
  int peek()override{return(!data||pos>=len)?-1:data[pos];}
  void flush()override{}
  size_t write(uint8_t)override{return 0;}
  size_t write(const uint8_t*,size_t)override{return 0;}
  void reset(){pos=0;}
};

static MemMP3Stream localMP3,alarmStream;

class AudioRingStream:public Stream{
  uint8_t b[AUDIO_RING_SIZE];
  volatile size_t h=0,t=0,n=0;
  volatile bool done=true,stopFlag=true,running=false;
  TaskHandle_t task=nullptr;
  portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
  WiFiClient*src=nullptr;
  int expected=-1,received=0;
  uint32_t lastRx=0;
  bool gotData=false;
public:
  void begin(WiFiClient*client,int expectedLength=-1){
    stop();src=client;expected=expectedLength;received=0;lastRx=millis();
    gotData=false;done=false;stopFlag=false;running=true;h=t=n=0;
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
  static void entry(void*p){AudioRingStream*s=(AudioRingStream*)p;s->rx();s->task=nullptr;vTaskDelete(nullptr);}
  void rx(){
    uint8_t temp[512];
    while(!stopFlag&&src){
      if(!src->connected())break;
      int av=src->available();
      if(av<=0){
        if(expected>=0&&received>=expected)break;
        if(gotData&&millis()-lastRx>5000)break;
        vTaskDelay(pdMS_TO_TICKS(2));continue;
      }
      size_t want=min((size_t)av,sizeof(temp));
      int got=src->read(temp,want);if(got<=0)continue;
      for(int i=0;i<got;i++){
        while(!stopFlag){
          portENTER_CRITICAL(&mux);bool full=n>=AUDIO_RING_SIZE;portEXIT_CRITICAL(&mux);
          if(!full)break;
          vTaskDelay(pdMS_TO_TICKS(1));
        }
        if(stopFlag)break;
        portENTER_CRITICAL(&mux);
        b[h++]=temp[i];if(h>=AUDIO_RING_SIZE)h=0;n++;
        portEXIT_CRITICAL(&mux);
      }
      received+=got;lastRx=millis();gotData=true;
    }
    done=true;running=false;
  }
  int available()override{
    portENTER_CRITICAL(&mux);size_t v=n;portEXIT_CRITICAL(&mux);return(int)v;
  }
  int read()override{uint8_t c;return read(&c,1)==1?c:-1;}
  int read(uint8_t*out,size_t len){
    if(!out||!len)return 0;
    size_t got=0;
    while(got<len){
      portENTER_CRITICAL(&mux);
      if(!n){portEXIT_CRITICAL(&mux);break;}
      out[got++]=b[t++];if(t>=AUDIO_RING_SIZE)t=0;n--;
      portEXIT_CRITICAL(&mux);
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
};

static AudioRingStream audioRing;

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
    if(!p||!n)return 0;
    decBytes+=n;size_t d=0;
    while(d<n){
      size_t w=out->write(p+d,n-d);
      if(w)d+=w;else{delay(1);yield();}
    }
    dacBytes+=d;return d;
  }
  int availableForWrite()override{return out->availableForWrite();}
  int available()override{return 0;}
  int read()override{return-1;}
  int peek()override{return-1;}
  void flush()override{}
  void report(){Serial.printf("TARS: PCM BYTES=%llu DAC=%llu\n",(unsigned long long)decBytes,(unsigned long long)dacBytes);}
};

static AudioESP32ULP dac;
static MP3DecoderHelix codec;
static WAVDecoder wav;
static PCMProbeStream pcmProbe(dac);
static ResampleStream mp3Resample(pcmProbe),wavResample(pcmProbe);
static EncodedAudioStream dec(&mp3Resample,&codec);
static EncodedAudioStream wavDec(&wavResample,&wav);
static StreamCopy copier(MP3_COPY_BUFFER);
static bool dacLinksReady=false;

bool initDAC(){
  AudioInfo info(22050,1,16);
  dac.setMonoDAC(ULP_DAC2);
  if(!dac.begin(info)){Serial.println("TARS: DAC INIT FAILED");dacOK=false;return false;}
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
  if(dacOK)return true;
  if(!initDAC())return false;
  playing=true;
  return true;
}

void audioStop(){
  if(dacOK)dac.end();
  dacOK=false;playing=false;
}

static bool playMemoryMP3(const uint8_t*start,const uint8_t*end,const String&name){
  if(!start||!end||end<=start)return false;
  if(!audioStart())return false;
  localMP3.begin(start,end);
  pcmProbe.reset();
  oledStartSpeak(name);
  bool ok=dec.begin();
  if(!ok){
    Serial.printf("TARS: HELIX LOCAL START FAILED HEAP=%u MAX=%u\n",ESP.getFreeHeap(),ESP.getMaxAllocHeap());
    audioStop();return false;
  }
  audio_tools::AudioInfo src=codec.audioInfo();
  ok=mp3Resample.begin(src,22050);
  if(ok){
    copier.begin(dec,localMP3);
    while(localMP3.available()>0)copier.copy();
    mp3Resample.flush();
    mp3Resample.end();
  }
  dec.end();
  pcmProbe.report();
  uint64_t bytes=pcmProbe.decodedBytes();
  audioStop();
  oledSetStatus("READY");
  return ok&&bytes>0;
}

bool playLocalMP3(const uint8_t*start,const uint8_t*end,const String&name,bool alarm){
  if(alarm)alarmStream.begin(start,end);
  return playMemoryMP3(start,end,name);
}

bool playLocalAlarm(){
  return playMemoryMP3(alarm_start,alarm_end,"ALARM");
}

bool streamAudio(const String&url,const String&text){
  if(WiFi.status()!=WL_CONNECTED)return false;
  WiFiClientSecure client;client.setInsecure();
  HTTPClient http;
  if(!http.begin(client,url)){Serial.println("TARS: TTS HTTP BEGIN FAILED");return false;}
  http.setTimeout(15000);
  http.addHeader("Content-Type","application/json");
  const char*hdrs[]={"Content-Type","X-TARS-TTS"};
  http.collectHeaders(hdrs,2);
  JsonDocument j;j["text"]=text;
  String body;serializeJson(j,body);
  int code=http.POST(body);body="";
  Serial.printf("TARS: TTS HTTP=%d\n",code);
  if(code<200||code>=300){http.end();return false;}

  int expected=http.getSize();
  WiFiClient*src=http.getStreamPtr();
  if(!src){http.end();return false;}

  String ct=http.header("Content-Type");
  String xt=http.header("X-TARS-TTS");
  ct.toLowerCase();xt.toLowerCase();
  bool isWav=ct.indexOf("wav")>=0||ct.indexOf("wave")>=0||xt.indexOf("wav")>=0;
  Serial.printf("TARS: TTS FORMAT=%s\n",isWav?"WAV":"MP3");

  if(!audioStart()){http.end();return false;}
  pcmProbe.reset();
  oledStartSpeak(text);
  audioRing.begin(src,expected);

  uint32_t waitStart=millis();
  while(audioRing.available()<AUDIO_PREBUFFER&&!audioRing.finished()&&millis()-waitStart<5000){
    delay(2);yield();
  }

  bool ok=false;
  if(isWav){
    ok=wavDec.begin();
    if(ok){
      audio_tools::AudioInfo info=wav.audioInfo();
      ok=wavResample.begin(info,22050);
    }
    if(ok){
      copier.begin(wavDec,audioRing);
      uint32_t last=millis();
      while(true){
        int before=audioRing.available();
        bool copied=copier.copy();
        int after=audioRing.available();
        if(copied){last=millis();}
        else if(audioRing.finished()&&!audioRing.available())break;
        else if(before==after)delay(1);
        if(millis()-last>10000)break;
        yield();
      }
      wavResample.flush();
      wavResample.end();
    }
    wavDec.end();
  }else{
    ok=dec.begin();
    if(ok){
      audio_tools::AudioInfo info=codec.audioInfo();
      ok=mp3Resample.begin(info,22050);
    }
    if(ok){
      copier.begin(dec,audioRing);
      uint32_t last=millis();
      while(true){
        int before=audioRing.available();
        bool copied=copier.copy();
        int after=audioRing.available();
        if(copied){last=millis();}
        else if(audioRing.finished()&&!audioRing.available())break;
        else if(before==after)delay(1);
        if(millis()-last>10000)break;
        yield();
      }
      mp3Resample.flush();
      mp3Resample.end();
    }
    dec.end();
  }

  audioRing.stop();
  pcmProbe.report();
  audioStop();
  http.end();
  playing=false;
  oledSetStatus("LISTENING");
  Serial.printf("TARS: TTS AUDIO %s\n",ok?"DONE":"FAILED");
  return ok;
}
