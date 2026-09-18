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
const size_t BUF=1024,PREROLL_SAMPLES=MIC_RATE*PREROLL_MS/1000;
const int MP3_COPY_BUFFER=2048;
const float MP3_VOLUME=.67f;
const size_t AUDIO_RING_SIZE=16384,AUDIO_PREBUFFER=8192;
const char* STT_HOST="tars-cloud-v1.hilmane34.workers.dev";

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);

AnalogAudioStream analog;
MP3DecoderHelix codec;
EncodedAudioStream dec(&analog,&codec);
WAVDecoder wav;
EncodedAudioStream wavDec(&analog,&wav);
StreamCopy copier(MP3_COPY_BUFFER);
WebSocketsClient sttWS;

bool oledOK=false,micOK=false,dacOK=false,playing=false,ntpOK=false;
bool sttConnected=false,sttReady=false,sttDone=false,sttError=false;

String sttFinal,sttPartial,oledText,oledStatus="READY";
uint32_t oledTypePos=0,oledLastType=0,oledLastWave=0,oledPage=0,oledLastPage=0;

static int32_t rawBuf[BUF/4];
static int16_t pcmBuf[BUF/4],preBuf[PREROLL_SAMPLES],sendBuf[256];

/* ================= AUDIO RING ================= */

class AudioRingStream:public Stream{
  uint8_t b[AUDIO_RING_SIZE];
  volatile size_t h=0,t=0,n=0;
  volatile bool done=false,stopFlag=false,running=false;
  TaskHandle_t task=nullptr;
  portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
  WiFiClient*src=nullptr;
  int expected=-1,received=0;

  static void entry(void*p){
    ((AudioRingStream*)p)->rx();
    vTaskDelete(nullptr);
  }

  void rx(){
    uint8_t tmp[1024];
    uint32_t last=millis();

    while(!stopFlag){
      int av=src?src->available():0;

      if(av>0){
        size_t used=available(),freeSpace=AUDIO_RING_SIZE-used;
        if(!freeSpace){vTaskDelay(1);continue;}

        size_t want=min((size_t)av,sizeof(tmp));
        want=min(want,freeSpace);

        int r=src->read(tmp,want);

        if(r>0){
          push(tmp,r);
          received+=r;
          last=millis();
          if(expected>=0&&received>=expected)break;
        }
      }else{
        if(expected>=0&&received>=expected)break;
        if(src&&!src->connected()&&millis()-last>50)break;
        if(expected<0&&src&&millis()-last>1500&&!available())break;
        vTaskDelay(1);
      }
    }

    portENTER_CRITICAL(&mux);
    done=true;
    running=false;
    portEXIT_CRITICAL(&mux);
    task=nullptr;
  }

  size_t push(const uint8_t*p,size_t x){
    if(!p||!x)return 0;

    portENTER_CRITICAL(&mux);

    size_t freeSpace=AUDIO_RING_SIZE-n;
    x=min(x,freeSpace);

    size_t first=min(x,AUDIO_RING_SIZE-h);
    memcpy(b+h,p,first);
    if(x>first)memcpy(b,p+first,x-first);

    h=(h+x)%AUDIO_RING_SIZE;
    n+=x;

    portEXIT_CRITICAL(&mux);
    return x;
  }

public:
  void start(WiFiClient&s,int len=-1){
    stop();

    portENTER_CRITICAL(&mux);
    h=t=n=0;
    done=false;
    stopFlag=false;
    running=true;
    portEXIT_CRITICAL(&mux);

    src=&s;
    expected=len;
    received=0;

    xTaskCreatePinnedToCore(
      entry,"TARS_RX",3072,this,2,&task,0
    );
  }

  void stop(){
    stopFlag=true;
    uint32_t st=millis();

    while(running&&millis()-st<1000)
      vTaskDelay(1);

    if(task){
      vTaskDelete(task);
      task=nullptr;
    }

    portENTER_CRITICAL(&mux);
    running=false;
    done=true;
    portEXIT_CRITICAL(&mux);

    src=nullptr;
  }

  bool finished(){
    return done&&available()==0;
  }

  int available()override{
    portENTER_CRITICAL(&mux);
    int r=(int)n;
    portEXIT_CRITICAL(&mux);
    return r;
  }

  int read()override{
    uint8_t c;
    return read(&c,1)==1?c:-1;
  }

  int read(uint8_t*p,size_t x){
    if(!p||!x)return 0;

    portENTER_CRITICAL(&mux);

    size_t take=(size_t)n<x?(size_t)n:x;

    if(take){
      size_t first=min(take,AUDIO_RING_SIZE-t);
      memcpy(p,b+t,first);

      if(take>first)
        memcpy(p+first,b,take-first);

      t=(t+take)%AUDIO_RING_SIZE;
      n-=take;
    }

    portEXIT_CRITICAL(&mux);
    return (int)take;
  }

  int peek()override{
    portENTER_CRITICAL(&mux);
    int r=n?b[t]:-1;
    portEXIT_CRITICAL(&mux);
    return r;
  }

  void flush()override{
    portENTER_CRITICAL(&mux);
    h=t=n=0;
    portEXIT_CRITICAL(&mux);
  }

  size_t write(uint8_t)override{return 0;}
  size_t write(const uint8_t*,size_t)override{return 0;}
};

AudioRingStream audioRing;

/* ================= OLED ================= */

void oledSetStatus(const String&s){
  oledStatus=s;
  oledText="";
  oledTypePos=0;
  oledPage=0;
  oledLastPage=millis();
}

void oledSetListening(){
  oledStatus="LISTENING";
  oledText="";
  oledTypePos=0;
  oledPage=0;
  oledLastPage=millis();
}

void oledStartSpeak(const String&s){
  oledStatus="SPEAKING";
  oledText=s;
  oledTypePos=0;
  oledPage=0;
  oledLastType=millis();
  oledLastPage=millis();
}

void oledTask(void*){
  for(;;){
    if(!oledOK){
      vTaskDelay(50);
      continue;
    }

    uint32_t now=millis();

    if(oledText.length()&&oledTypePos<oledText.length()&&
       now-oledLastType>=OLED_TYPE_MS){
      oledTypePos++;
      oledLastType=now;
    }

    if(now-oledLastWave>=OLED_WAVE_MS){
      oledLastWave=now;
      oled.clearDisplay();
      oled.setTextColor(SSD1306_WHITE);

      oled.setTextSize(2);
      oled.setCursor(36,0);
      oled.print("TARS");

      oled.setTextSize(1);
      oled.setCursor(3,17);
      oled.print(oledStatus);

      if(oledText.length()){
        String s=oledText.substring(
          0,min(oledTypePos,(uint32_t)oledText.length())
        );

        uint32_t lineNo=0,target=oledPage*4;
        uint8_t shown=0;
        String line;
        bool next=false;

        for(size_t i=0;i<=s.length();i++){
          char c=i<s.length()?s[i]:'\0';

          if(c=='\n'||c=='\0'){
            if(lineNo>=target&&shown<4){
              oled.setCursor(3,29+shown*8);
              oled.print(line);
              shown++;
            }

            line="";
            lineNo++;

            if(shown>=4){
              next=i<s.length();
              break;
            }
            continue;
          }

          line+=c;

          if(line.length()>=20){
            int cut=line.lastIndexOf(' ');

            if(cut>0){
              String rest=line.substring(cut+1);
              line=line.substring(0,cut);

              if(lineNo>=target&&shown<4){
                oled.setCursor(3,29+shown*8);
                oled.print(line);
                shown++;
              }

              line=rest;
              lineNo++;

              if(shown>=4){
                next=i+1<s.length();
                break;
              }
            }
          }
        }

        if(oledStatus=="SPEAKING"&&
           now-oledLastPage>=OLED_PAGE_MS){
          if(next)oledPage++;
          oledLastPage=now;
        }
      }

      if(oledStatus=="LISTENING"){
        int x=64+(int)(sin(now/120.0)*25);
        oled.drawCircle(x,56,4,SSD1306_WHITE);
      }else if(oledStatus=="SPEAKING"){
        int w=8+(now/40)%18;
        oled.fillRect(64-w/2,51,w,6,SSD1306_WHITE);
      }

      oled.display();
    }

    vTaskDelay(10);
  }
}

/* ================= DAC ================= */

bool initDAC(){
  auto cfg=analog.defaultConfig(TX_MODE);

  /* Initial format only.
     MP3DecoderHelix will notify the output with
     the actual decoded AudioInfo dynamically. */
  cfg.sample_rate=44100;
  cfg.channels=1;
  cfg.bits_per_sample=16;

  if(!analog.begin(cfg)){
    Serial.println("TARS: DAC ERROR");
    return false;
  }

  auto v=mp3Volume.defaultConfig();

  /* Decoder -> Volume -> Analog DAC */
  dec.addNotifyAudioChange(mp3Volume);
  mp3Volume.addNotifyAudioChange(analog);

  Serial.println("TARS: DAC GPIO26 READY");
  Serial.println("TARS: AUTO MP3 FORMAT");
  Serial.println("TARS: DECODER -> VOLUME -> DAC");
  Serial.printf("TARS: MP3 VOLUME=%.2f\n",MP3_VOLUME);

  return true;
}

/* ================= MIC ================= */

bool initMic(){
  i2s_config_t c={};
  c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_RX);
  c.sample_rate=MIC_RATE;
  c.bits_per_sample=I2S_BITS_PER_SAMPLE_32BIT;
  c.channel_format=I2S_CHANNEL_FMT_ONLY_RIGHT;
  c.communication_format=I2S_COMM_FORMAT_STAND_I2S;
  c.intr_alloc_flags=ESP_INTR_FLAG_LEVEL1;
  c.dma_buf_count=2;
  c.dma_buf_len=256;
  c.use_apll=false;
  c.tx_desc_auto_clear=false;
  c.fixed_mclk=0;

  if(i2s_driver_install(MIC_PORT,&c,0,nullptr)!=ESP_OK)
    return false;

  i2s_pin_config_t p={};
  p.bck_io_num=MIC_SCK;
  p.ws_io_num=MIC_WS;
  p.data_out_num=I2S_PIN_NO_CHANGE;
  p.data_in_num=MIC_SD;

  if(i2s_set_pin(MIC_PORT,&p)!=ESP_OK)
    return false;

  i2s_zero_dma_buffer(MIC_PORT);
  Serial.println("TARS: INMP441 RIGHT READY");
  return true;
}

/* ================= WIFI/NTP ================= */

bool wifiOK(){
  if(WiFi.status()==WL_CONNECTED)return true;
  return wifiManagerConnect(false)&&WiFi.status()==WL_CONNECTED;
}

bool bootWiFi(){
  Serial.println("TARS: WIFI CONNECTING...");
  uint32_t st=millis();

  while(WiFi.status()!=WL_CONNECTED&&millis()-st<30000){
    wifiManagerConnect(false);
    delay(100);
    yield();
  }

  if(WiFi.status()!=WL_CONNECTED){
    Serial.println("TARS: WIFI BOOT FAILED");
    return false;
  }

  Serial.print("TARS: WIFI CONNECTED IP=");
  Serial.println(WiFi.localIP());
  return true;
}

bool syncTime(){
  if(ntpOK)return true;

  configTime(
    7*3600,0,
    "pool.ntp.org",
    "time.nist.gov",
    "time.google.com"
  );

  for(int a=1;a<=4;a++){
    Serial.printf("TARS: NTP ATTEMPT %d/4\n",a);

    for(int i=0;i<20;i++){
      time_t now=time(nullptr);

      if(now>=1704067200){
        struct tm t;
        localtime_r(&now,&t);

        Serial.printf(
          "TARS: NTP VALID %04d-%02d-%02d %02d:%02d:%02d\n",
          t.tm_year+1900,t.tm_mon+1,t.tm_mday,
          t.tm_hour,t.tm_min,t.tm_sec
        );

        ntpOK=true;
        return true;
      }

      delay(500);
    }
  }

  Serial.println("TARS: NTP FAILED 4/4");
  return false;
}

/* ================= STT ================= */

void sttEvent(WStype_t type,uint8_t*payload,size_t length){
  if(type==WStype_CONNECTED){
    sttConnected=true;
    Serial.println("TARS: STT WS CONNECTED");
    return;
  }

  if(type==WStype_DISCONNECTED){
    sttConnected=false;
    if(!sttDone)sttError=true;
    Serial.println("TARS: STT WS DISCONNECTED");
    return;
  }

  if(type==WStype_ERROR){
    sttError=true;
    Serial.println("TARS: STT WS ERROR");
    return;
  }

  if(type!=WStype_TEXT)return;

  String msg;
  msg.reserve(length+1);

  for(size_t i=0;i<length;i++)
    msg+=(char)payload[i];

  JsonDocument j;
  if(deserializeJson(j,msg))return;

  String t=j["type"].as<String>();

  if(t=="ready"){
    sttReady=true;
    Serial.println("TARS: STT REALTIME READY");
  }else if(t=="partial"){
    sttPartial=j["text"].as<String>();
    sttPartial.trim();

    if(sttPartial.length()){
      Serial.print("TARS: STT PARTIAL = ");
      Serial.println(sttPartial);
    }
  }else if(t=="final"){
    sttFinal=j["text"].as<String>();
    sttFinal.trim();
    sttDone=true;

    Serial.print("TARS: YOU SAID = ");
    Serial.println(sttFinal);
  }else if(t=="error"){
    sttError=true;
    sttDone=true;

    Serial.print("TARS: STT ERROR = ");
    Serial.println(j["error"].as<String>());
  }
}

bool startSTT(){
  if(!wifiOK())return false;

  sttConnected=sttReady=sttDone=sttError=false;
  sttFinal="";
  sttPartial="";

  sttWS.disconnect();
  sttWS.onEvent(sttEvent);
  sttWS.setReconnectInterval(0);
  sttWS.enableHeartbeat(15000,5000,2);
  sttWS.beginSSL(STT_HOST,443,"/stt");

  uint32_t st=millis();

  while(!sttReady&&!sttError&&millis()-st<7000){
    sttWS.loop();
    delay(2);
    yield();
  }

  if(!sttReady){
    Serial.println("TARS: STT REALTIME TIMEOUT");
    sttWS.disconnect();
    return false;
  }

  return true;
}

String stopSTT(uint32_t samples){
  if(!sttConnected)return "";

  JsonDocument j;
  j["type"]="end";
  j["timestamp"]=(double)samples/MIC_RATE;

  String msg;
  serializeJson(j,msg);
  sttWS.sendTXT(msg);

  Serial.println("TARS: STT END SENT");

  uint32_t st=millis();

  while(!sttDone&&!sttError&&millis()-st<6000){
    sttWS.loop();
    delay(2);
    yield();
  }

  String r=sttFinal;
  sttWS.disconnect();
  return r;
}

String recordRealtime(){
  if(!micOK||!startSTT())return "";

  oledSetListening();

  size_t prePos=0,preCount=0;
  uint32_t voiceStart=0,lastVoice=0,samples=0;
  bool voice=false;

  Serial.println("TARS: REALTIME LISTENING");

  for(;;){
    sttWS.loop();
    if(sttError)break;

    size_t bytes=0;

    if(i2s_read(
      MIC_PORT,rawBuf,sizeof(rawBuf),
      &bytes,pdMS_TO_TICKS(30)
    )!=ESP_OK)continue;

    size_t count=bytes/4;
    int32_t peak=0;
    uint64_t sum=0;

    for(size_t i=0;i<count;i++){
      int32_t v=constrain(
        rawBuf[i]>>16,-32768,32767
      );

      pcmBuf[i]=(int16_t)v;

      int32_t a=abs(v);
      if(a>peak)peak=a;
      sum+=(uint64_t)a*a;
    }

    uint32_t rms=count?
      (uint32_t)sqrt((double)sum/count):0;

    if(!voice){
      for(size_t i=0;i<count;i++){
        preBuf[prePos]=pcmBuf[i];
        prePos=(prePos+1)%PREROLL_SAMPLES;

        if(preCount<PREROLL_SAMPLES)
          preCount++;
      }

      if(peak>=MIC_THRESHOLD||rms>=1800){
        voice=true;
        voiceStart=lastVoice=millis();

        size_t start=
          preCount==PREROLL_SAMPLES?prePos:0;

        size_t nsend=0;

        for(size_t i=0;i<preCount;i++){
          sendBuf[nsend++]=
            preBuf[(start+i)%PREROLL_SAMPLES];

          if(nsend==256){
            if(!sttWS.sendBIN(
              (uint8_t*)sendBuf,nsend*2
            )){
              sttError=true;
              break;
            }

            nsend=0;
          }
        }

        if(nsend&&!sttError)
          sttWS.sendBIN(
            (uint8_t*)sendBuf,nsend*2
          );

        samples+=preCount;

        Serial.printf(
          "TARS: VOICE DETECTED PEAK=%ld RMS=%lu\n",
          (long)peak,(unsigned long)rms
        );
      }
    }else{
      if(!sttWS.sendBIN(
        (uint8_t*)pcmBuf,count*2
      )){
        Serial.println("TARS: STT PCM SEND FAILED");
        sttError=true;
        break;
      }

      samples+=count;

      if(peak>=MIC_SILENCE||rms>=1200)
        lastVoice=millis();

      if(millis()-voiceStart>=RECORD_MIN_MS&&
         millis()-lastVoice>=SILENCE_MS)
        break;
    }

    yield();
  }

  if(!voice||sttError){
    sttWS.disconnect();

    if(!voice)
      Serial.println("TARS: MIC AUDIO TOO LOW");

    return "";
  }

  return stopSTT(samples);
}

/* ================= ASK ================= */

String ask(const String&q){
  if(!wifiOK())return "";

  WiFiClientSecure c;
  c.setInsecure();

  HTTPClient h;

  if(!h.begin(c,String(TARS_CLOUD_URL)+"/ask"))
    return "";

  h.setTimeout(12000);
  h.addHeader("Content-Type","application/json");

  JsonDocument j;
  j["question"]=q;

  String body;
  serializeJson(j,body);

  uint32_t st=millis();
  int code=h.POST(body);

  Serial.printf(
    "TARS: ASK HTTP=%d TIME=%lu ms\n",
    code,(unsigned long)(millis()-st)
  );

  if(code<200||code>=300){
    h.end();
    return "";
  }

  String r=h.getString();
  h.end();

  JsonDocument x;
  if(deserializeJson(x,r))return "";

  String s=x["response"].as<String>();
  s.trim();

  return s;
}

/* ================= AUDIO ================= */

bool streamAudio(const String&url,const String&text){
  if(!wifiOK())return false;

  WiFiClientSecure c;
  c.setInsecure();
  c.setTimeout(15000);

  HTTPClient h;
  uint32_t total=millis();

  if(!h.begin(c,url))
    return false;

  h.setTimeout(60000);
  h.addHeader("Content-Type","application/json");

  const char*keys[]={
    "Content-Type",
    "X-TARS-TTS",
    "X-TARS-TTS-FORMAT"
  };

  h.collectHeaders(keys,3);

  JsonDocument j;
  j["text"]=text;

  String body;
  serializeJson(j,body);

  uint32_t st=millis();
  int code=h.POST(body);

  Serial.printf(
    "TARS: AUDIO HTTP=%d TIME=%lu ms\n",
    code,(unsigned long)(millis()-st)
  );

  if(code<200||code>=300){
    h.end();
    return false;
  }

  String ct=h.header("Content-Type");
  String fmt=h.header("X-TARS-TTS-FORMAT");
  String engine=h.header("X-TARS-TTS");

  ct.toLowerCase();

  Serial.print("TARS: TTS CONTENT-TYPE=");
  Serial.println(ct.length()?ct:"<none>");

  Serial.print("TARS: TTS STATUS=");
  Serial.println(engine.length()?engine:"<none>");

  Serial.print("TARS: TTS FORMAT=");
  Serial.println(fmt.length()?fmt:"<none>");

  WiFiClient*stream=h.getStreamPtr();

  if(!stream){
    h.end();
    return false;
  }

  if(!dacOK)dacOK=initDAC();

  if(!dacOK){
    h.end();
    return false;
  }

  int contentLen=h.getSize();

  Serial.printf(
    "TARS: AUDIO CONTENT-LENGTH=%d\n",
    contentLen
  );

  audioRing.start(*stream,contentLen);
  playing=true;

  bool isWav=ct.indexOf("wav")>=0;

  /* ---------- MP3 ---------- */

  if(!isWav){
    Serial.println("TARS: MP3 AUTO FORMAT");

    size_t target=AUDIO_PREBUFFER;

    if(contentLen>0)
      target=min(target,(size_t)contentLen);

    uint32_t ps=millis();

    while(audioRing.available()<(int)target){
      if(audioRing.finished()&&
         audioRing.available()==0)
        break;

      if(millis()-ps>10000){
        Serial.println("TARS: MP3 PREBUFFER TIMEOUT");

        audioRing.stop();
        h.end();
        playing=false;

        return false;
      }

      delay(1);
      yield();
    }

    Serial.printf(
      "TARS: MP3 PREBUFFER=%d/%u BYTES\n",
      audioRing.available(),
      (unsigned)target
    );

    /* Decoder format is propagated through:
       decoder -> VolumeStream -> AnalogAudioStream */
    dec.begin();

    copier.begin(dec,audioRing);

    bool started=false;
    uint32_t start=millis();
    uint32_t lastData=start;
    uint32_t emptySince=0;

    while(true){
      int before=audioRing.available();
      bool copied=copier.copy();
      int after=audioRing.available();

      if(copied){
        lastData=millis();
        emptySince=0;

        if(!started){
          started=true;

          AudioInfo ai=codec.audioInfo();

          Serial.printf(
            "TARS: DECODER %lu Hz / %d ch / %d bit\n",
            (unsigned long)ai.sample_rate,
            ai.channels,
            ai.bits_per_sample
          );

          Serial.println(
            "TARS: AUDIO AUTO -> DAC GPIO26"
          );

          oledStartSpeak(text);
        }
      }else if(after==0){
        if(!emptySince)
          emptySince=millis();

        if(audioRing.finished()&&
           millis()-emptySince>=150)
          break;
      }

      if(audioRing.finished()&&
         audioRing.available()==0&&
         millis()-lastData>=200)
        break;

      if(audioRing.available()==0&&
         millis()-lastData>5000){
        Serial.println(
          "TARS: MP3 PLAYBACK TIMEOUT"
        );
        break;
      }

      if(before==after)
        yield();
    }

    audioRing.stop();
    dec.end();
    h.end();
    playing=false;

    Serial.printf(
      "TARS: AUDIO STREAM=%lu ms\n",
      (unsigned long)(millis()-start)
    );

    Serial.printf(
      "TARS: AUDIO TOTAL=%lu ms\n",
      (unsigned long)(millis()-total)
    );

    oledSetListening();
    return started;
  }

  /* ---------- WAV ---------- */

  Serial.println("TARS: WAV STREAMING");

  wavDec.addNotifyAudioChange(analog);
  wavDec.begin();

  copier.begin(wavDec,audioRing);

  bool started=false;
  uint32_t start=millis();
  uint32_t lastData=start;

  while(true){
    bool copied=copier.copy();

    if(copied){
      lastData=millis();

      if(!started){
        started=true;

        AudioInfo ai=wav.audioInfo();

        Serial.printf(
          "TARS: WAV %lu Hz / %d ch / %d bit\n",
          (unsigned long)ai.sample_rate,
          ai.channels,
          ai.bits_per_sample
        );

        oledStartSpeak(text);
      }
    }

    if(audioRing.finished()&&
       audioRing.available()==0&&
       millis()-lastData>=200)
      break;

    if(audioRing.available()==0&&
       millis()-lastData>5000){
      Serial.println(
        "TARS: WAV PLAYBACK TIMEOUT"
      );
      break;
    }

    yield();
  }

  audioRing.stop();
  wavDec.end();
  h.end();
  playing=false;
  oledSetListening();

  return started;
}

/* ================= PROCESS ================= */

void processQuestion(const String&q){
  String answer=ask(q);

  if(!answer.length()){
    oledSetStatus("ASK ERROR");
    return;
  }

  uint32_t st=millis();

  bool ok=streamAudio(
    String(TARS_CLOUD_URL)+"/tts",
    answer
  );

  Serial.printf(
    "TARS: AUDIO FUNCTION TIME=%lu ms\n",
    (unsigned long)(millis()-st)
  );

  oledSetStatus(ok?"LISTENING":"AUDIO ERROR");
}

/* ================= SETUP ================= */

void setup(){
  Serial.begin(SERIAL_BAUD);

  Wire.begin(OLED_SDA,OLED_SCL);
  Wire.setClock(400000);

  oledOK=oled.begin(
    SSD1306_SWITCHCAPVCC,
    OLED_ADDR
  );

  if(oledOK){
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);

    oled.setTextSize(2);
    oled.setCursor(36,0);
    oled.print("TARS");

    oled.setTextSize(1);
    oled.setCursor(3,27);
    oled.print("BOOT");

    oled.display();
  }

  dacOK=initDAC();
  micOK=initMic();

  Serial.printf(
    "TARS: DAC=%s MIC=%s\n",
    dacOK?"READY":"ERROR",
    micOK?"READY":"ERROR"
  );

  Serial.println(
    "TARS: DAC GPIO26 INTERNAL DAC"
  );
  Serial.println(
    "TARS: AUDIO FORMAT AUTO"
  );
  Serial.println(
    "TARS: AUDIO DECODER FORMAT PROPAGATION ON"
  );

  Serial.println(
    "TARS: INMP441 RIGHT GPIO34"
  );

  Serial.printf(
    "TARS: MIC THRESHOLD=%ld\n",
    (long)MIC_THRESHOLD
  );

  Serial.printf(
    "TARS: MIC SILENCE=%ld\n",
    (long)MIC_SILENCE
  );

  Serial.println(
    "TARS: MIC RMS TRIGGER=1800"
  );
  Serial.println(
    "TARS: MIC RMS SILENCE=1200"
  );
  Serial.println(
    "TARS: PREROLL=700 ms"
  );
  Serial.println(
    "TARS: NO RECORD TIMEOUT"
  );
  Serial.println(
    "TARS: STT REALTIME PCM"
  );
  Serial.println(
    "TARS: BLUETOOTH DISABLED"
  );

  Serial.printf(
    "TARS: MP3 COPY BUFFER=%d BYTES\n",
    MP3_COPY_BUFFER
  );

  Serial.printf(
    "TARS: MP3 VOLUME=%.2f\n",
    MP3_VOLUME
  );

  Serial.printf(
    "TARS: AUDIO RING=%u BYTES\n",
    (unsigned)AUDIO_RING_SIZE
  );

  Serial.printf(
    "TARS: AUDIO PREBUFFER=%u BYTES\n",
    (unsigned)AUDIO_PREBUFFER
  );

  if(oledOK)
    xTaskCreatePinnedToCore(
      oledTask,"TARS_OLED",4096,
      nullptr,1,nullptr,0
    );

  wifiManagerBegin();

  if(bootWiFi())
    syncTime();

  oledSetListening();
}

/* ================= LOOP ================= */

void loop(){
  if(playing){
    delay(1);
    return;
  }

  if(WiFi.status()!=WL_CONNECTED){
    if(!wifiOK()){
      oledSetStatus("WIFI ERROR");
      delay(500);
      return;
    }
  }

  String q=recordRealtime();

  if(q.length())
    processQuestion(q);
  else
    oledSetListening();

  delay(1);
}
