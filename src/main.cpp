#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Wire.h>
#include <time.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <mbedtls/base64.h>
#include <WebSocketsClient.h>
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
const uint32_t PREROLL_MS=500,OLED_TYPE_MS=40;
const int32_t MIC_THRESHOLD=8000,MIC_SILENCE=6000;
const size_t BUF=1024,DAC_BUF=16384,PREROLL_SAMPLES=MIC_RATE*PREROLL_MS/1000;

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);
WebSocketsClient sttWS;

bool oledOK=false,micOK=false,dacOK=false,playing=false,singMode=false,ntpOK=false;
bool sttConnected=false;
String oledText,sttFinal;
size_t oledPos=0;
uint32_t oledTick=0,dotTick=0;
uint8_t dotState=1;

// ================= OLED =================
void oledHeader(const char*t){
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(42,0);oled.print("T A R S");
  oled.drawLine(0,9,127,9,SSD1306_WHITE);
  oled.setCursor(3,14);oled.print(t);
}

void oledBase(const char*t,const String&s=""){
  if(!oledOK)return;
  oledHeader(t);
  if(s.length()){oled.setCursor(3,27);oled.print(s);}
  oled.display();
}

void oledListening(){
  if(!oledOK||millis()-dotTick<350)return;
  dotTick=millis();dotState=dotState>=4?1:dotState+1;
  String s;for(uint8_t i=0;i<dotState;i++)s+='.';
  oledBase("LISTENING",s);
}

void oledType(){
  if(!oledOK||!oledText.length()||millis()-oledTick<OLED_TYPE_MS)return;
  if(oledPos>=oledText.length())return;

  oledTick=millis();
  const char*t=singMode?"SINGING":"SPEAKING";
  oledHeader(t);

  int x=3,y=27;
  size_t end=min(oledPos+4,oledText.length());

  for(size_t i=0;i<end;i++){
    char ch=oledText[i];
    if(ch=='\n'||x>121){
      x=3;y+=8;
      if(y>59){oledHeader(t);x=3;y=27;}
      if(ch=='\n')continue;
    }
    oled.setCursor(x,y);oled.write(ch);x=oled.getCursorX();
  }

  oledPos=end;
  oled.display();
}

// ================= DAC =================
bool initDAC(){
  pinMode(AUDIO_DAC_PIN,OUTPUT);
  dacWrite(AUDIO_DAC_PIN,0);
  Serial.println("TARS: DIRECT DAC GPIO26 READY");
  return true;
}

void dacMute(){dacWrite(AUDIO_DAC_PIN,0);}
void dacCenter(){dacWrite(AUDIO_DAC_PIN,128);}

// ================= INMP441 =================
bool initMic(){
  i2s_config_t c={};
  c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_RX);
  c.sample_rate=MIC_RATE;
  c.bits_per_sample=I2S_BITS_PER_SAMPLE_32BIT;
  c.channel_format=I2S_CHANNEL_FMT_ONLY_RIGHT;
  c.communication_format=I2S_COMM_FORMAT_I2S;
  c.dma_buf_count=2;
  c.dma_buf_len=256;

  if(i2s_driver_install(MIC_PORT,&c,0,nullptr)!=ESP_OK)return false;

  i2s_pin_config_t p={
    I2S_PIN_NO_CHANGE,MIC_SCK,MIC_WS,
    I2S_PIN_NO_CHANGE,MIC_SD
  };

  return i2s_set_pin(MIC_PORT,&p)==ESP_OK;
}

// ================= WIFI / NTP =================
bool syncTime(){
  configTime(7*3600,0,"pool.ntp.org","time.nist.gov","time.google.com");

  for(int a=1;a<=4;a++){
    Serial.printf("TARS: NTP SYNC %d/4\r\n",a);

    for(int i=0;i<20;i++){
      time_t now=time(nullptr);

      if(now>=1704067200){
        struct tm t;
        localtime_r(&now,&t);

        Serial.printf(
          "TARS: NTP VALID %04d-%02d-%02d %02d:%02d:%02d\r\n",
          t.tm_year+1900,t.tm_mon+1,t.tm_mday,
          t.tm_hour,t.tm_min,t.tm_sec
        );

        ntpOK=true;
        return true;
      }

      delay(500);
    }
  }

  ntpOK=false;
  Serial.println("TARS: NTP FAILED AFTER 4 ATTEMPTS");
  return false;
}

bool wifiOK(){
  if(WiFi.status()!=WL_CONNECTED&&!wifiManagerConnect(false))
    return false;

  if(!ntpOK&&!syncTime())
    return false;

  return true;
}

// ================= STT REALTIME =================
String cloudHost(){
  String h=TARS_CLOUD_URL;
  int p=h.indexOf("://");

  if(p>=0)h=h.substring(p+3);
  p=h.indexOf('/');

  if(p>=0)h=h.substring(0,p);
  return h;
}

void sttEvent(WStype_t type,uint8_t*payload,size_t len){
  if(type==WStype_CONNECTED){
    sttConnected=true;
    Serial.println("TARS: STT WS READY");
    return;
  }

  if(type==WStype_DISCONNECTED){
    sttConnected=false;
    Serial.println("TARS: STT WS DISCONNECTED");
    return;
  }

  if(type!=WStype_TEXT)return;

  String s;
  s.reserve(len+1);

  for(size_t i=0;i<len;i++)s+=(char)payload[i];

  JsonDocument j;
  if(deserializeJson(j,s))return;

  String typeName=j["type"].as<String>();

  if(typeName=="status"){
    Serial.print("TARS: STT STATUS = ");
    Serial.println(j["status"].as<String>());
  }
  else if(typeName=="partial"){
    String text=j["text"].as<String>();
    if(text.length()){
      Serial.print("TARS: STT PARTIAL = ");
      Serial.println(text);
    }
  }
  else if(typeName=="final"){
    sttFinal=j["text"].as<String>();
    sttFinal.trim();

    if(sttFinal.length()){
      Serial.print("TARS: YOU SAID = ");
      Serial.println(sttFinal);
    }
  }
  else if(typeName=="error"){
    Serial.print("TARS: STT ERROR = ");
    Serial.println(j["error"].as<String>());
  }
}

bool initSTT(){
  String host=cloudHost();

  sttWS.beginSSL(host.c_str(),443,"/stt-ws");
  sttWS.onEvent(sttEvent);
  sttWS.setReconnectInterval(5000);
  sttWS.enableHeartbeat(15000,3000,2);

  Serial.println("TARS: STT WS CONNECTING");
  return true;
}

String b64PCM(const uint8_t*data,size_t len){
  size_t outLen=0;

  if(mbedtls_base64_encode(nullptr,0,&outLen,data,len)!=
     MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL)
    return "";

  uint8_t*buf=(uint8_t*)malloc(outLen+1);
  if(!buf)return "";

  if(mbedtls_base64_encode(buf,outLen+1,&outLen,data,len)!=0){
    free(buf);
    return "";
  }

  buf[outLen]=0;

  String out=(char*)buf;
  free(buf);
  return out;
}

bool sendAudio(const int16_t*pcm,size_t samples,bool commit=false){
  if(!sttConnected)return false;
  if(!samples&&!commit)return false;

  String b64="";

  if(samples){
    b64=b64PCM(
      (const uint8_t*)pcm,
      samples*2
    );

    if(!b64.length())return false;
  }

  JsonDocument j;
  j["type"]="audio";
  j["audio_base64"]=b64;

  if(commit)j["commit"]=true;

  String msg;
  serializeJson(j,msg);

  return sttWS.sendTXT(msg);
}

String realtimeSTT(){
  if(!wifiOK())return "";

  if(!sttConnected){
    Serial.println("TARS: STT WS NOT READY");

    uint32_t t=millis();

    while(!sttConnected&&millis()-t<6000){
      sttWS.loop();
      delay(5);
    }

    if(!sttConnected)return "";
  }

  sttFinal="";

  JsonDocument start;
  start["type"]="start";

  String startMsg;
  serializeJson(start,startMsg);

  if(!sttWS.sendTXT(startMsg))
    return "";

  Serial.println("TARS: REALTIME STT START");
  oledBase("LISTENING",".");

  static int16_t pre[PREROLL_SAMPLES];

  size_t prePos=0;
  size_t preCount=0;

  int32_t raw[BUF/4];
  int16_t pcm[BUF/4];

  uint32_t voiceStart=0,lastVoice=0;
  bool voiceStarted=false;
  uint8_t activeBlocks=0;

  // STANDBY TANPA BATAS WAKTU SEBELUM ADA SUARA
  while(!voiceStarted){
    sttWS.loop();
    oledListening();

    size_t n=0;

    if(i2s_read(
      MIC_PORT,raw,sizeof(raw),&n,pdMS_TO_TICKS(50)
    )!=ESP_OK)continue;

    size_t count=n/4;
    int32_t blockPeak=0;

    for(size_t i=0;i<count;i++){
      int32_t v=constrain(
        raw[i]>>16,
        (int32_t)-32768,
        (int32_t)32767
      );

      pcm[i]=(int16_t)v;
      blockPeak=max(blockPeak,abs(v));

      pre[prePos]=pcm[i];
      prePos=(prePos+1)%PREROLL_SAMPLES;

      if(preCount<PREROLL_SAMPLES)
        preCount++;
    }

    if(blockPeak>=MIC_THRESHOLD){
      if(++activeBlocks>=2){
        voiceStarted=true;
        voiceStart=millis();
        lastVoice=voiceStart;

        Serial.println("TARS: VOICE DETECTED");

        size_t startPos=
          preCount==PREROLL_SAMPLES?prePos:0;

        for(size_t pos=0;pos<preCount;pos+=256){
          size_t nS=min((size_t)256,preCount-pos);
          int16_t tmp[256];

          for(size_t k=0;k<nS;k++)
            tmp[k]=pre[
              (startPos+pos+k)%PREROLL_SAMPLES
            ];

          sendAudio(tmp,nS,false);
          sttWS.loop();
        }
      }
    }else{
      activeBlocks=0;
    }

    yield();
  }

  // SUDAH ADA SUARA: REKAM MAKSIMAL 5 DETIK
  while(millis()-voiceStart<RECORD_MAX_MS){
    sttWS.loop();
    oledListening();

    size_t n=0;

    if(i2s_read(
      MIC_PORT,raw,sizeof(raw),&n,pdMS_TO_TICKS(50)
    )!=ESP_OK)continue;

    size_t count=n/4;
    int32_t blockPeak=0;

    for(size_t i=0;i<count;i++){
      int32_t v=constrain(
        raw[i]>>16,
        (int32_t)-32768,
        (int32_t)32767
      );

      pcm[i]=(int16_t)v;
      blockPeak=max(blockPeak,abs(v));
    }

    sendAudio(pcm,count,false);

    uint32_t now=millis();

    if(blockPeak>=MIC_SILENCE)
      lastVoice=now;

    if(now-voiceStart>=RECORD_MIN_MS&&
       now-lastVoice>=SILENCE_MS){
      break;
    }

    yield();
  }

  // COMMIT HANYA SETELAH ADA SUARA
  Serial.println("TARS: STT COMMIT");
  sendAudio(nullptr,0,true);

  // TUNGGU FINAL
  uint32_t t=millis();

  while(
    millis()-t<3000&&
    !sttFinal.length()
  ){
    sttWS.loop();
    delay(5);
  }

  sttFinal.trim();

  if(sttFinal.length()){
    Serial.print("TARS: FINAL STT = ");
    Serial.println(sttFinal);
  }else{
    Serial.println("TARS: STT NO FINAL");
  }

  return sttFinal;
}

// ================= ASK =================
String ask(const String&q){
  if(!wifiOK())return "";

  Serial.print("TARS: ASK = ");
  Serial.println(q);

  oledBase("PROCESSING","ANALYZING...");

  WiFiClientSecure c;
  c.setInsecure();

  HTTPClient h;

  if(!h.begin(c,String(TARS_CLOUD_URL)+"/ask"))
    return "";

  h.setTimeout(30000);
  h.addHeader("Content-Type","application/json");

  JsonDocument j;
  j["text"]=q;

  String b;
  serializeJson(j,b);

  int code=h.POST(b);

  Serial.printf(
    "TARS: ASK HTTP = %d\r\n",
    code
  );

  if(code<200||code>=300){
    h.end();
    return "";
  }

  String r=h.getString();
  h.end();

  JsonDocument x;

  if(deserializeJson(x,r))
    return "";

  String s=x["response"].as<String>();
  s.trim();

  Serial.print("TARS: ANSWER = ");
  Serial.println(s);

  return s;
}

// ================= TTS =================
bool downloadMP3(const String&url,const String&text){
  if(!wifiOK())return false;

  Serial.println(
    url.endsWith("/sing")?
    "TARS: SING":"TARS: TTS"
  );

  WiFiClientSecure c;
  c.setInsecure();

  HTTPClient h;

  if(!h.begin(c,url))
    return false;

  h.setTimeout(60000);
  h.addHeader("Content-Type","application/json");

  JsonDocument j;
  j["text"]=text;

  String b;
  serializeJson(j,b);

  int code=h.POST(b);

  Serial.printf(
    "TARS: AUDIO HTTP = %d\r\n",
    code
  );

  if(code<200||code>=300){
    h.end();
    return false;
  }

  if(LittleFS.exists(MP3_FILE))
    LittleFS.remove(MP3_FILE);

  File f=LittleFS.open(MP3_FILE,FILE_WRITE);

  if(!f){
    h.end();
    return false;
  }

  WiFiClient*s=h.getStreamPtr();
  uint8_t buf[BUF];

  int len=h.getSize();
  size_t total=0;
  uint32_t t=millis();

  while(h.connected()&&(len>0||len==-1)){
    size_t n=s->available();

    if(n){
      n=min(n,sizeof(buf));

      int r=s->readBytes(buf,n);

      if(r>0){
        f.write(buf,r);
        total+=r;

        if(len>0)
          len-=r;

        t=millis();
      }
    }else{
      if(millis()-t>5000)
        break;

      delay(1);
    }

    yield();
  }

  f.close();
  h.end();

  Serial.printf(
    "TARS: MP3 BYTES = %lu\r\n",
    (unsigned long)total
  );

  return total>0;
}

// ================= DAC RING =================
class DACOut:public AudioStream{
  AudioInfo info;
  int16_t buffer[DAC_BUF];
  volatile size_t head=0,tail=0;
  volatile bool active=false;
  TaskHandle_t task=nullptr;

  uint32_t calls=0,pcmBytes=0,dacSamples=0,errors=0;
  uint32_t sampleRate=PLAY_RATE;
  int32_t peak=0;

  size_t count(){
    size_t h=head,t=tail;
    return h>=t?h-t:DAC_BUF-t+h;
  }

  size_t freeSpace(){
    return DAC_BUF-1-count();
  }

  void run(){
    uint32_t rate=sampleRate?sampleRate:PLAY_RATE;
    uint32_t base=1000000UL/rate;
    uint32_t rem=1000000UL%rate;
    uint32_t frac=0,next=micros();
    bool started=false;

    while(active){
      if(head==tail){
        next=micros();
        frac=0;
        vTaskDelay(1);
        continue;
      }

      int16_t sample=buffer[tail];
      tail=(tail+1)%DAC_BUF;

      int32_t v=((int32_t)sample*3)/4;
      peak=max(peak,abs(v));

      if(!started){
        dacCenter();
        next=micros();
        frac=0;
        started=true;
      }

      int32_t out=constrain(
        (v+32768+128)>>8,
        0,255
      );

      while((int32_t)(next-micros())>0){
        delayMicroseconds(1);

        if((int32_t)(next-micros())>200)
          taskYIELD();
      }

      dacWrite(AUDIO_DAC_PIN,(uint8_t)out);

      dacSamples++;
      next+=base;
      frac+=rem;

      if(frac>=rate){
        next++;
        frac-=rate;
      }

      if((int32_t)(micros()-next)>50000){
        next=micros();
        frac=0;
      }

      if(!(dacSamples&63))
        taskYIELD();
    }

    dacMute();
    active=false;
    task=nullptr;
    vTaskDelete(nullptr);
  }

  static void taskFunc(void*a){
    ((DACOut*)a)->run();
  }

public:
  void setAudioInfo(AudioInfo i)override{
    info=i;
    AudioStream::setAudioInfo(i);

    sampleRate=
      info.sample_rate?
      info.sample_rate:
      PLAY_RATE;

    Serial.printf(
      "TARS: DAC AUDIO %uHz %ubit %uch GPIO26\r\n",
      info.sample_rate,
      info.bits_per_sample,
      info.channels
    );
  }

  int availableForWrite()override{
    return freeSpace()*2;
  }

  void resetStats(){
    calls=pcmBytes=dacSamples=errors=0;
    peak=0;
    head=tail=0;
  }

  void startDAC(){
    head=tail=0;
    dacMute();
    active=true;

    if(task)return;

    if(xTaskCreatePinnedToCore(
      taskFunc,
      "TARS_DAC",
      4096,
      this,
      2,
      &task,
      1
    )!=pdPASS){
      task=nullptr;
      active=false;
      Serial.println("TARS: DAC TASK ERROR");
    }
  }

  bool empty(){
    return head==tail;
  }

  bool waitDrain(){
    uint32_t t=millis();

    while(!empty()&&millis()-t<10000){
      oledType();
      sttWS.loop();
      delay(1);
      yield();
    }

    if(!empty()){
      Serial.println("TARS: DAC DRAIN TIMEOUT");
      return false;
    }

    delay(5);
    return true;
  }

  void stopDAC(){
    waitDrain();
    active=false;

    uint32_t t=millis();

    while(task){
      vTaskDelay(1);

      if(millis()-t>2000)
        break;
    }

    dacMute();
  }

  void printStats(){
    Serial.printf(
      "TARS: DAC CALL=%lu PCM=%lu SAMPLES=%lu PEAK=%ld ERROR=%lu\r\n",
      (unsigned long)calls,
      (unsigned long)pcmBytes,
      (unsigned long)dacSamples,
      (long)peak,
      (unsigned long)errors
    );
  }

  size_t write(const uint8_t*d,size_t n)override{
    calls++;
    pcmBytes+=n;

    if(!dacOK||!d||!n||
       info.bits_per_sample!=16||
       (info.channels!=1&&info.channels!=2)){
      errors++;
      return 0;
    }

    size_t bpf=info.channels*2;
    size_t frames=n/bpf;
    size_t done=0;

    if(!frames)return 0;

    while(done<frames){
      if(!active){
        errors++;
        return done*bpf;
      }

      size_t space=freeSpace();

      if(!space){
        vTaskDelay(1);
        continue;
      }

      size_t chunk=min(space,frames-done);

      for(size_t i=0;i<chunk;i++){
        size_t k=done+i;

        int16_t s=
          info.channels==1?
          (int16_t)(
            d[k*2]|
            ((uint16_t)d[k*2+1]<<8)
          ):
          (int16_t)(
            d[k*4+2]|
            ((uint16_t)d[k*4+3]<<8)
          );

        buffer[head]=s;
        head=(head+1)%DAC_BUF;
      }

      done+=chunk;
      taskYIELD();
    }

    return n;
  }
}dacOut;

MP3DecoderHelix decoder;
EncodedAudioStream mp3(&dacOut,&decoder);

// ================= PLAY =================
bool playMP3(){
  File f=LittleFS.open(MP3_FILE,FILE_READ);
  if(!f)return false;

  Serial.printf(
    "TARS: PLAY MP3 SIZE=%u\r\n",
    (unsigned)f.size()
  );

  dacOut.resetStats();
  playing=true;

  oledPos=0;
  oledTick=millis();

  if(!dacOK)
    dacOK=initDAC();

  if(!dacOK){
    f.close();
    playing=false;
    return false;
  }

  if(!mp3.begin()){
    f.close();
    playing=false;
    Serial.println("TARS: MP3 DECODER ERROR");
    return false;
  }

  oledBase(singMode?"SINGING":"SPEAKING");
  dacOut.startDAC();

  StreamCopy copy(mp3,f,BUF);
  uint32_t start=millis();

  while(f.available()&&millis()-start<120000){
    sttWS.loop();

    if(!copy.copy())
      delay(1);

    oledType();
    yield();
  }

  mp3.end();
  f.close();

  dacOut.waitDrain();
  dacOut.stopDAC();

  playing=false;
  dacMute();
  dacOut.printStats();

  LittleFS.remove(MP3_FILE);
  oledBase("READY","WAITING...");
  Serial.println("TARS: PLAYBACK DONE");

  return true;
}

// ================= SING =================
bool singRequest(String s){
  s.toLowerCase();

  return s.indexOf("nyanyi")>=0||
         s.indexOf("bernyanyi")>=0||
         s.indexOf("nyanyikan")>=0;
}

// ================= PROCESS =================
void processQuestion(const String&q){
  uint32_t total=millis();

  uint32_t t=millis();
  String answer=ask(q);

  Serial.printf(
    "TARS: ASK TIME=%lu ms\r\n",
    (unsigned long)(millis()-t)
  );

  if(!answer.length()){
    oledBase("READY","ASK ERROR");
    return;
  }

  oledText=answer;
  oledPos=0;
  singMode=singRequest(q);

  String url=
    String(TARS_CLOUD_URL)+
    (singMode?"/sing":"/tts");

  t=millis();

  if(downloadMP3(
    url,
    singMode?q:answer
  )){
    Serial.printf(
      "TARS: TTS TIME=%lu ms\r\n",
      (unsigned long)(millis()-t)
    );

    playMP3();
  }else{
    oledBase("READY","AUDIO ERROR");
  }

  Serial.printf(
    "TARS: TOTAL PROCESS=%lu ms\r\n",
    (unsigned long)(millis()-total)
  );

  singMode=false;
}

// ================= SETUP =================
void setup(){
  Serial.begin(SERIAL_BAUD);

  Wire.begin(OLED_SDA,OLED_SCL);
  Wire.setClock(400000);

  oledOK=oled.begin(
    SSD1306_SWITCHCAPVCC,
    OLED_ADDR
  );

  if(oledOK)
    oledBase("BOOT");

  LittleFS.begin(true);

  dacOK=initDAC();
  micOK=initMic();

  Serial.printf(
    "TARS: DAC %s\r\n",
    dacOK?"READY":"ERROR"
  );

  Serial.printf(
    "TARS: MIC %s\r\n",
    micOK?"READY":"ERROR"
  );

  Serial.printf(
    "TARS: MIC START=%ld SILENCE=%ld\r\n",
    (long)MIC_THRESHOLD,
    (long)MIC_SILENCE
  );

  Serial.println("TARS: DIRECT AUDIO GPIO26");
  Serial.println("TARS: BLUETOOTH DISABLED");
  Serial.println("TARS: REALTIME STT ENABLED");

  wifiManagerBegin();

  if(wifiOK())
    Serial.println("TARS: WIFI + NTP READY");

  initSTT();

  oledBase("READY","WAITING...");
}

// ================= LOOP =================
void loop(){
  sttWS.loop();

  if(playing){
    delay(10);
    return;
  }

  if(WiFi.status()!=WL_CONNECTED){
    ntpOK=false;

    if(!wifiOK()){
      oledBase("READY","WIFI ERROR");
      delay(1000);
      return;
    }
  }

  if(!sttConnected){
    sttWS.loop();
    oledListening();
    delay(10);
    return;
  }

  String q=realtimeSTT();

  if(q.length())
    processQuestion(q);
  else
    oledBase("READY","NO INPUT");

  sttWS.loop();
  delay(10);
}
