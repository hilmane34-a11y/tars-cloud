#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <time.h>
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
const uint32_t PREROLL_MS=700,OLED_TYPE_MS=39,OLED_WAVE_MS=70,AUDIO_IDLE_MS=2500;
const int32_t MIC_THRESHOLD=8000,MIC_SILENCE=6000;
const size_t BUF=2048,PREROLL_SAMPLES=MIC_RATE*PREROLL_MS/1000;
const char* STT_HOST="tars-cloud-v1.hilmane34.workers.dev";

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);
AnalogAudioStream analog;
MP3DecoderHelix mp3;
WAVDecoder wav;
AudioInfo audioIn(44100,1,16),audioOut(44100,2,16);
FormatConverterStream stereoOut(analog);
EncodedAudioStream mp3Dec(&stereoOut,&mp3);
EncodedAudioStream wavDec(&stereoOut,&wav);
StreamCopy copier;
WebSocketsClient sttWS;

bool oledOK=false,micOK=false,dacOK=false,playing=false,ntpOK=false;
bool sttConnected=false,sttReady=false,sttDone=false,sttError=false;
String sttFinal,sttPartial,oledText,oledStatus="READY";
uint32_t oledTypePos=0,oledLastType=0,oledLastWave=0;

static int32_t rawBuf[BUF/4];
static int16_t pcmBuf[BUF/4],preBuf[PREROLL_SAMPLES],sendBuf[256];

/* OLED */
void oledHeader(){
  if(!oledOK)return;
  oled.clearDisplay();oled.setTextColor(SSD1306_WHITE);oled.setTextSize(2);
  oled.setCursor(36,0);oled.print("TARS");oled.display();
}
void oledSetStatus(const String&s){oledStatus=s;oledText="";oledTypePos=0;}
void oledSetListening(){oledSetStatus("LISTENING");}
void oledStartSpeak(const String&s){
  oledStatus="SPEAKING";oledText=s;oledTypePos=0;oledLastType=millis();
}
void oledTask(void*){
  for(;;){
    if(!oledOK){vTaskDelay(pdMS_TO_TICKS(50));continue;}
    uint32_t now=millis();

    if(oledText.length()&&oledTypePos<oledText.length()&&now-oledLastType>=OLED_TYPE_MS)
      oledTypePos++,oledLastType=now;

    if(now-oledLastWave>=OLED_WAVE_MS){
      oledLastWave=now;oled.clearDisplay();oled.setTextColor(SSD1306_WHITE);
      oled.setTextSize(1);oled.setCursor(3,0);oled.print("T A R S");
      oled.setCursor(3,13);oled.print(oledStatus);

      if(oledText.length()){
        String src=oledText.substring(0,min((size_t)oledTypePos,oledText.length()));
        String lines[20],line;int n=0;

        for(size_t i=0;i<src.length();i++){
          char c=src[i];
          if(c=='\n'){
            if(n<20)lines[n++]=line;
            line="";continue;
          }
          line+=c;
          if(line.length()>=20){
            int cut=line.lastIndexOf(' ');
            if(cut>0){
              String rest=line.substring(cut+1);
              line=line.substring(0,cut);
              if(n<20)lines[n++]=line;
              line=rest;
            }else{
              if(n<20)lines[n++]=line;
              line="";
            }
          }
        }

        if(line.length()&&n<20)lines[n++]=line;
        int first=n?(n-1)/4*4:0;

        for(int i=0;i<4&&first+i<n;i++){
          oled.setCursor(3,27+i*8);
          oled.print(lines[first+i]);
        }
      }

      if(oledStatus=="LISTENING"){
        int x=64+(int)(sin(now/120.0)*25);
        oled.drawCircle(x,52,5,SSD1306_WHITE);
      }else if(oledStatus=="SPEAKING"){
        int w=8+(now/40)%18;
        oled.fillRect(60-w/2,49,w,7,SSD1306_WHITE);
      }

      oled.display();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

/* DAC */
bool initDAC(){
  auto cfg=analog.defaultConfig(TX_MODE);
  cfg.sample_rate=44100;cfg.channels=2;cfg.bits_per_sample=16;

  if(!analog.begin(cfg)){
    Serial.println("TARS: DAC ERROR");
    return false;
  }

  Serial.println("TARS: PAM RIGHT GPIO26 READY");
  Serial.println("TARS: DAC 44100 Hz / 2 CH / 16 BIT");
  return true;
}

/* INMP441 - TETAP */
bool initMic(){
  i2s_config_t c={};
  c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_RX);
  c.sample_rate=MIC_RATE;
  c.bits_per_sample=I2S_BITS_PER_SAMPLE_32BIT;
  c.channel_format=I2S_CHANNEL_FMT_ONLY_RIGHT;
  c.communication_format=I2S_COMM_FORMAT_STAND_I2S;
  c.intr_alloc_flags=ESP_INTR_FLAG_LEVEL1;
  c.dma_buf_count=2;c.dma_buf_len=256;
  c.use_apll=false;c.tx_desc_auto_clear=false;c.fixed_mclk=0;

  if(i2s_driver_install(MIC_PORT,&c,0,nullptr)!=ESP_OK)return false;

  i2s_pin_config_t p={};
  p.bck_io_num=MIC_SCK;p.ws_io_num=MIC_WS;
  p.data_out_num=I2S_PIN_NO_CHANGE;p.data_in_num=MIC_SD;

  if(i2s_set_pin(MIC_PORT,&p)!=ESP_OK)return false;

  i2s_zero_dma_buffer(MIC_PORT);
  Serial.println("TARS: INMP441 RIGHT READY");
  return true;
}

/* NTP */
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
        ntpOK=true;
        return true;
      }
      delay(500);
    }
  }

  Serial.println("TARS: NTP FAILED 4/4");
  return false;
}

/* WIFI */
bool wifiOK(){
  if(WiFi.status()==WL_CONNECTED)return true;
  return wifiManagerConnect(false)&&WiFi.status()==WL_CONNECTED;
}

bool bootWiFi(){
  Serial.println("TARS: WIFI CONNECTING...");
  uint32_t start=millis();

  while(WiFi.status()!=WL_CONNECTED&&millis()-start<30000){
    wifiManagerConnect(false);
    delay(100);yield();
  }

  if(WiFi.status()!=WL_CONNECTED){
    Serial.println("TARS: WIFI BOOT FAILED");
    return false;
  }

  Serial.print("TARS: WIFI CONNECTED IP=");
  Serial.println(WiFi.localIP());
  return true;
}

/* STT */
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

  String msg;msg.reserve(length+1);
  for(size_t i=0;i<length;i++)msg+=(char)payload[i];

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

  sttConnected=false;sttReady=false;sttDone=false;sttError=false;
  sttFinal="";sttPartial="";

  sttWS.disconnect();
  sttWS.onEvent(sttEvent);
  sttWS.setReconnectInterval(0);
  sttWS.enableHeartbeat(15000,5000,2);
  sttWS.beginSSL(STT_HOST,443,"/stt");

  uint32_t start=millis();

  while(!sttReady&&!sttError&&millis()-start<7000){
    sttWS.loop();delay(2);yield();
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

  String msg;serializeJson(j,msg);
  sttWS.sendTXT(msg);
  Serial.println("TARS: STT END SENT");

  uint32_t start=millis();

  while(!sttDone&&!sttError&&millis()-start<6000){
    sttWS.loop();delay(2);yield();
  }

  String result=sttFinal;
  sttWS.disconnect();
  return result;
}

/* MIC/STT - TETAP */
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

    if(i2s_read(MIC_PORT,rawBuf,sizeof(rawBuf),&bytes,pdMS_TO_TICKS(30))!=ESP_OK)
      continue;

    size_t count=bytes/4;
    int32_t peak=0;

    for(size_t i=0;i<count;i++){
      int32_t v=constrain(rawBuf[i]>>16,-32768,32767);
      pcmBuf[i]=(int16_t)v;
      int32_t a=abs(v);
      if(a>peak)peak=a;
    }

    if(!voice){
      for(size_t i=0;i<count;i++){
        preBuf[prePos]=pcmBuf[i];
        prePos=(prePos+1)%PREROLL_SAMPLES;
        if(preCount<PREROLL_SAMPLES)preCount++;
      }

      if(peak>=MIC_THRESHOLD){
        voice=true;
        voiceStart=millis();
        lastVoice=voiceStart;

        size_t start=preCount==PREROLL_SAMPLES?prePos:0,n=0;

        for(size_t i=0;i<preCount;i++){
          size_t k=(start+i)%PREROLL_SAMPLES;
          sendBuf[n++]=preBuf[k];

          if(n==256){
            if(!sttWS.sendBIN((uint8_t*)sendBuf,n*2)){
              sttError=true;
              break;
            }
            n=0;
          }
        }

        if(n&&!sttError)
          sttWS.sendBIN((uint8_t*)sendBuf,n*2);

        samples+=preCount;
        Serial.printf("TARS: VOICE DETECTED PEAK=%ld\n",(long)peak);
      }

    }else{
      if(!sttWS.sendBIN((uint8_t*)pcmBuf,count*2)){
        Serial.println("TARS: STT PCM SEND FAILED");
        sttError=true;
        break;
      }

      samples+=count;

      if(peak>=MIC_SILENCE)
        lastVoice=millis();

      if(millis()-voiceStart>=RECORD_MIN_MS&&
         millis()-lastVoice>=SILENCE_MS)
        break;
    }

    yield();
  }

  if(!voice){
    sttWS.disconnect();
    Serial.println("TARS: MIC AUDIO TOO LOW");
    return "";
  }

  if(sttError){
    sttWS.disconnect();
    return "";
  }

  return stopSTT(samples);
}

/* ASK - TETAP */
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

  uint32_t t=millis();
  int code=h.POST(body);

  Serial.printf("TARS: ASK HTTP=%d TIME=%lu ms\n",
    code,(unsigned long)(millis()-t));

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

/* TTS DIAGNOSTIC */
int b64val(char c){
  if(c>='A'&&c<='Z')return c-'A';
  if(c>='a'&&c<='z')return c-'a'+26;
  if(c>='0'&&c<='9')return c-'0'+52;
  if(c=='+')return 62;
  if(c=='/')return 63;
  return -1;
}

size_t decodeB64Head(const String&s,uint8_t*out,size_t maxOut){
  size_t n=0;
  int val=0,bits=-8;

  for(size_t i=0;i<s.length()&&n<maxOut;i++){
    int v=b64val(s[i]);
    if(v<0)continue;

    val=(val<<6)|v;
    bits+=6;

    if(bits>=0){
      out[n++]=(uint8_t)((val>>bits)&255);
      bits-=8;
    }
  }

  return n;
}

String detectAudio(const uint8_t*b,size_t n){
  if(n>=12&&memcmp(b,"RIFF",4)==0&&memcmp(b+8,"WAVE",4)==0)return "WAV";
  if(n>=3&&memcmp(b,"ID3",3)==0)return "MP3";
  if(n>=2&&b[0]==0xFF&&(b[1]&0xE0)==0xE0)return "MP3";
  if(n>=4&&memcmp(b,"OggS",4)==0)return "OGG";
  if(n>=4&&memcmp(b,"fLaC",4)==0)return "FLAC";
  if(n>=2&&b[0]==0xFF&&(b[1]&0xF6)==0xF0)return "AAC";
  return "UNKNOWN";
}

void printBytes(const uint8_t*b,size_t n){
  Serial.print("TARS: AUDIO HEADER=");

  for(size_t i=0;i<n;i++){
    if(b[i]<16)Serial.print('0');
    Serial.print(b[i],HEX);
    if(i+1<n)Serial.print(' ');
  }

  Serial.println();
}

bool streamAudio(const String&text){
  if(!wifiOK())return false;

  WiFiClientSecure c;
  c.setInsecure();
  c.setTimeout(15000);

  HTTPClient h;
  uint32_t totalStart=millis();

  if(!h.begin(c,String(TARS_CLOUD_URL)+"/tts"))
    return false;

  h.setTimeout(60000);
  h.addHeader("Content-Type","application/json");

  const char* hdr[]={
    "Content-Type",
    "Content-Length",
    "X-TARS-TTS",
    "X-TARS-TTS-FORMAT"
  };
  h.collectHeaders(hdr,4);

  JsonDocument j;
  j["text"]=text;

  String body;
  serializeJson(j,body);

  Serial.println("TARS: TTS REQUEST START");
  Serial.print("TARS: TTS TEXT = ");
  Serial.println(text);

  uint32_t t=millis();
  int code=h.POST(body);
  uint32_t httpTime=millis()-t;

  String ct=h.header("Content-Type");
  String cl=h.header("Content-Length");
  String xt=h.header("X-TARS-TTS");
  String xf=h.header("X-TARS-TTS-FORMAT");

  Serial.printf("TARS: AUDIO HTTP=%d TIME=%lu ms\n",
    code,(unsigned long)httpTime);

  Serial.print("TARS: TTS CONTENT-TYPE=");
  Serial.println(ct.length()?ct:"<none>");

  Serial.print("TARS: TTS CONTENT-LENGTH=");
  Serial.println(cl.length()?cl:"<none>");

  Serial.print("TARS: TTS HEADER STATUS=");
  Serial.println(xt.length()?xt:"<none>");

  Serial.print("TARS: TTS HEADER FORMAT=");
  Serial.println(xf.length()?xf:"<none>");

  /* ERROR */
  if(code<200||code>=300){
    String err=h.getString();

    Serial.println("TARS: ===== TTS DIAGNOSTIC =====");
    Serial.println("TARS: RESPONSE MODE=ERROR");
    Serial.printf("TARS: HTTP CODE=%d\n",code);
    Serial.printf("TARS: HTTP TIME=%lu ms",(unsigned long)httpTime);
    Serial.println();

    Serial.print("TARS: CONTENT-TYPE=");
    Serial.println(ct.length()?ct:"<none>");

    Serial.print("TARS: CONTENT-LENGTH=");
    Serial.println(cl.length()?cl:"<none>");

    Serial.print("TARS: X-TARS-TTS=");
    Serial.println(xt.length()?xt:"<none>");

    Serial.print("TARS: X-TARS-TTS-FORMAT=");
    Serial.println(xf.length()?xf:"<none>");

    Serial.printf("TARS: ERROR BODY LENGTH=%u\n",(unsigned)err.length());
    Serial.println("TARS: ERROR BODY BEGIN");
    Serial.println(err);
    Serial.println("TARS: ERROR BODY END");
    Serial.println("TARS: AUDIO PAYLOAD=NONE");
    Serial.println("TARS: ===== END TTS DIAGNOSTIC =====");

    h.end();
    return false;
  }

  Serial.println("TARS: TTS HTTP SUCCESS");

  String rawCT=ct;
  ct.toLowerCase();

  Serial.println("TARS: ===== TTS DIAGNOSTIC =====");

  bool jsonMode=
    ct.indexOf("application/json")>=0||
    ct.indexOf("text/json")>=0;

  Serial.print("TARS: RESPONSE MODE=");
  Serial.println(jsonMode?"JSON":"BINARY");

  Serial.print("TARS: CONTENT-TYPE=");
  Serial.println(rawCT.length()?rawCT:"<none>");

  Serial.print("TARS: CONTENT-LENGTH=");
  Serial.println(cl.length()?cl:"<none>");

  Serial.print("TARS: X-TARS-TTS=");
  Serial.println(xt.length()?xt:"<none>");

  Serial.print("TARS: X-TARS-TTS-FORMAT=");
  Serial.println(xf.length()?xf:"<none>");

  /* JSON RESPONSE */
  if(jsonMode){
    String r=h.getString();

    JsonDocument x;
    DeserializationError de=deserializeJson(x,r);

    if(de){
      Serial.print("TARS: JSON PARSE ERROR=");
      Serial.println(de.c_str());
      Serial.println("TARS: JSON AUDIO FIELD=NO");
      Serial.println("TARS: ===== END TTS DIAGNOSTIC =====");
      h.end();
      return false;
    }

    JsonVariant av=x["audio"];

    if(av.is<const char*>()){
      String b64=av.as<String>();

      Serial.println("TARS: JSON AUDIO FIELD=YES");
      Serial.printf("TARS: BASE64 LENGTH=%u\n",(unsigned)b64.length());

      uint8_t head[32];
      size_t n=decodeB64Head(b64,head,sizeof(head));

      Serial.printf(
        "TARS: BASE64 DECODED HEAD=%u BYTES\n",
        (unsigned)n
      );

      if(n){
        printBytes(head,n);

        Serial.print("TARS: DETECTED FORMAT=");
        Serial.println(detectAudio(head,n));
      }else{
        Serial.println("TARS: DETECTED FORMAT=UNKNOWN");
      }

    }else{
      Serial.println("TARS: JSON AUDIO FIELD=NO");
      Serial.println("TARS: JSON HAS NO AUDIO PAYLOAD");
    }

    Serial.println("TARS: ===== END TTS DIAGNOSTIC =====");
    h.end();

    return false;
  }

  /* BINARY RESPONSE */
  bool isWav=
    ct.indexOf("audio/wav")>=0||
    ct.indexOf("audio/x-wav")>=0||
    ct.indexOf("audio/wave")>=0||
    ct.indexOf("application/wav")>=0;

  bool isMp3=
    ct.indexOf("audio/mpeg")>=0||
    ct.indexOf("audio/mp3")>=0;

  Serial.println("TARS: BINARY AUDIO RESPONSE");

  if(!isWav&&!isMp3)
    Serial.println("TARS: CONTENT-TYPE FORMAT=UNKNOWN");
  else
    Serial.printf(
      "TARS: CONTENT-TYPE FORMAT=%s\n",
      isWav?"WAV":"MP3"
    );

  Serial.println("TARS: NOTE=PLAYBACK PATH UNCHANGED");
  Serial.println("TARS: ===== END TTS DIAGNOSTIC =====");

  WiFiClient*stream=h.getStreamPtr();

  if(!stream){
    Serial.println("TARS: AUDIO STREAM NULL");
    h.end();
    return false;
  }

  if(!dacOK)dacOK=initDAC();

  if(!dacOK){
    h.end();
    return false;
  }

  if(!isWav&&!isMp3){
    Serial.println("TARS: TTS UNKNOWN AUDIO FORMAT");
    h.end();
    return false;
  }

  uint32_t bs=millis();

  while(stream->available()<1024&&millis()-bs<500)
    delay(2);

  Serial.printf(
    "TARS: AUDIO BYTES AVAILABLE=%d\n",
    stream->available()
  );

  stereoOut.begin(audioIn,audioOut);

  if(isWav){
    Serial.println("TARS: FORMAT=WAV");
    Serial.println("TARS: WAV MONO->STEREO");

    wavDec.addNotifyAudioChange(stereoOut);
    wavDec.begin();
    copier.begin(wavDec,*stream);

    Serial.println("TARS: WAV STREAM START");

  }else{
    Serial.println("TARS: FORMAT=MP3");
    Serial.println("TARS: MP3 MONO->STEREO");

    mp3Dec.addNotifyAudioChange(stereoOut);
    mp3Dec.begin();
    copier.begin(mp3Dec,*stream);

    Serial.println("TARS: MP3 STREAM START");
  }

  playing=true;

  bool started=false;
  uint32_t start=millis(),lastData=start,copyCount=0;

  while(true){
    bool copied=copier.copy();

    if(copied){
      copyCount++;
      lastData=millis();

      if(!started){
        started=true;
        oledStartSpeak(text);

        AudioInfo info=isWav?wavDec.audioInfo():mp3Dec.audioInfo();

        Serial.printf(
          "TARS: AUDIOINFO %lu Hz / %d CH / %d BIT\n",
          (unsigned long)info.sample_rate,
          info.channels,
          info.bits_per_sample
        );

        Serial.printf(
          "TARS: OUTPUT 44100 Hz / 2 CH / 16 BIT\n"
          "TARS: %s DECODE START=%lu ms\n",
          isWav?"WAV":"MP3",
          (unsigned long)(millis()-start)
        );
      }
    }

    bool available=stream->available();

    if(started&&!available&&millis()-lastData>=AUDIO_IDLE_MS)
      break;

    if(!started&&!h.connected()&&!available)
      break;

    if(millis()-start>70000){
      Serial.println("TARS: AUDIO TIMEOUT");
      break;
    }

    yield();
  }

  Serial.printf(
    "TARS: %s COPY CALLS=%lu\n",
    isWav?"WAV":"MP3",
    (unsigned long)copyCount
  );

  Serial.printf(
    "TARS: AUDIO STREAM=%lu ms\n",
    (unsigned long)(millis()-start)
  );

  if(isWav)wavDec.end();
  else mp3Dec.end();

  stereoOut.end();
  h.end();
  playing=false;

  Serial.printf(
    "TARS: AUDIO TOTAL=%lu ms\n",
    (unsigned long)(millis()-totalStart)
  );

  oledSetListening();

  if(!started)
    Serial.printf(
      "TARS: %s DATA RECEIVED BUT DECODER NEVER STARTED\n",
      isWav?"WAV":"MP3"
    );

  return started;
}

/* PROCESS */
void processQuestion(const String&q){
  String answer=ask(q);

  if(!answer.length()){
    oledSetStatus("ASK ERROR");
    return;
  }

  uint32_t t=millis();
  bool ok=streamAudio(answer);

  Serial.printf(
    "TARS: AUDIO FUNCTION TIME=%lu ms\n",
    (unsigned long)(millis()-t)
  );

  oledSetStatus(ok?"LISTENING":"AUDIO ERROR");
}

/* SETUP */
void setup(){
  Serial.begin(SERIAL_BAUD);

  Wire.begin(OLED_SDA,OLED_SCL);
  Wire.setClock(400000);

  oledOK=oled.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR);

  if(oledOK){
    oledHeader();
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

  Serial.println("TARS: PAM RIGHT GPIO26");
  Serial.println("TARS: PAM LEFT GPIO25 UNUSED");
  Serial.println("TARS: AUDIO OUTPUT RIGHT");
  Serial.println("TARS: INMP441 RIGHT GPIO34");
  Serial.println("TARS: MIC THRESHOLD=8000");
  Serial.println("TARS: MIC SILENCE=6000");
  Serial.println("TARS: PREROLL=700 ms");
  Serial.println("TARS: STT REALTIME PCM");
  Serial.println("TARS: BLUETOOTH DISABLED");
  Serial.println("TARS: MP3/WAV AUTO STREAMING");
  Serial.println("TARS: MONO->STEREO FORMAT CONVERTER");
  Serial.println("TARS: OUTPUT 44100 Hz / 2 CH / 16 BIT");
  Serial.println("TARS: AUTO AUDIOINFO");
  Serial.println("TARS: TTS = MELOTTS");
  Serial.println("TARS: TTS RESPONSE DIAGNOSTIC ENABLED");
  Serial.println("TARS: JSON/BINARY DETECTION ENABLED");
  Serial.println("TARS: AUDIO MAGIC-BYTE DETECTION ENABLED");

  if(oledOK)
    xTaskCreatePinnedToCore(
      oledTask,"TARS_OLED",4096,nullptr,1,nullptr,0
    );

  wifiManagerBegin();

  if(bootWiFi())
    syncTime();

  oledSetListening();
}

/* LOOP */
void loop(){
  if(playing){
    delay(1);
    return;
  }

  if(WiFi.status()!=WL_CONNECTED&&!wifiOK()){
    oledSetStatus("WIFI ERROR");
    delay(500);
    return;
  }

  String q=recordRealtime();

  if(q.length())
    processQuestion(q);
  else
    oledSetListening();

  delay(1);
}
