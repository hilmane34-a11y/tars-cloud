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
const uint32_t RECORD_MAX_MS=5000,RECORD_MIN_MS=500;
const uint32_t SILENCE_MS=800,LISTEN_MAX_MS=8000,PREROLL_MS=600;
const int32_t MIC_THRESHOLD=14000,MIC_SILENCE=12000;
const uint32_t OLED_TYPE_MS=4;
const size_t BUF=2048,DAC_BUF=16384;
const size_t PREROLL_SAMPLES=MIC_RATE*PREROLL_MS/1000;
static const char* STT_FILE="/stt.wav";

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);
bool oledOK=false,micOK=false,dacOK=false,playing=false,singMode=false,ntpOK=false;
String oledText;
size_t oledPos=0;
uint32_t oledTick=0,dotTick=0;
uint8_t dotState=1;

void oledHeader(const char*t){
  oled.clearDisplay();oled.setTextColor(SSD1306_WHITE);oled.setTextSize(1);
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
  if(!oledOK||millis()-dotTick<300)return;
  dotTick=millis();dotState=dotState>=4?1:dotState+1;
  String s;for(uint8_t i=0;i<dotState;i++)s+='.';
  oledBase("LISTENING",s);
}
void oledType(){
  if(!oledOK||!oledText.length()||millis()-oledTick<OLED_TYPE_MS)return;
  oledTick=millis();
  if(oledPos>=oledText.length())return;

  const char*title=singMode?"SINGING":"SPEAKING";
  oledHeader(title);

  int x=3,y=27;
  for(size_t i=0;i<=oledPos;i++){
    char ch=oledText[i];
    if(ch=='\n'||x>121){
      x=3;y+=8;
      if(y>59){oledHeader(title);x=3;y=27;}
      if(ch=='\n')continue;
    }
    oled.setCursor(x,y);oled.write(ch);x=oled.getCursorX();
  }

  oledPos++;
  oled.display();
}

bool initDAC(){
  pinMode(AUDIO_DAC_PIN,OUTPUT);
  dacWrite(AUDIO_DAC_PIN,0);
  Serial.println("TARS: DIRECT DAC GPIO26 READY");
  return true;
}
inline void dacMute(){dacWrite(AUDIO_DAC_PIN,0);}
inline void dacCenter(){dacWrite(AUDIO_DAC_PIN,128);}

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
    I2S_PIN_NO_CHANGE,
    MIC_SCK,
    MIC_WS,
    I2S_PIN_NO_CHANGE,
    MIC_SD
  };

  return i2s_set_pin(MIC_PORT,&p)==ESP_OK;
}

void put16(uint8_t*p,uint16_t v){p[0]=v;p[1]=v>>8;}
void put32(uint8_t*p,uint32_t v){
  p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;
}
void wavHeader(File&f,uint32_t n){
  uint8_t h[44]={};
  memcpy(h,"RIFF",4);put32(h+4,n+36);
  memcpy(h+8,"WAVEfmt ",8);put32(h+16,16);
  put16(h+20,1);put16(h+22,1);
  put32(h+24,MIC_RATE);put32(h+28,MIC_RATE*2);
  put16(h+32,2);put16(h+34,16);
  memcpy(h+36,"data",4);put32(h+40,n);
  f.seek(0);f.write(h,44);
}

bool recordMic(){
  if(!micOK)return false;

  Serial.println("TARS: RECORDING");
  oledBase("LISTENING",".");

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

  uint32_t samples=0,reads=0,errors=0;
  uint32_t listenStart=millis();
  uint32_t voiceStart=0,lastVoice=0;

  bool voiceStarted=false;

  int32_t peak=0,minV=32767,maxV=-32768;

  while(
    (!voiceStarted&&millis()-listenStart<LISTEN_MAX_MS)||
    (voiceStarted&&millis()-voiceStart<RECORD_MAX_MS)
  ){
    oledListening();

    size_t n=0;
    esp_err_t err=i2s_read(
      MIC_PORT,
      raw,
      sizeof(raw),
      &n,
      pdMS_TO_TICKS(50)
    );

    if(err!=ESP_OK){
      errors++;
      continue;
    }

    reads++;

    size_t count=n/4;
    int32_t blockPeak=0;

    for(size_t i=0;i<count;i++){
      int32_t v=constrain(
        raw[i]>>16,
        (int32_t)-32768,
        (int32_t)32767
      );

      pcm[i]=(int16_t)v;

      int32_t av=abs(v);
      blockPeak=max(blockPeak,av);
      peak=max(peak,av);
      minV=min(minV,v);
      maxV=max(maxV,v);
    }

    if(!voiceStarted){

      for(size_t i=0;i<count;i++){
        pre[prePos]=pcm[i];
        prePos=(prePos+1)%PREROLL_SAMPLES;
        if(preCount<PREROLL_SAMPLES)
          preCount++;
      }

      // Cepat: satu blok suara cukup untuk mulai merekam.
      if(blockPeak>=MIC_THRESHOLD){

        voiceStarted=true;
        voiceStart=millis();
        lastVoice=voiceStart;

        Serial.println("TARS: VOICE DETECTED");

        size_t start=
          preCount==PREROLL_SAMPLES?
          prePos:0;

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
      }

    }else{

      f.write(
        (uint8_t*)pcm,
        count*2
      );

      samples+=count;

      if(blockPeak>=MIC_SILENCE)
        lastVoice=millis();

      // Selesai 350 ms setelah suara berhenti.
      if(
        millis()-voiceStart>=RECORD_MIN_MS&&
        millis()-lastVoice>=SILENCE_MS
      ){
        break;
      }
    }

    yield();
  }

  uint32_t rt=millis()-listenStart;

  wavHeader(f,samples*2);
  f.close();

  Serial.printf(
    "TARS: RECORD TIME=%lu ms\r\n",
    (unsigned long)rt
  );

  Serial.printf(
    "TARS: MIC READ=%lu ERROR=%lu SAMPLES=%lu\r\n",
    (unsigned long)reads,
    (unsigned long)errors,
    (unsigned long)samples
  );

  Serial.printf(
    "TARS: MIC MIN=%ld MAX=%ld PEAK=%ld\r\n",
    (long)minV,
    (long)maxV,
    (long)peak
  );

  if(!voiceStarted||samples==0){
    Serial.println("TARS: MIC AUDIO TOO LOW");
    return false;
  }

  Serial.println("TARS: MIC AUDIO OK");
  return true;
}

bool syncTime(){
  configTime(
    7*3600,
    0,
    "pool.ntp.org",
    "time.nist.gov",
    "time.google.com"
  );

  for(int a=1;a<=4;a++){

    Serial.printf(
      "TARS: NTP SYNC %d/4\r\n",
      a
    );

    for(int i=0;i<20;i++){

      time_t now=time(nullptr);

      if(now>=1704067200){

        struct tm t;
        localtime_r(&now,&t);

        Serial.printf(
          "TARS: NTP VALID = %04d-%02d-%02d %02d:%02d:%02d\r\n",
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

  ntpOK=false;
  Serial.println("TARS: NTP FAILED AFTER 4 ATTEMPTS");
  return false;
}

bool wifiOK(){
  if(WiFi.status()!=WL_CONNECTED)
    if(!wifiManagerConnect(false))
      return false;

  return WiFi.status()==WL_CONNECTED;
}

String readHTTPBody(WiFiClientSecure&c){
  String line,body;
  bool chunked=false;
  int len=-1;

  while(c.connected()){

    line=c.readStringUntil('\n');
    line.trim();

    if(!line.length())
      break;

    String low=line;
    low.toLowerCase();

    if(low.startsWith("content-length:"))
      len=low.substring(15).toInt();

    if(
      low.indexOf("transfer-encoding:")>=0&&
      low.indexOf("chunked")>=0
    )
      chunked=true;
  }

  if(chunked){

    while(c.connected()){

      line=c.readStringUntil('\n');
      line.trim();

      if(!line.length())
        continue;

      int n=strtol(
        line.c_str(),
        nullptr,
        16
      );

      if(n<=0){
        c.readStringUntil('\n');
        break;
      }

      while(n>0){

        uint8_t b[BUF];

        size_t w=min(
          (int)sizeof(b),
          n
        );

        size_t r=c.readBytes(b,w);

        if(!r)
          break;

        body.concat(
          (const char*)b,
          r
        );

        n-=r;
      }

      c.readStringUntil('\n');
    }

  }else if(len>=0){

    while(
      (int)body.length()<len&&
      c.connected()
    ){

      uint8_t b[BUF];

      int rem=len-body.length();

      size_t w=min(
        (int)sizeof(b),
        rem
      );

      size_t r=c.readBytes(b,w);

      if(!r)
        break;

      body.concat(
        (const char*)b,
        r
      );
    }

  }else{
    body=c.readString();
  }

  return body;
}

String stt(){
  if(!wifiOK())return "";

  File f=LittleFS.open(
    STT_FILE,
    FILE_READ
  );

  if(!f)return "";

  const char*b="----TARSSTT";

  String a=
    "--"+String(b)+
    "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"stt.wav\""+
    "\r\nContent-Type: audio/wav\r\n\r\n";

  String e=
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

  Serial.println("TARS: STT CONNECTING");

  if(!c.connect(host.c_str(),443)){
    f.close();
    return "";
  }

  size_t total=
    a.length()+
    f.size()+
    e.length();

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

  c.print(a);

  uint8_t buf[BUF];

  while(f.available()){

    size_t n=f.read(
      buf,
      sizeof(buf)
    );

    if(!n)
      break;

    if(c.write(buf,n)!=n){
      f.close();
      c.stop();
      return "";
    }

    yield();
  }

  f.close();
  c.print(e);

  Serial.println("TARS: STT WAITING");

  uint32_t t=millis();

  while(
    !c.available()&&
    c.connected()&&
    millis()-t<20000
  ){
    delay(2);
    yield();
  }

  if(!c.available()){
    c.stop();
    return "";
  }

  String status=
    c.readStringUntil('\n');

  status.trim();

  Serial.print("TARS: STT HTTP = ");
  Serial.println(status);

  String body=readHTTPBody(c);

  c.stop();

  if(
    !body.length()||
    status.indexOf(" 200 ")<0
  )
    return "";

  JsonDocument j;

  if(deserializeJson(j,body))
    return "";

  String s=
    j["text"].as<String>();

  if(!s.length())
    s=j["transcript"].as<String>();

  s.trim();

  if(s.length()){
    Serial.print("TARS: YOU SAID = ");
    Serial.println(s);
  }

  return s;
}

String ask(const String&q){
  if(!wifiOK())return "";

  Serial.print("TARS: ASK = ");
  Serial.println(q);

  oledBase(
    "PROCESSING",
    "ANALYZING..."
  );

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

  String s=
    x["response"].as<String>();

  s.trim();

  Serial.print("TARS: ANSWER = ");
  Serial.println(s);

  return s;
}

/*
 * STREAMING TTS
 * Tidak menyimpan MP3 ke LittleFS.
 * HTTP -> MP3 decoder -> DAC langsung.
 */
bool streamAudio(
  const String&url,
  const String&text
){
  if(!wifiOK())
    return false;

  bool singing=
    url.endsWith("/sing");

  Serial.println(
    singing?
    "TARS: SING STREAM":
    "TARS: TTS STREAM"
  );

  WiFiClientSecure c;
  c.setInsecure();

  HTTPClient h;

  if(!h.begin(c,url))
    return false;

  h.setTimeout(90000);

  h.addHeader(
    "Content-Type",
    "application/json"
  );

  JsonDocument j;

  if(singing)
    j["prompt"]=text;
  else
    j["text"]=text;

  String requestBody;
  serializeJson(
    j,
    requestBody
  );

  int code=h.POST(requestBody);

  Serial.printf(
    "TARS: AUDIO HTTP = %d\r\n",
    code
  );

  if(code<200||code>=300){
    h.end();
    return false;
  }

  WiFiClient*stream=
    h.getStreamPtr();

  if(!stream){
    h.end();
    return false;
  }

  playing=true;
  oledPos=0;
  oledTick=millis();

  dacOut.resetStats();

  if(!dacOK)
    dacOK=initDAC();

  if(!dacOK){
    h.end();
    playing=false;
    return false;
  }

  if(!mp3.begin()){
    Serial.println(
      "TARS: MP3 DECODER ERROR"
    );

    h.end();
    playing=false;
    return false;
  }

  oledBase(
    singing?
    "SINGING":
    "SPEAKING"
  );

  dacOut.startDAC();

  StreamCopy copy(
    mp3,
    *stream,
    BUF
  );

  uint32_t start=millis();
  uint32_t lastData=millis();

  Serial.println(
    "TARS: AUDIO STREAM START"
  );

  while(
    h.connected()&&
    millis()-start<120000
  ){

    size_t available=
      stream->available();

    if(available){

      size_t before=available;

      if(copy.copy()){

        lastData=millis();

      }else if(before==0){

        delay(1);
      }

    }else{

      if(
        millis()-lastData>10000
      ){
        Serial.println(
          "TARS: AUDIO STREAM TIMEOUT"
        );
        break;
      }

      delay(1);
    }

    oledType();
    yield();
  }

  Serial.println(
    "TARS: AUDIO STREAM END"
  );

  mp3.end();

  h.end();

  dacOut.waitDrain();
  dacOut.stopDAC();

  playing=false;
  dacMute();

  dacOut.printStats();

  oledBase(
    "READY",
    "WAITING..."
  );

  Serial.println(
    "TARS: PLAYBACK DONE"
  );

  return true;
}

bool singRequest(String s){
  s.toLowerCase();

  return
    s.indexOf("nyanyi")>=0||
    s.indexOf("bernyanyi")>=0||
    s.indexOf("nyanyikan")>=0;
}

void processQuestion(const String&q){

  uint32_t total=millis();

  uint32_t t=millis();

  String answer=ask(q);

  Serial.printf(
    "TARS: ASK TIME=%lu ms\r\n",
    (unsigned long)(
      millis()-t
    )
  );

  if(!answer.length()){
    oledBase(
      "READY",
      "ASK ERROR"
    );
    return;
  }

  oledText=answer;
  oledPos=0;
  singMode=singRequest(q);

  String url=
    String(TARS_CLOUD_URL)+
    (singMode?
      "/sing":
      "/tts");

  t=millis();

  if(streamAudio(
    url,
    singMode?
      q:
      answer
  )){

    Serial.printf(
      "TARS: TTS TIME=%lu ms\r\n",
      (unsigned long)(
        millis()-t
      )
    );

  }else{

    oledBase(
      "READY",
      "AUDIO ERROR"
    );
  }

  Serial.printf(
    "TARS: TOTAL PROCESS=%lu ms\r\n",
    (unsigned long)(
      millis()-total
    )
  );

  singMode=false;
}

void setup(){

  Serial.begin(
    SERIAL_BAUD
  );

  Wire.begin(
    OLED_SDA,
    OLED_SCL
  );

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
    dacOK?
    "READY":
    "ERROR"
  );

  Serial.printf(
    "TARS: MIC %s\r\n",
    micOK?
    "READY":
    "ERROR"
  );

  Serial.printf(
    "TARS: MIC THRESHOLD=%ld MIC SILENCE=%ld END SILENCE=%lu ms\r\n",
    (long)MIC_THRESHOLD,
    (long)MIC_SILENCE,
    (unsigned long)SILENCE_MS
  );

  Serial.println(
    "TARS: DIRECT AUDIO GPIO26"
  );

  Serial.println(
    "TARS: BLUETOOTH DISABLED"
  );

  Serial.println(
    "TARS: STREAMING AUDIO ENABLED"
  );

  Serial.println(
    "TARS: POWER ON BOOT"
  );

  wifiManagerBegin();

  if(WiFi.status()==WL_CONNECTED){

    Serial.println(
      "TARS: WIFI CONNECTED"
    );

    if(syncTime())
      Serial.println(
        "TARS: WIFI + NTP READY"
      );
    else
      Serial.println(
        "TARS: WIFI READY, NTP FAILED"
      );

  }else{

    Serial.println(
      "TARS: WIFI NOT CONNECTED"
    );
  }

  oledBase(
    "READY",
    "WAITING..."
  );
}

void loop(){

  if(playing){
    delay(1);
    return;
  }

  if(WiFi.status()!=WL_CONNECTED){

    if(!wifiOK()){

      oledBase(
        "READY",
        "WIFI ERROR"
      );

      delay(500);
      return;
    }
  }

  if(recordMic()){

    String q=stt();

    LittleFS.remove(
      STT_FILE
    );

    if(q.length())
      processQuestion(q);
    else
      oledBase(
        "READY",
        "NO INPUT"
      );

  }else{

    oledListening();
  }

  delay(1);
}
