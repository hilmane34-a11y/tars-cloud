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

const uint32_t MIC_RATE=16000;
const uint32_t RECORD_MAX_MS=5000;
const uint32_t RECORD_MIN_MS=500;
const uint32_t SILENCE_MS=900;
const uint32_t LISTEN_MAX_MS=8000;
const uint32_t PREROLL_MS=500;

const int32_t MIC_THRESHOLD=14000;
const int32_t MIC_SILENCE=12000;

const uint32_t OLED_TYPE_MS=35;
const uint32_t OLED_WAVE_MS=70;

const size_t BUF=2048;
const size_t PREROLL_SAMPLES=MIC_RATE*PREROLL_MS/1000;

const char*STT_FILE="/stt.wav";

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);
AnalogAudioStream analog;
MP3DecoderHelix codec;
EncodedAudioStream dec(&analog,&codec);
StreamCopy copier;

bool oledOK=false,micOK=false,dacOK=false;
bool playing=false,ntpOK=false,singMode=false;

String oledText="",oledStatus="READY";
uint32_t oledTypePos=0,oledLastType=0,oledLastWave=0;

/* ================= OLED ================= */

void oledHeader(){
  if(!oledOK)return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(2);
  oled.setCursor(36,0);
  oled.print("TARS");
  oled.display();
}

void oledSetStatus(const String&s){
  oledStatus=s;
  oledText="";
  oledTypePos=0;
}

void oledSetListening(){
  oledStatus="LISTENING";
  oledText="";
  oledTypePos=0;
}

void oledStartSpeak(const String&s){
  oledStatus="SPEAKING";
  oledText=s;
  oledTypePos=0;
  oledLastType=millis();
}

void oledTask(void*){
  for(;;){
    if(!oledOK){
      vTaskDelay(pdMS_TO_TICKS(50));
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

      oled.setTextSize(1);
      oled.setCursor(3,0);
      oled.print("T A R S");

      oled.setCursor(3,13);
      oled.print(oledStatus);

      if(oledText.length()){
        oled.setCursor(3,27);
        String s=oledText.substring(
          0,
          min((size_t)oledTypePos,oledText.length())
        );
        oled.print(s);
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

/* ================= DAC ================= */

bool initDAC(){
  auto cfg=analog.defaultConfig(TX_MODE);
  cfg.channels=2;

  if(!analog.begin(cfg)){
    Serial.println("TARS: DAC ERROR");
    return false;
  }

  Serial.println("TARS: PAM RIGHT GPIO26 READY");
  return true;
}

/* ================= INMP441 ================= */

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

/* ================= WAV ================= */

void put16(uint8_t*p,uint16_t v){
  p[0]=v;
  p[1]=v>>8;
}

void put32(uint8_t*p,uint32_t v){
  p[0]=v;
  p[1]=v>>8;
  p[2]=v>>16;
  p[3]=v>>24;
}

void wavHeader(File&f,uint32_t n){
  uint8_t h[44]={};

  memcpy(h,"RIFF",4);
  put32(h+4,n+36);
  memcpy(h+8,"WAVEfmt ",8);
  put32(h+16,16);
  put16(h+20,1);
  put16(h+22,1);
  put32(h+24,MIC_RATE);
  put32(h+28,MIC_RATE*2);
  put16(h+32,2);
  put16(h+34,16);
  memcpy(h+36,"data",4);
  put32(h+40,n);

  f.seek(0);
  f.write(h,44);
}

/* ================= RECORD ================= */

bool recordMic(){
  if(!micOK)return false;

  oledSetListening();

  if(LittleFS.exists(STT_FILE))
    LittleFS.remove(STT_FILE);

  File f=LittleFS.open(STT_FILE,FILE_WRITE);
  if(!f)return false;

  uint8_t z[44]={};
  f.write(z,44);

  static int16_t pre[PREROLL_SAMPLES];

  size_t prePos=0,preCount=0;
  int32_t raw[BUF/4];
  int16_t pcm[BUF/4];

  uint32_t startTime=millis();
  uint32_t voiceStart=0;
  uint32_t lastVoice=0;
  uint32_t samples=0;

  bool voice=false;

  while(millis()-startTime<LISTEN_MAX_MS){

    size_t n=0;

    if(i2s_read(
      MIC_PORT,
      raw,
      sizeof(raw),
      &n,
      pdMS_TO_TICKS(30)
    )!=ESP_OK)
      continue;

    size_t count=n/4;
    int32_t peak=0;

    for(size_t i=0;i<count;i++){
      int32_t v=constrain(raw[i]>>16,-32768,32767);
      pcm[i]=(int16_t)v;
      peak=max(peak,abs(v));
    }

    if(!voice){

      for(size_t i=0;i<count;i++){
        pre[prePos]=pcm[i];
        prePos=(prePos+1)%PREROLL_SAMPLES;
        if(preCount<PREROLL_SAMPLES)
          preCount++;
      }

      if(peak>=MIC_THRESHOLD){

        voice=true;
        voiceStart=lastVoice=millis();

        size_t start=
          preCount==PREROLL_SAMPLES?prePos:0;

        for(size_t i=0;i<preCount;i++){
          size_t k=(start+i)%PREROLL_SAMPLES;
          f.write((uint8_t*)&pre[k],2);
        }

        samples+=preCount;

        f.write(
          (uint8_t*)pcm,
          count*2
        );

        samples+=count;

        Serial.println("TARS: VOICE DETECTED");
      }

    }else{

      f.write(
        (uint8_t*)pcm,
        count*2
      );

      samples+=count;

      if(peak>=MIC_SILENCE)
        lastVoice=millis();

      if(
        millis()-voiceStart>=RECORD_MIN_MS&&
        millis()-lastVoice>=SILENCE_MS
      )
        break;

      if(millis()-voiceStart>=RECORD_MAX_MS)
        break;
    }

    yield();
  }

  wavHeader(f,samples*2);
  f.close();

  uint32_t elapsed=millis()-startTime;

  Serial.printf(
    "TARS: RECORD TIME=%lu ms\n",
    (unsigned long)elapsed
  );

  if(!voice||!samples){
    LittleFS.remove(STT_FILE);
    Serial.println("TARS: MIC AUDIO TOO LOW");
    return false;
  }

  Serial.printf(
    "TARS: WAV=%lu BYTES\n",
    (unsigned long)(samples*2)
  );

  return true;
}

/* ================= NTP ================= */

bool syncTime(){
  if(ntpOK)return true;

  configTime(
    7*3600,
    0,
    "pool.ntp.org",
    "time.nist.gov",
    "time.google.com"
  );

  for(int a=1;a<=4;a++){

    Serial.printf(
      "TARS: NTP ATTEMPT %d/4\n",
      a
    );

    for(int i=0;i<20;i++){

      time_t now=time(nullptr);

      if(now>=1704067200){

        struct tm t;
        localtime_r(&now,&t);

        Serial.printf(
          "TARS: NTP VALID %04d-%02d-%02d %02d:%02d:%02d\n",
          t.tm_year+1900,
          t.tm_mon+1,
          t.tm_mday,
          t.tm_hour,
          t.tm_min,
          t.tm_sec
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

/* ================= WIFI ================= */

bool wifiOK(){
  if(WiFi.status()==WL_CONNECTED)
    return true;

  return wifiManagerConnect(false)&&
         WiFi.status()==WL_CONNECTED;
}

bool bootWiFi(){
  Serial.println("TARS: WIFI CONNECTING...");

  uint32_t start=millis();

  while(
    WiFi.status()!=WL_CONNECTED&&
    millis()-start<30000
  ){
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

/* ================= HTTP BODY ================= */

String readHTTPBody(WiFiClientSecure&c){
  String headers;

  uint32_t t=millis();

  while(c.connected()&&millis()-t<10000){

    if(!c.available()){
      delay(1);
      yield();
      continue;
    }

    String line=c.readStringUntil('\n');
    headers+=line;

    if(line=="\r"||line.length()==1)
      break;
  }

  bool chunked=
    headers.indexOf("Transfer-Encoding: chunked")>=0||
    headers.indexOf("transfer-encoding: chunked")>=0;

  int pos=headers.indexOf("Content-Length:");
  if(pos<0)
    pos=headers.indexOf("content-length:");

  String body;

  if(chunked){

    while(c.connected()){

      String line=c.readStringUntil('\n');
      line.trim();

      int n=strtol(line.c_str(),nullptr,16);
      if(n<=0)break;

      while(n>0){

        uint8_t buf[512];
        size_t want=min(n,(int)sizeof(buf));
        size_t got=c.readBytes(buf,want);

        if(!got)break;

        body.concat((char*)buf,got);
        n-=got;
      }

      c.readStringUntil('\n');
    }

  }else if(pos>=0){

    int e=headers.indexOf('\n',pos);

    String s=headers.substring(
      pos+15,
      e
    );

    int len=s.toInt();

    while(len>0){

      uint8_t buf[512];
      size_t want=min(len,(int)sizeof(buf));
      size_t got=c.readBytes(buf,want);

      if(!got)break;

      body.concat((char*)buf,got);
      len-=got;
    }

  }else{

    while(c.connected()||c.available()){

      uint8_t buf[512];
      size_t n=c.read(buf,sizeof(buf));

      if(n)
        body.concat((char*)buf,n);
      else
        delay(1);

      yield();
    }
  }

  return body;
}

/* ================= STT ================= */

String stt(){
  if(!wifiOK())
    return "";

  File f=LittleFS.open(STT_FILE,FILE_READ);
  if(!f)return "";

  const char*b="----TARSSTT";

  String head=
    "--"+String(b)+
    "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"stt.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n";

  String tail=
    "\r\n--"+String(b)+"--\r\n";

  WiFiClientSecure c;
  c.setInsecure();
  c.setTimeout(15000);

  String host=TARS_CLOUD_URL;

  int p=host.indexOf("://");
  if(p>=0)
    host=host.substring(p+3);

  p=host.indexOf('/');
  if(p>=0)
    host=host.substring(0,p);

  uint32_t t0=millis();

  if(!c.connect(host.c_str(),443)){
    f.close();
    Serial.printf(
      "TARS: STT CONNECT FAILED TIME=%lu ms\n",
      (unsigned long)(millis()-t0)
    );
    return "";
  }

  Serial.printf(
    "TARS: STT CONNECT=%lu ms\n",
    (unsigned long)(millis()-t0)
  );

  size_t total=
    head.length()+f.size()+tail.length();

  c.printf(
    "POST /stt HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Content-Type: multipart/form-data; boundary=%s\r\n"
    "Content-Length: %u\r\n"
    "Connection: close\r\n\r\n",
    host.c_str(),
    b,
    (unsigned)total
  );

  c.print(head);

  uint32_t upload=millis();
  uint8_t buf[BUF];

  while(f.available()){

    size_t n=f.read(buf,sizeof(buf));

    if(!n)break;

    if(c.write(buf,n)!=n){
      f.close();
      c.stop();
      return "";
    }

    yield();
  }

  f.close();
  c.print(tail);

  Serial.printf(
    "TARS: STT UPLOAD=%lu ms\n",
    (unsigned long)(millis()-upload)
  );

  uint32_t wait=millis();

  while(
    !c.available()&&
    c.connected()&&
    millis()-wait<20000
  ){
    delay(2);
    yield();
  }

  Serial.printf(
    "TARS: STT WAIT=%lu ms\n",
    (unsigned long)(millis()-wait)
  );

  if(!c.available()){
    c.stop();
    return "";
  }

  String status=c.readStringUntil('\n');
  status.trim();

  Serial.print("TARS: STT HTTP = ");
  Serial.println(status);

  String body=readHTTPBody(c);
  c.stop();

  if(!body.length()||status.indexOf(" 200 ")<0)
    return "";

  JsonDocument j;

  if(deserializeJson(j,body))
    return "";

  String s=j["text"].as<String>();

  if(!s.length())
    s=j["transcript"].as<String>();

  s.trim();

  if(s.length()){
    Serial.print("TARS: YOU SAID = ");
    Serial.println(s);
  }

  return s;
}

/* ================= ASK ================= */

String ask(const String&q){
  if(!wifiOK())
    return "";

  WiFiClientSecure c;
  c.setInsecure();

  HTTPClient h;

  if(!h.begin(
    c,
    String(TARS_CLOUD_URL)+"/ask"
  ))
    return "";

  h.setTimeout(12000);
  h.addHeader(
    "Content-Type",
    "application/json"
  );

  JsonDocument j;
  j["question"]=q;

  String body;
  serializeJson(j,body);

  uint32_t t=millis();
  int code=h.POST(body);

  Serial.printf(
    "TARS: ASK HTTP=%d TIME=%lu ms\n",
    code,
    (unsigned long)(millis()-t)
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

  return s;
}

/* ================= STREAM AUDIO ================= */

bool streamAudio(
  const String&url,
  const String&text
){
  if(!wifiOK())
    return false;

  bool singing=url.endsWith("/sing");

  WiFiClientSecure c;
  c.setInsecure();
  c.setTimeout(15000);

  HTTPClient h;

  uint32_t totalStart=millis();

  if(!h.begin(c,url))
    return false;

  h.setTimeout(60000);
  h.addHeader(
    "Content-Type",
    "application/json"
  );

  JsonDocument j;

  if(singing)
    j["prompt"]=text;
  else
    j["text"]=text;

  String body;
  serializeJson(j,body);

  uint32_t httpStart=millis();
  int code=h.POST(body);

  Serial.printf(
    "TARS: AUDIO HTTP=%d TIME=%lu ms\n",
    code,
    (unsigned long)(millis()-httpStart)
  );

  if(code<200||code>=300){
    h.end();
    return false;
  }

  WiFiClient*stream=h.getStreamPtr();

  if(!stream){
    h.end();
    return false;
  }

  if(!dacOK)
    dacOK=initDAC();

  if(!dacOK){
    h.end();
    return false;
  }

  dec.begin();
  copier.begin(dec,*stream);

  playing=true;

  bool started=false;
  uint32_t streamStart=millis();
  uint32_t firstData=0;
  uint32_t lastData=millis();

  Serial.println("TARS: AUDIO STREAM START");

  while(
    h.connected()||
    stream->available()
  ){

    size_t avail=stream->available();
    bool copied=copier.copy();

    if(!started&&(copied||avail>0)){

      started=true;
      firstData=millis()-streamStart;
      lastData=millis();

      Serial.printf(
        "TARS: AUDIO FIRST DATA=%lu ms\n",
        (unsigned long)firstData
      );

      oledStartSpeak(text);
    }

    if(copied)
      lastData=millis();

    /*
       Jangan gunakan timeout 10 detik di sini.
       HTTP connection boleh tetap hidup sementara decoder
       menunggu buffer/data berikutnya.
    */
    if(
      started&&
      !h.connected()&&
      !stream->available()
    )
      break;

    yield();
  }

  uint32_t streamTime=millis()-streamStart;

  dec.end();
  h.end();

  playing=false;

  Serial.printf(
    "TARS: AUDIO STREAM=%lu ms\n",
    (unsigned long)streamTime
  );

  Serial.printf(
    "TARS: AUDIO TOTAL=%lu ms\n",
    (unsigned long)(millis()-totalStart)
  );

  oledSetListening();

  return started;
}

/* ================= SING ================= */

bool singRequest(String s){
  s.toLowerCase();

  return
    s.indexOf("nyanyi")>=0||
    s.indexOf("bernyanyi")>=0||
    s.indexOf("nyanyikan")>=0;
}

/* ================= PROCESS ================= */

void processQuestion(const String&q){

  uint32_t total=millis();

  String answer=ask(q);

  if(!answer.length()){
    oledSetStatus("ASK ERROR");
    return;
  }

  singMode=singRequest(q);

  String url=
    String(TARS_CLOUD_URL)+
    (singMode?"/sing":"/tts");

  String payload=
    singMode?q:answer;

  uint32_t audioStart=millis();

  bool ok=streamAudio(
    url,
    payload
  );

  Serial.printf(
    "TARS: AUDIO FUNCTION TIME=%lu ms\n",
    (unsigned long)(millis()-audioStart)
  );

  Serial.printf(
    "TARS: TOTAL=%lu ms\n",
    (unsigned long)(millis()-total)
  );

  if(!ok)
    oledSetStatus("AUDIO ERROR");

  singMode=false;
}

/* ================= SETUP ================= */

void setup(){

  Serial.begin(SERIAL_BAUD);

  Wire.begin(
    OLED_SDA,
    OLED_SCL
  );

  Wire.setClock(400000);

  oledOK=oled.begin(
    SSD1306_SWITCHCAPVCC,
    OLED_ADDR
  );

  if(oledOK){
    oledHeader();
    oled.setCursor(3,27);
    oled.print("BOOT");
    oled.display();
  }

  LittleFS.begin(true);

  dacOK=initDAC();
  micOK=initMic();

  Serial.printf(
    "TARS: DAC=%s MIC=%s\n",
    dacOK?"READY":"ERROR",
    micOK?"READY":"ERROR"
  );

  Serial.println("TARS: PAM RIGHT GPIO26");
  Serial.println("TARS: INMP441 RIGHT GPIO34");
  Serial.println("TARS: BLUETOOTH DISABLED");
  Serial.println("TARS: MP3 STREAMING ENABLED");

  if(oledOK){
    xTaskCreatePinnedToCore(
      oledTask,
      "TARS_OLED",
      4096,
      nullptr,
      1,
      nullptr,
      0
    );
  }

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

  if(recordMic()){

    String q=stt();

    LittleFS.remove(STT_FILE);

    if(q.length())
      processQuestion(q);
    else
      oledSetStatus("NO INPUT");

  }else{
    oledSetListening();
  }

  delay(1);
}
