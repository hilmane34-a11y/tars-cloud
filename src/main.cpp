#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <time.h>
#include <math.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
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
#define MIC_SD 34
#define AUDIO_DAC_PIN 26

const uint32_t MIC_RATE=16000,RECORD_MIN_MS=500,SILENCE_MS=1000;
const uint32_t PREROLL_MS=700,OLED_TYPE_MS=39,OLED_WAVE_MS=70,AUDIO_IDLE_MS=2500,OLED_PAGE_MS=2200;
const int32_t MIC_THRESHOLD=4500,MIC_SILENCE=3000;
const size_t BUF=2048,PREROLL_SAMPLES=MIC_RATE*PREROLL_MS/1000;
const int MP3_COPY_BUFFER=1024;
const float MP3_VOLUME=0.67f;
const size_t AUDIO_RING_SIZE=8192,AUDIO_PREBUFFER=3072;
const char* STT_HOST="tars-cloud-v1.hilmane34.workers.dev";

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);
AnalogAudioStream analog;
MP3DecoderHelix codec;
VolumeStream mp3Volume(analog);
EncodedAudioStream dec(&mp3Volume,&codec);
StreamCopy copier(MP3_COPY_BUFFER);
WAVDecoder wav;
AudioInfo audioIn(44100,1,16),audioOut(44100,2,16);
FormatConverterStream stereoOut(analog);
EncodedAudioStream wavDec(&stereoOut,&wav);
WebSocketsClient sttWS;

bool oledOK=false,micOK=false,dacOK=false,playing=false,ntpOK=false;
bool sttConnected=false,sttReady=false,sttDone=false,sttError=false;
String sttFinal,sttPartial,oledText,oledStatus="READY";
uint32_t oledTypePos=0,oledLastType=0,oledLastWave=0,oledPage=0,oledLastPage=0;
static int32_t rawBuf[BUF/4];
static int16_t pcmBuf[BUF/4],preBuf[PREROLL_SAMPLES],sendBuf[256];

class AudioRingStream:public Stream{
  uint8_t buf[AUDIO_RING_SIZE];
  volatile size_t head=0,tail=0,count=0;
  volatile bool done=false,abortRx=false,running=false,zeroActive=false;
  TaskHandle_t task=nullptr;
  portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
  WiFiClient*src=nullptr;
  int expected=-1,received=0;

  volatile uint32_t rxChunks=0,rxBytes=0,rxMaxGap=0,ringMin=AUDIO_RING_SIZE,ringMax=0;
  volatile uint32_t ringZero=0,ringLow=0,ringZeroMs=0,ringZeroStart=0;
  volatile uint32_t readCalls=0,readBytes=0,emptyReads=0;
  uint32_t lastRx=0;

  static void taskEntry(void*p){((AudioRingStream*)p)->rxTask();vTaskDelete(nullptr);}

  void rxTask(){
    uint8_t tmp[1024];uint32_t last=millis();
    while(!abortRx){
      int av=src?src->available():0;
      if(av>0){
        size_t used=available(),freeSpace=AUDIO_RING_SIZE>used?AUDIO_RING_SIZE-used:0;
        if(!freeSpace){vTaskDelay(1);continue;}
        size_t want=(size_t)av;
        if(want>sizeof(tmp))want=sizeof(tmp);
        if(want>freeSpace)want=freeSpace;
        int n=src->read(tmp,want);
        if(n>0){
          uint32_t now=millis();
          if(rxChunks){uint32_t g=now-lastRx;if(g>rxMaxGap)rxMaxGap=g;}
          lastRx=now;last=now;
          size_t p=push(tmp,n);rxBytes+=p;rxChunks++;received+=n;
          if(expected>=0&&received>=expected)break;
        }
      }else{
        if(expected>=0&&received>=expected)break;
        if(src&&!src->connected()&&millis()-last>50)break;
        if(expected<0&&millis()-last>1500&&available()==0)break;
        vTaskDelay(1);
      }
    }
    portENTER_CRITICAL(&mux);done=true;running=false;portEXIT_CRITICAL(&mux);
  }

  size_t push(const uint8_t*p,size_t n){
    if(!p||!n)return 0;
    portENTER_CRITICAL(&mux);
    size_t c=count,f=AUDIO_RING_SIZE-c;if(n>f)n=f;
    size_t h=head,r=AUDIO_RING_SIZE-h,x=n<r?n:r;
    memcpy(buf+h,p,x);if(n>x)memcpy(buf,p+x,n-x);
    head=(h+n)%AUDIO_RING_SIZE;count+=n;
    if(count<ringMin)ringMin=count;if(count>ringMax)ringMax=count;
    if(count<1024)ringLow++;
    portEXIT_CRITICAL(&mux);return n;
  }

public:
  void start(WiFiClient&s,int len=-1){
    stop();
    portENTER_CRITICAL(&mux);
    head=tail=count=0;done=false;abortRx=false;running=true;
    rxChunks=rxBytes=rxMaxGap=0;ringMin=AUDIO_RING_SIZE;ringMax=0;
    ringZero=ringLow=ringZeroMs=0;ringZeroStart=0;zeroActive=false;
    readCalls=readBytes=emptyReads=0;
    portEXIT_CRITICAL(&mux);
    src=&s;expected=len;received=0;lastRx=millis();setTimeout(50);
    xTaskCreatePinnedToCore(taskEntry,"TARS_AUDIO_RX",3072,this,2,&task,0);
  }

  void stop(){
    abortRx=true;uint32_t t=millis();
    while(running&&millis()-t<1000)vTaskDelay(1);
    if(task){vTaskDelete(task);task=nullptr;}
    if(zeroActive){ringZeroMs+=millis()-ringZeroStart;zeroActive=false;}
    portENTER_CRITICAL(&mux);running=false;done=true;portEXIT_CRITICAL(&mux);
    src=nullptr;
  }

  bool finished(){return done&&available()==0;}

  int available() override{
    portENTER_CRITICAL(&mux);int n=count;portEXIT_CRITICAL(&mux);
    if(n==0){
      if(!zeroActive){zeroActive=true;ringZeroStart=millis();ringZero++;}
    }else if(zeroActive){
      ringZeroMs+=millis()-ringZeroStart;zeroActive=false;
    }
    return n;
  }

  int read() override{uint8_t c;return read(&c,1)==1?c:-1;}

  int read(uint8_t*p,size_t n){
    if(!p||!n)return 0;
    portENTER_CRITICAL(&mux);
    size_t c=count;
    if(!c){emptyReads++;portEXIT_CRITICAL(&mux);return 0;}
    size_t take=n<c?n:c,t=tail,r=AUDIO_RING_SIZE-t,x=take<r?take:r;
    memcpy(p,buf+t,x);if(take>x)memcpy(p+x,buf,take-x);
    tail=(t+take)%AUDIO_RING_SIZE;count-=take;
    portEXIT_CRITICAL(&mux);
    readCalls++;readBytes+=take;return take;
  }

  int peek() override{
    portENTER_CRITICAL(&mux);int r=count?buf[tail]:-1;portEXIT_CRITICAL(&mux);return r;
  }

  void flush() override{
    portENTER_CRITICAL(&mux);head=tail=count=0;portEXIT_CRITICAL(&mux);
  }

  size_t write(uint8_t) override{return 0;}
  size_t write(const uint8_t*,size_t) override{return 0;}

  void printDiag(){
    if(zeroActive)ringZeroMs+=millis()-ringZeroStart;
    Serial.printf("TARS: MP3 DIAG RING MIN=%u MAX=%u LOW=%lu ZERO=%lu ZERO_MS=%lu\n",
      (unsigned)ringMin,(unsigned)ringMax,(unsigned long)ringLow,
      (unsigned long)ringZero,(unsigned long)ringZeroMs);
    Serial.printf("TARS: MP3 DIAG READ CALLS=%lu BYTES=%lu EMPTY=%lu\n",
      (unsigned long)readCalls,(unsigned long)readBytes,(unsigned long)emptyReads);
    Serial.printf("TARS: MP3 DIAG RX CHUNKS=%lu BYTES=%lu MAX_GAP=%lu ms\n",
      (unsigned long)rxChunks,(unsigned long)rxBytes,(unsigned long)rxMaxGap);
  }
};

AudioRingStream audioRing;

void oledHeader(){
  if(!oledOK)return;
  oled.clearDisplay();oled.setTextColor(SSD1306_WHITE);oled.setTextSize(2);
  oled.setCursor(36,0);oled.print("TARS");oled.display();
}

void oledSetStatus(const String&s){
  oledStatus=s;oledText="";oledTypePos=0;oledPage=0;oledLastPage=millis();
}

void oledSetListening(){
  oledStatus="LISTENING";oledText="";oledTypePos=0;oledPage=0;oledLastPage=millis();
}

void oledStartSpeak(const String&s){
  oledStatus="SPEAKING";oledText=s;oledTypePos=0;oledPage=0;
  oledLastType=millis();oledLastPage=millis();
}

void oledTask(void*){
  for(;;){
    if(!oledOK){vTaskDelay(50);continue;}
    uint32_t now=millis();

    if(oledText.length()&&oledTypePos<oledText.length()&&now-oledLastType>=OLED_TYPE_MS){
      oledTypePos++;oledLastType=now;
    }

    if(now-oledLastWave>=OLED_WAVE_MS){
      oledLastWave=now;oled.clearDisplay();oled.setTextColor(SSD1306_WHITE);oled.setTextSize(1);
      oled.setCursor(3,0);oled.print("T A R S");oled.setCursor(3,13);oled.print(oledStatus);

      if(oledText.length()){
        size_t pos=oledTypePos;if(pos>oledText.length())pos=oledText.length();
        String src=oledText.substring(0,pos);uint32_t target=oledPage*4,lineNo=0;
        uint8_t shown=0;bool next=false;String line;

        for(size_t i=0;i<=src.length();i++){
          char c=i<src.length()?src[i]:'\0';
          if(c=='\n'||c=='\0'){
            if(lineNo>=target&&shown<4){oled.setCursor(3,27+shown*8);oled.print(line);shown++;}
            line="";lineNo++;
            if(shown>=4){if(i<src.length())next=true;break;}
            continue;
          }
          line+=c;
          if(line.length()>=20){
            int cut=line.lastIndexOf(' ');
            if(cut>0){
              String rest=line.substring(cut+1);line=line.substring(0,cut);
              if(lineNo>=target&&shown<4){oled.setCursor(3,27+shown*8);oled.print(line);shown++;}
              line=rest;lineNo++;
              if(shown>=4){if(i+1<src.length())next=true;break;}
            }else{
              if(lineNo>=target&&shown<4){oled.setCursor(3,27+shown*8);oled.print(line);shown++;}
              line="";lineNo++;
              if(shown>=4){if(i+1<src.length())next=true;break;}
            }
          }
        }

        if(oledStatus=="SPEAKING"&&now-oledLastPage>=OLED_PAGE_MS){
          if(next)oledPage++;oledLastPage=now;
        }
      }

      if(oledStatus=="LISTENING"){
        int x=64+(int)(sin(now/120.0)*25);oled.drawCircle(x,52,5,SSD1306_WHITE);
      }else if(oledStatus=="SPEAKING"){
        int w=8+(now/40)%18;oled.fillRect(60-w/2,49,w,7,SSD1306_WHITE);
      }
      oled.display();
    }
    vTaskDelay(10);
  }
}

bool initDAC(){
  auto cfg=analog.defaultConfig(TX_MODE);cfg.channels=2;
  if(!analog.begin(cfg)){Serial.println("TARS: DAC ERROR");return false;}
  auto v=mp3Volume.defaultConfig();v.copyFrom(cfg);v.volume=MP3_VOLUME;v.allow_boost=false;
  mp3Volume.begin(v);mp3Volume.setVolume(MP3_VOLUME);
  Serial.println("TARS: PAM RIGHT GPIO26 READY");
  Serial.printf("TARS: MP3 VOLUME=%.2f\n",MP3_VOLUME);
  return true;
}

bool initMic(){
  i2s_config_t c={};
  c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_RX);c.sample_rate=MIC_RATE;
  c.bits_per_sample=I2S_BITS_PER_SAMPLE_32BIT;c.channel_format=I2S_CHANNEL_FMT_ONLY_RIGHT;
  c.communication_format=I2S_COMM_FORMAT_STAND_I2S;c.intr_alloc_flags=ESP_INTR_FLAG_LEVEL1;
  c.dma_buf_count=2;c.dma_buf_len=256;c.use_apll=false;c.tx_desc_auto_clear=false;c.fixed_mclk=0;
  if(i2s_driver_install(MIC_PORT,&c,0,nullptr)!=ESP_OK)return false;
  i2s_pin_config_t p={};p.bck_io_num=MIC_SCK;p.ws_io_num=MIC_WS;
  p.data_out_num=I2S_PIN_NO_CHANGE;p.data_in_num=MIC_SD;
  if(i2s_set_pin(MIC_PORT,&p)!=ESP_OK)return false;
  i2s_zero_dma_buffer(MIC_PORT);Serial.println("TARS: INMP441 RIGHT READY");return true;
}

bool syncTime(){
  if(ntpOK)return true;
  configTime(7*3600,0,"pool.ntp.org","time.nist.gov","time.google.com");
  for(int a=1;a<=4;a++){
    Serial.printf("TARS: NTP ATTEMPT %d/4\n",a);
    for(int i=0;i<20;i++){
      time_t now=time(nullptr);
      if(now>=1704067200){
        struct tm t;localtime_r(&now,&t);
        Serial.printf("TARS: NTP VALID %04d-%02d-%02d %02d:%02d:%02d\n",
          t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec);
        ntpOK=true;return true;
      }
      delay(500);
    }
  }
  Serial.println("TARS: NTP FAILED 4/4");return false;
}

bool wifiOK(){
  if(WiFi.status()==WL_CONNECTED)return true;
  return wifiManagerConnect(false)&&WiFi.status()==WL_CONNECTED;
}

bool bootWiFi(){
  Serial.println("TARS: WIFI CONNECTING...");
  uint32_t start=millis();
  while(WiFi.status()!=WL_CONNECTED&&millis()-start<30000){
    wifiManagerConnect(false);delay(100);yield();
  }
  if(WiFi.status()!=WL_CONNECTED){Serial.println("TARS: WIFI BOOT FAILED");return false;}
  Serial.print("TARS: WIFI CONNECTED IP=");Serial.println(WiFi.localIP());return true;
}

void sttEvent(WStype_t type,uint8_t*payload,size_t length){
  if(type==WStype_CONNECTED){sttConnected=true;Serial.println("TARS: STT WS CONNECTED");return;}
  if(type==WStype_DISCONNECTED){
    sttConnected=false;if(!sttDone)sttError=true;
    Serial.println("TARS: STT WS DISCONNECTED");return;
  }
  if(type==WStype_ERROR){sttError=true;Serial.println("TARS: STT WS ERROR");return;}
  if(type!=WStype_TEXT)return;

  String msg;msg.reserve(length+1);
  for(size_t i=0;i<length;i++)msg+=(char)payload[i];
  JsonDocument j;if(deserializeJson(j,msg))return;
  String t=j["type"].as<String>();

  if(t=="ready"){
    sttReady=true;Serial.println("TARS: STT REALTIME READY");
  }else if(t=="partial"){
    sttPartial=j["text"].as<String>();sttPartial.trim();
    if(sttPartial.length()){Serial.print("TARS: STT PARTIAL = ");Serial.println(sttPartial);}
  }else if(t=="final"){
    sttFinal=j["text"].as<String>();sttFinal.trim();sttDone=true;
    Serial.print("TARS: YOU SAID = ");Serial.println(sttFinal);
  }else if(t=="error"){
    sttError=true;sttDone=true;
    Serial.print("TARS: STT ERROR = ");Serial.println(j["error"].as<String>());
  }
}

bool startSTT(){
  if(!wifiOK())return false;
  sttConnected=sttReady=sttDone=sttError=false;sttFinal="";sttPartial="";
  sttWS.disconnect();sttWS.onEvent(sttEvent);sttWS.setReconnectInterval(0);
  sttWS.enableHeartbeat(15000,5000,2);sttWS.beginSSL(STT_HOST,443,"/stt");
  uint32_t start=millis();
  while(!sttReady&&!sttError&&millis()-start<7000){sttWS.loop();delay(2);yield();}
  if(!sttReady){Serial.println("TARS: STT REALTIME TIMEOUT");sttWS.disconnect();return false;}
  return true;
}

String stopSTT(uint32_t samples){
  if(!sttConnected)return "";
  JsonDocument j;j["type"]="end";j["timestamp"]=(double)samples/MIC_RATE;
  String msg;serializeJson(j,msg);sttWS.sendTXT(msg);Serial.println("TARS: STT END SENT");
  uint32_t start=millis();
  while(!sttDone&&!sttError&&millis()-start<6000){sttWS.loop();delay(2);yield();}
  String result=sttFinal;sttWS.disconnect();return result;
}

String recordRealtime(){
  if(!micOK||!startSTT())return "";
  oledSetListening();

  size_t prePos=0,preCount=0;uint32_t voiceStart=0,lastVoice=0,samples=0;bool voice=false;
  Serial.println("TARS: REALTIME LISTENING");

  for(;;){
    sttWS.loop();if(sttError)break;
    size_t bytes=0;
    if(i2s_read(MIC_PORT,rawBuf,sizeof(rawBuf),&bytes,pdMS_TO_TICKS(30))!=ESP_OK)continue;
    size_t count=bytes/4;int32_t peak=0;uint64_t sum=0;

    for(size_t i=0;i<count;i++){
      int32_t v=constrain(rawBuf[i]>>16,-32768,32767);
      pcmBuf[i]=(int16_t)v;int32_t a=abs(v);if(a>peak)peak=a;
      uint64_t aa=(uint64_t)a;sum+=aa*aa;
    }

    uint32_t rms=count?(uint32_t)sqrt((double)sum/(double)count):0;

    if(!voice){
      for(size_t i=0;i<count;i++){
        preBuf[prePos]=pcmBuf[i];prePos=(prePos+1)%PREROLL_SAMPLES;
        if(preCount<PREROLL_SAMPLES)preCount++;
      }

      if(peak>=MIC_THRESHOLD||rms>=1800){
        voice=true;voiceStart=millis();lastVoice=voiceStart;
        size_t start=preCount==PREROLL_SAMPLES?prePos:0,n=0;

        for(size_t i=0;i<preCount;i++){
          size_t k=(start+i)%PREROLL_SAMPLES;sendBuf[n++]=preBuf[k];
          if(n==256){
            if(!sttWS.sendBIN((uint8_t*)sendBuf,n*2)){sttError=true;break;}
            n=0;
          }
        }

        if(n&&!sttError)sttWS.sendBIN((uint8_t*)sendBuf,n*2);
        samples+=preCount;
        Serial.printf("TARS: VOICE DETECTED PEAK=%ld RMS=%lu\n",(long)peak,(unsigned long)rms);
      }
    }else{
      if(!sttWS.sendBIN((uint8_t*)pcmBuf,count*2)){
        Serial.println("TARS: STT PCM SEND FAILED");sttError=true;break;
      }
      samples+=count;
      if(peak>=MIC_SILENCE||rms>=1200)lastVoice=millis();
      if(millis()-voiceStart>=RECORD_MIN_MS&&millis()-lastVoice>=SILENCE_MS)break;
    }
    yield();
  }

  if(!voice){sttWS.disconnect();Serial.println("TARS: MIC AUDIO TOO LOW");return "";}
  if(sttError){sttWS.disconnect();return "";}
  return stopSTT(samples);
}

String ask(const String&q){
  if(!wifiOK())return "";
  WiFiClientSecure c;c.setInsecure();HTTPClient h;
  if(!h.begin(c,String(TARS_CLOUD_URL)+"/ask"))return "";
  h.setTimeout(12000);h.addHeader("Content-Type","application/json");
  JsonDocument j;j["question"]=q;String body;serializeJson(j,body);
  uint32_t t=millis();int code=h.POST(body);
  Serial.printf("TARS: ASK HTTP=%d TIME=%lu ms\n",code,(unsigned long)(millis()-t));
  if(code<200||code>=300){h.end();return "";}
  String r=h.getString();h.end();JsonDocument x;
  if(deserializeJson(x,r))return "";
  String s=x["response"].as<String>();s.trim();return s;
}

bool streamAudio(const String&url,const String&text){
  if(!wifiOK())return false;

  WiFiClientSecure c;c.setInsecure();c.setTimeout(15000);
  HTTPClient h;uint32_t totalStart=millis();
  if(!h.begin(c,url))return false;
  h.setTimeout(60000);h.addHeader("Content-Type","application/json");

  const char*keys[]={"Content-Type","X-TARS-TTS","X-TARS-TTS-FORMAT"};
  h.collectHeaders(keys,3);

  JsonDocument j;j["text"]=text;String body;serializeJson(j,body);
  uint32_t t=millis();int code=h.POST(body);

  Serial.printf("TARS: AUDIO HTTP=%d TIME=%lu ms\n",code,(unsigned long)(millis()-t));

  if(code<200||code>=300){h.end();return false;}

  String ct=h.header("Content-Type"),fmt=h.header("X-TARS-TTS-FORMAT"),engine=h.header("X-TARS-TTS");
  ct.toLowerCase();

  Serial.print("TARS: TTS CONTENT-TYPE=");Serial.println(ct.length()?ct:"<none>");
  Serial.print("TARS: TTS STATUS=");Serial.println(engine.length()?engine:"<none>");
  Serial.print("TARS: TTS FORMAT=");Serial.println(fmt.length()?fmt:"<none>");

  WiFiClient*stream=h.getStreamPtr();
  if(!stream){h.end();return false;}

  if(!dacOK)dacOK=initDAC();
  if(!dacOK){h.end();return false;}

  bool isWav=ct.indexOf("wav")>=0||fmt.equalsIgnoreCase("WAV");
  int contentLen=h.getSize();

  Serial.printf("TARS: AUDIO CONTENT-LENGTH=%d\n",contentLen);

  audioRing.start(*stream,contentLen);
  playing=true;

  if(!isWav){
    Serial.println("TARS: MP3 STREAMING");

    size_t target=AUDIO_PREBUFFER;
    if(contentLen>0&&(size_t)contentLen<target)target=contentLen;

    uint32_t preStart=millis();

    while(audioRing.available()<(int)target){
      int a=audioRing.available();

      if(audioRing.finished()){
        if(a>0)break;
        Serial.println("TARS: MP3 PREBUFFER FAILED");
        audioRing.stop();h.end();playing=false;return false;
      }

      if(millis()-preStart>10000){
        Serial.println("TARS: MP3 PREBUFFER TIMEOUT");
        audioRing.stop();h.end();playing=false;return false;
      }

      delay(1);yield();
    }

    Serial.printf("TARS: MP3 PREBUFFER=%d/%u BYTES\n",
      audioRing.available(),(unsigned)target);

    dec.begin();
    copier.begin(dec,audioRing);

    bool started=false;
    uint32_t start=millis(),lastProgress=start;
    uint32_t copyMax=0,copySlow=0,copyNoProgress=0,copyCount=0,copyGapMax=0,lastCopyEnd=start;
    uint32_t lastLowLog=0,lastZeroLog=0;

    while(true){
      int before=audioRing.available();
      uint32_t cs=micros();
      bool copied=copier.copy();
      uint32_t cus=micros()-cs;
      int after=audioRing.available();

      uint32_t gap=millis()-lastCopyEnd;
      lastCopyEnd=millis();
      if(gap>copyGapMax)copyGapMax=gap;
      if(cus>copyMax)copyMax=cus;
      copyCount++;

      if(cus>50000){
        copySlow++;
        Serial.printf("TARS: MP3 COPY LONG=%lu us RING=%d->%d\n",
          (unsigned long)cus,before,after);
        if(before>=2048)
          Serial.println("TARS: MP3 DIAG COPY BLOCK WITH DATA");
      }

      if(!copied){
        copyNoProgress++;
        if(before>1024&&millis()-lastProgress>100){
          Serial.printf("TARS: MP3 DIAG NO PROGRESS RING=%d\n",before);
          lastProgress=millis();
        }
      }else{
        lastProgress=millis();

        if(!started){
          started=true;
          Serial.printf("TARS: AUDIO FIRST DATA=%lu ms\n",
            (unsigned long)(millis()-start));
          oledStartSpeak(text);
        }
      }

      if(after<=1024&&millis()-lastLowLog>200){
        Serial.printf("TARS: MP3 DIAG RING LOW=%d\n",after);
        lastLowLog=millis();
      }

      if(after==0&&millis()-lastZeroLog>200){
        Serial.println("TARS: MP3 DIAG RING EMPTY");
        lastZeroLog=millis();
      }

      if(audioRing.finished()&&audioRing.available()==0)break;

      if(audioRing.available()==0&&millis()-lastProgress>5000){
        Serial.println("TARS: MP3 DIAG PLAYBACK STALL");
        break;
      }

      yield();
    }

    audioRing.stop();
    dec.end();
    h.end();
    playing=false;

    Serial.printf("TARS: MP3 COPY MAX=%lu us SLOW=%lu COUNT=%lu\n",
      (unsigned long)copyMax,(unsigned long)copySlow,(unsigned long)copyCount);

    Serial.printf("TARS: MP3 COPY GAP MAX=%lu ms NO_PROGRESS=%lu\n",
      (unsigned long)copyGapMax,(unsigned long)copyNoProgress);

    Serial.printf("TARS: AUDIO STREAM=%lu ms\n",
      (unsigned long)(millis()-start));

    audioRing.printDiag();

    Serial.printf("TARS: AUDIO TOTAL=%lu ms\n",
      (unsigned long)(millis()-totalStart));

    oledSetListening();
    return started;
  }

  Serial.println("TARS: WAV PLAYBACK");

  size_t wavTarget=2048;
  if(contentLen>0&&(size_t)contentLen<wavTarget)wavTarget=contentLen;

  uint32_t wavPreStart=millis();

  while(audioRing.available()<(int)wavTarget){
    if(audioRing.finished())break;
    if(millis()-wavPreStart>10000)break;
    delay(1);yield();
  }

  copier.begin(wavDec,audioRing);
  wavDec.addNotifyAudioChange(stereoOut);
  wavDec.begin();

  bool started=false;
  uint32_t start=millis(),lastData=start;

  while(true){
    bool copied=copier.copy();

    if(copied){
      lastData=millis();
      if(!started){
        started=true;
        Serial.printf("TARS: AUDIO FIRST DATA=%lu ms",
          (unsigned long)(millis()-start));
        Serial.println();
        oledStartSpeak(text);
      }
    }

    if(audioRing.finished()&&audioRing.available()==0&&millis()-lastData>=200)break;

    if(audioRing.available()==0&&millis()-lastData>5000){
      Serial.println("TARS: WAV PLAYBACK TIMEOUT");
      break;
    }

    yield();
  }

  audioRing.stop();
  wavDec.end();
  stereoOut.end();
  h.end();
  playing=false;

  Serial.printf("TARS: AUDIO STREAM=%lu ms\n",
    (unsigned long)(millis()-start));
  audioRing.printDiag();
  Serial.printf("TARS: AUDIO TOTAL=%lu ms\n",
    (unsigned long)(millis()-totalStart));

  oledSetListening();
  return started;
}

void processQuestion(const String&q){
  String answer=ask(q);
  if(!answer.length()){oledSetStatus("ASK ERROR");return;}

  String url=String(TARS_CLOUD_URL)+"/tts";
  uint32_t t=millis();
  bool ok=streamAudio(url,answer);

  Serial.printf("TARS: AUDIO FUNCTION TIME=%lu ms\n",
    (unsigned long)(millis()-t));

  oledSetStatus(ok?"LISTENING":"AUDIO ERROR");
}

void setup(){
  Serial.begin(SERIAL_BAUD);

  Wire.begin(OLED_SDA,OLED_SCL);
  Wire.setClock(400000);

  oledOK=oled.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR);

  if(oledOK){
    oledHeader();oled.setCursor(3,27);oled.print("BOOT");oled.display();
  }

  dacOK=initDAC();
  micOK=initMic();

  Serial.printf("TARS: DAC=%s MIC=%s\n",
    dacOK?"READY":"ERROR",micOK?"READY":"ERROR");

  Serial.println("TARS: PAM RIGHT GPIO26");
  Serial.println("TARS: INMP441 RIGHT GPIO34");
  Serial.printf("TARS: MIC THRESHOLD=%ld\n",(long)MIC_THRESHOLD);
  Serial.printf("TARS: MIC SILENCE=%ld\n",(long)MIC_SILENCE);
  Serial.println("TARS: MIC RMS TRIGGER=1800");
  Serial.println("TARS: MIC RMS SILENCE=1200");
  Serial.println("TARS: PREROLL=700 ms");
  Serial.println("TARS: NO RECORD TIMEOUT");
  Serial.println("TARS: STT REALTIME PCM");
  Serial.println("TARS: BLUETOOTH DISABLED");
  Serial.printf("TARS: MP3 GLOBAL BUFFER=%d BYTES\n",MP3_COPY_BUFFER);
  Serial.printf("TARS: MP3 VOLUME=%.2f\n",MP3_VOLUME);
  Serial.printf("TARS: AUDIO RING=%u BYTES\n",(unsigned)AUDIO_RING_SIZE);
  Serial.printf("TARS: AUDIO PREBUFFER=%u BYTES\n",(unsigned)AUDIO_PREBUFFER);

  if(oledOK)
    xTaskCreatePinnedToCore(oledTask,"TARS_OLED",4096,nullptr,1,nullptr,0);

  wifiManagerBegin();

  if(bootWiFi())syncTime();

  oledSetListening();
}

void loop(){
  if(playing){delay(1);return;}

  if(WiFi.status()!=WL_CONNECTED){
    if(!wifiOK()){oledSetStatus("WIFI ERROR");delay(500);return;}
  }

  String q=recordRealtime();

  if(q.length())processQuestion(q);
  else oledSetListening();

  delay(1);
}
