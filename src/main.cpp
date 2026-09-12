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

#define DAC_PORT I2S_NUM_0
#define MIC_PORT I2S_NUM_1
#define MIC_SCK 18
#define MIC_WS 19
#define MIC_SD 34

const uint32_t MIC_RATE=16000;
const uint32_t PLAY_RATE=22050;
const uint32_t RECORD_MS=4000;
const size_t BUF=1024;
const int32_t MIC_THRESHOLD=2500;

static const char *STT_FILE="/stt.wav";

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);

bool oledOK=false,micOK=false,dacOK=false;
bool playing=false,singMode=false,ntpOK=false;

String oledText;
size_t oledPos=0;
uint32_t oledTick=0,dotTick=0;
uint8_t dotState=1;

// ============================================================
// OLED
// ============================================================

void oledBase(const char *title,const String &text=""){
  if(!oledOK)return;

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(42,0);
  oled.print("T A R S");
  oled.drawLine(0,9,127,9,SSD1306_WHITE);
  oled.setCursor(3,14);
  oled.print(title);

  if(text.length()){
    oled.setCursor(3,27);
    oled.print(text);
  }

  oled.display();
}

void oledListening(){
  if(!oledOK||millis()-dotTick<350)return;

  dotTick=millis();
  dotState=dotState>=4?1:dotState+1;

  String d;
  for(uint8_t i=0;i<dotState;i++)d+=".";

  oledBase("LISTENING",d);
}

void oledType(){
  if(!oledOK||!oledText.length()||millis()-oledTick<65)return;

  oledTick=millis();
  if(oledPos<oledText.length())oledPos++;

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(42,0);
  oled.print("T A R S");
  oled.drawLine(0,9,127,9,SSD1306_WHITE);
  oled.setCursor(3,14);
  oled.print(playing?(singMode?"SINGING":"SPEAKING"):"READY");
  oled.setCursor(3,27);

  for(size_t i=0;i<oledPos;i++)
    oled.print(oledText[i]);

  oled.display();
}

// ============================================================
// DAC GPIO25/26
// PCM 16-bit -> internal DAC 8-bit
// ============================================================

bool initDAC(){
  i2s_config_t c={};

  c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_TX|I2S_MODE_DAC_BUILT_IN);
  c.sample_rate=PLAY_RATE;
  c.bits_per_sample=I2S_BITS_PER_SAMPLE_16BIT;
  c.channel_format=I2S_CHANNEL_FMT_RIGHT_LEFT;
  c.communication_format=I2S_COMM_FORMAT_I2S_MSB;
  c.dma_buf_count=4;
  c.dma_buf_len=256;
  c.tx_desc_auto_clear=true;

  esp_err_t e=i2s_driver_install(DAC_PORT,&c,0,nullptr);

  if(e!=ESP_OK){
    Serial.printf("TARS: DAC DRIVER ERROR=%d\r\n",e);
    return false;
  }

  e=i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN);

  if(e!=ESP_OK){
    Serial.printf("TARS: DAC MODE ERROR=%d\r\n",e);
    i2s_driver_uninstall(DAC_PORT);
    return false;
  }

  i2s_zero_dma_buffer(DAC_PORT);
  Serial.println("TARS: DAC GPIO25/26 READY");
  return true;
}

// ============================================================
// DAC DIRECT TEST
// ============================================================

void testDAC(){
  Serial.println("TARS: DAC DIRECT TEST");

  const int sampleRate=22050;
  const int freq=1000;
  const int samples=256;

  uint16_t buf[samples*2];

  for(int i=0;i<samples;i++){
    float s=sin(2.0f*PI*freq*i/sampleRate);
    uint8_t u=(uint8_t)(128.0f+100.0f*s);
    uint16_t v=(uint16_t)u<<8;

    buf[i*2]=v;       // GPIO25
    buf[i*2+1]=v;     // GPIO26
  }

  for(int n=0;n<100;n++){
    size_t written=0;

    i2s_write(
      DAC_PORT,
      buf,
      sizeof(buf),
      &written,
      portMAX_DELAY
    );
  }

  i2s_zero_dma_buffer(DAC_PORT);

  Serial.println("TARS: DAC DIRECT TEST DONE");
}

// ============================================================
// INMP441
// ============================================================

bool initMic(){
  i2s_config_t c={};

  c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_RX);
  c.sample_rate=MIC_RATE;
  c.bits_per_sample=I2S_BITS_PER_SAMPLE_32BIT;
  c.channel_format=I2S_CHANNEL_FMT_ONLY_RIGHT;
  c.communication_format=I2S_COMM_FORMAT_I2S;
  c.dma_buf_count=2;
  c.dma_buf_len=256;

  if(i2s_driver_install(MIC_PORT,&c,0,nullptr)!=ESP_OK)
    return false;

  i2s_pin_config_t p={};
  p.mck_io_num=I2S_PIN_NO_CHANGE;
  p.bck_io_num=MIC_SCK;
  p.ws_io_num=MIC_WS;
  p.data_out_num=I2S_PIN_NO_CHANGE;
  p.data_in_num=MIC_SD;

  return i2s_set_pin(MIC_PORT,&p)==ESP_OK;
}

// ============================================================
// WAV
// ============================================================

void put16(uint8_t *p,uint16_t v){
  p[0]=v;
  p[1]=v>>8;
}

void put32(uint8_t *p,uint32_t v){
  p[0]=v;
  p[1]=v>>8;
  p[2]=v>>16;
  p[3]=v>>24;
}

void wavHeader(File &f,uint32_t n){
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

// ============================================================
// RECORD MIC
// ============================================================

bool recordMic(){
  if(!micOK){
    Serial.println("TARS: MIC NOT READY");
    return false;
  }

  dotState=1;
  dotTick=millis();
  oledBase("LISTENING",".");

  Serial.println("TARS: RECORDING");

  if(LittleFS.exists(STT_FILE))
    LittleFS.remove(STT_FILE);

  File f=LittleFS.open(STT_FILE,FILE_WRITE);

  if(!f){
    Serial.println("TARS: WAV OPEN ERROR");
    return false;
  }

  uint8_t z[44]={};
  f.write(z,44);

  int32_t raw[BUF/4];
  int16_t pcm[BUF/4];

  uint32_t samples=0,reads=0,errors=0,start=millis();
  int32_t peak=0,minV=32767,maxV=-32768;

  while(millis()-start<RECORD_MS){
    oledListening();

    size_t n=0;
    esp_err_t err=i2s_read(
      MIC_PORT,raw,sizeof(raw),&n,pdMS_TO_TICKS(100)
    );

    if(err!=ESP_OK){
      errors++;
      continue;
    }

    reads++;
    size_t count=n/sizeof(int32_t);

    for(size_t i=0;i<count;i++){
      int32_t v=raw[i]>>16;

      v=max((int32_t)-32768,min((int32_t)32767,v));
      pcm[i]=(int16_t)v;

      int32_t av=abs(v);
      if(av>peak)peak=av;
      if(v<minV)minV=v;
      if(v>maxV)maxV=v;
    }

    if(count){
      f.write((uint8_t*)pcm,count*2);
      samples+=count;
    }

    yield();
  }

  wavHeader(f,samples*2);
  f.close();

  Serial.printf(
    "TARS: MIC READ=%lu ERROR=%lu SAMPLES=%lu\r\n",
    (unsigned long)reads,
    (unsigned long)errors,
    (unsigned long)samples
  );

  Serial.printf(
    "TARS: MIC MIN=%ld MAX=%ld PEAK=%ld\r\n",
    (long)minV,(long)maxV,(long)peak
  );

  if(peak<MIC_THRESHOLD){
    Serial.printf(
      "TARS: MIC AUDIO TOO LOW (<%ld)\r\n",
      (long)MIC_THRESHOLD
    );
    return false;
  }

  Serial.println("TARS: MIC AUDIO OK");
  return samples>0;
}

// ============================================================
// NTP
// ============================================================

bool syncTime(){
  configTime(
    7*3600,0,
    "pool.ntp.org",
    "time.nist.gov",
    "time.google.com"
  );

  for(int attempt=1;attempt<=4;attempt++){
    Serial.printf("TARS: NTP SYNC %d/4\r\n",attempt);

    for(int i=0;i<20;i++){
      time_t now=time(nullptr);

      if(now>=1704067200){
        struct tm t;
        localtime_r(&now,&t);

        Serial.printf(
          "TARS: NTP VALID = %04d-%02d-%02d %02d:%02d:%02d\r\n",
          t.tm_year+1900,t.tm_mon+1,t.tm_mday,
          t.tm_hour,t.tm_min,t.tm_sec
        );

        ntpOK=true;
        return true;
      }

      delay(500);
    }

    if(attempt<4)
      Serial.println("TARS: NTP INVALID, RETRY");
  }

  ntpOK=false;
  Serial.println("TARS: NTP FAILED AFTER 4 ATTEMPTS");
  return false;
}

// ============================================================
// WIFI
// ============================================================

bool wifiOK(){
  if(WiFi.status()!=WL_CONNECTED){
    if(!wifiManagerConnect(false))
      return false;
  }

  if(!ntpOK){
    if(!syncTime())
      return false;
  }

  return true;
}

// ============================================================
// HTTP BODY
// ============================================================

String readHTTPBody(WiFiClientSecure &c){
  String line,body;
  bool chunked=false;
  int contentLength=-1;

  while(c.connected()){
    line=c.readStringUntil('\n');
    line.trim();

    if(!line.length())break;

    String low=line;
    low.toLowerCase();

    if(low.startsWith("content-length:"))
      contentLength=low.substring(15).toInt();

    if(low.indexOf("transfer-encoding:")>=0&&
       low.indexOf("chunked")>=0)
      chunked=true;
  }

  Serial.printf(
    "TARS: STT TRANSFER=%s LENGTH=%d\r\n",
    chunked?"CHUNKED":"NORMAL",
    contentLength
  );

  if(chunked){
    while(c.connected()){
      line=c.readStringUntil('\n');
      line.trim();

      if(!line.length())continue;

      int size=(int)strtol(line.c_str(),nullptr,16);

      if(size<=0){
        c.readStringUntil('\n');
        break;
      }

      while(size>0){
        uint8_t buf[BUF];
        size_t want=min((int)sizeof(buf),size);
        size_t n=c.readBytes(buf,want);

        if(!n)break;

        body.concat((const char*)buf,n);
        size-=n;
      }

      c.readStringUntil('\n');
    }

  }else if(contentLength>=0){
    while((int)body.length()<contentLength&&c.connected()){
      uint8_t buf[BUF];

      int remain=contentLength-body.length();
      size_t want=min((int)sizeof(buf),remain);
      size_t n=c.readBytes(buf,want);

      if(!n)break;

      body.concat((const char*)buf,n);
    }

  }else{
    body=c.readString();
  }

  return body;
}

// ============================================================
// STT
// ============================================================

String stt(){
  if(!wifiOK())return "";

  File f=LittleFS.open(STT_FILE,FILE_READ);

  if(!f){
    Serial.println("TARS: STT WAV OPEN ERROR");
    return "";
  }

  const char *b="----TARSSTT";

  String a=
    "--"+String(b)+
    "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"stt.wav\""
    "\r\nContent-Type: audio/wav\r\n\r\n";

  String e="\r\n--"+String(b)+"--\r\n";

  WiFiClientSecure c;
  c.setInsecure();
  c.setTimeout(15000);

  String host=TARS_CLOUD_URL;
  int proto=host.indexOf("://");

  if(proto>=0)host=host.substring(proto+3);

  int slash=host.indexOf('/');
  if(slash>=0)host=host.substring(0,slash);

  Serial.println("TARS: STT CONNECTING");

  if(!c.connect(host.c_str(),443)){
    Serial.println("TARS: STT CONNECT ERROR");
    f.close();
    return "";
  }

  size_t total=a.length()+f.size()+e.length();

  c.printf(
    "POST /stt HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Content-Type: multipart/form-data; boundary=%s\r\n"
    "Content-Length: %u\r\n"
    "Connection: close\r\n\r\n",
    host.c_str(),b,(unsigned)total
  );

  c.print(a);

  uint8_t buf[BUF];

  while(f.available()){
    size_t n=f.read(buf,sizeof(buf));

    if(!n)break;

    if(c.write(buf,n)!=n){
      Serial.println("TARS: STT UPLOAD ERROR");
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

  while(!c.available()&&c.connected()&&millis()-t<20000){
    delay(5);
    yield();
  }

  if(!c.available()){
    Serial.println("TARS: STT TIMEOUT");
    c.stop();
    return "";
  }

  String status=c.readStringUntil('\n');
  status.trim();

  Serial.print("TARS: STT HTTP = ");
  Serial.println(status);

  String body=readHTTPBody(c);
  c.stop();

  if(!body.length()){
    Serial.println("TARS: STT EMPTY RESPONSE");
    return "";
  }

  Serial.print("TARS: STT BODY = ");
  Serial.println(body);

  if(status.indexOf(" 200 ")<0){
    Serial.println("TARS: STT HTTP ERROR");
    return "";
  }

  JsonDocument j;

  if(deserializeJson(j,body)){
    Serial.println("TARS: STT JSON ERROR");
    return "";
  }

  String s;

  if(j["text"].is<const char*>())
    s=j["text"].as<const char*>();

  if(!s.length()&&j["transcript"].is<const char*>())
    s=j["transcript"].as<const char*>();

  s.trim();

  if(s.length()){
    Serial.print("TARS: YOU SAID = ");
    Serial.println(s);
  }else{
    Serial.println("TARS: STT NO TEXT");
  }

  return s;
}

// ============================================================
// ASK
// ============================================================

String ask(const String &q){
  if(!wifiOK())return "";

  Serial.print("TARS: ASK = ");
  Serial.println(q);

  oledBase("PROCESSING","ANALYZING...");

  WiFiClientSecure c;
  c.setInsecure();

  HTTPClient h;

  if(!h.begin(c,String(TARS_CLOUD_URL)+"/ask")){
    Serial.println("TARS: ASK BEGIN ERROR");
    return "";
  }

  h.setTimeout(30000);
  h.addHeader("Content-Type","application/json");

  JsonDocument j;
  j["text"]=q;

  String b;
  serializeJson(j,b);

  int code=h.POST(b);

  Serial.printf("TARS: ASK HTTP = %d\r\n",code);

  if(code<200||code>=300){
    h.end();
    return "";
  }

  String r=h.getString();
  h.end();

  JsonDocument x;

  if(deserializeJson(x,r)){
    Serial.println("TARS: ASK JSON ERROR");
    return "";
  }

  String s=x["response"].as<String>();
  s.trim();

  Serial.print("TARS: ANSWER = ");
  Serial.println(s);

  return s;
}

// ============================================================
// TTS / SING DOWNLOAD
// ============================================================

bool downloadMP3(const String &url,const String &text){
  if(!wifiOK())return false;

  Serial.println(
    url.endsWith("/sing")?
    "TARS: SING":
    "TARS: TTS"
  );

  WiFiClientSecure c;
  c.setInsecure();

  HTTPClient h;

  if(!h.begin(c,url)){
    Serial.println("TARS: AUDIO BEGIN ERROR");
    return false;
  }

  h.setTimeout(60000);
  h.addHeader("Content-Type","application/json");

  JsonDocument j;
  j["text"]=text;

  String b;
  serializeJson(j,b);

  int code=h.POST(b);

  Serial.printf("TARS: AUDIO HTTP = %d\r\n",code);

  if(code<200||code>=300){
    h.end();
    return false;
  }

  if(LittleFS.exists(MP3_FILE))
    LittleFS.remove(MP3_FILE);

  File f=LittleFS.open(MP3_FILE,FILE_WRITE);

  if(!f){
    Serial.println("TARS: MP3 FILE ERROR");
    h.end();
    return false;
  }

  WiFiClient *s=h.getStreamPtr();
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

        if(len>0)len-=r;

        t=millis();
      }
    }else{
      if(millis()-t>5000)break;
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

// ============================================================
// DAC OUTPUT
// ============================================================

class DACOut:public AudioStream{
  AudioInfo info;
  uint16_t buffer[BUF/2];

  uint32_t calls=0;
  uint32_t pcmBytes=0;
  uint32_t outBytes=0;
  uint32_t errors=0;
  int32_t peak=0;

public:

  void resetStats(){
    calls=0;
    pcmBytes=0;
    outBytes=0;
    errors=0;
    peak=0;
  }

  void printStats(){
    Serial.printf(
      "TARS: DAC CALL=%lu PCM=%lu OUT=%lu PEAK=%ld ERROR=%lu\r\n",
      (unsigned long)calls,
      (unsigned long)pcmBytes,
      (unsigned long)outBytes,
      (long)peak,
      (unsigned long)errors
    );
  }

  void setAudioInfo(AudioInfo i)override{
    info=i;
    AudioStream::setAudioInfo(i);

    Serial.printf(
      "TARS: DAC AUDIO %uHz %ubit %uch\r\n",
      info.sample_rate,
      info.bits_per_sample,
      info.channels
    );
  }

  int availableForWrite()override{
    return BUF;
  }

  size_t write(const uint8_t *d,size_t n)override{
    calls++;
    pcmBytes+=n;

    if(!dacOK||!d||!n||info.bits_per_sample!=16||!info.channels){
      errors++;
      return 0;
    }

    size_t frames=n/(info.channels*2);

    if(frames>BUF/4)
      frames=BUF/4;

    for(size_t i=0;i<frames;i++){
      int32_t v;

      if(info.channels==1){
        v=(int16_t)(
          d[i*2]|
          ((uint16_t)d[i*2+1]<<8)
        );
      }else{
        int16_t l=(int16_t)(
          d[i*4]|
          ((uint16_t)d[i*4+1]<<8)
        );

        int16_t r=(int16_t)(
          d[i*4+2]|
          ((uint16_t)d[i*4+3]<<8)
        );

        v=((int32_t)l+(int32_t)r)/2;
      }

      int32_t av=abs(v);
      if(av>peak)peak=av;

      int32_t u=(v+32768)>>8;

      if(u<0)u=0;
      if(u>255)u=255;

      uint16_t sample=(uint16_t)(u<<8);

      buffer[i*2]=sample;
      buffer[i*2+1]=sample;
    }

    size_t written=0;

    esp_err_t e=i2s_write(
      DAC_PORT,
      buffer,
      frames*4,
      &written,
      portMAX_DELAY
    );

    if(e!=ESP_OK){
      errors++;

      Serial.printf(
        "TARS: DAC I2S ERROR=%d\r\n",
        (int)e
      );

      return 0;
    }

    outBytes+=written;

    return written?
      frames*info.channels*2:
      0;
  }
};

DACOut dacOut;
MP3DecoderHelix decoder;

EncodedAudioStream mp3(
  &dacOut,
  &decoder
);

// ============================================================
// PLAY MP3
// ============================================================

bool playMP3(){
  File f=LittleFS.open(MP3_FILE,FILE_READ);

  if(!f){
    Serial.println("TARS: MP3 FILE ERROR");
    return false;
  }

  Serial.printf(
    "TARS: PLAY MP3 SIZE=%u\r\n",
    (unsigned)f.size()
  );

  dacOut.resetStats();

  playing=true;
  oledPos=0;
  oledTick=millis();

  if(!mp3.begin()){
    Serial.println("TARS: MP3 DECODER ERROR");
    f.close();
    playing=false;
    return false;
  }

  oledBase(singMode?"SINGING":"SPEAKING");

  StreamCopy copy(mp3,f,BUF);
  uint32_t t=millis();

  while(f.available()&&millis()-t<120000){
    size_t copied=copy.copy();

    if(!copied)
      delay(1);

    oledType();
    yield();
  }

  mp3.end();
  f.close();
  playing=false;

  dacOut.printStats();
  LittleFS.remove(MP3_FILE);

  oledBase("READY","WAITING...");
  Serial.println("TARS: PLAYBACK DONE");

  return true;
}

// ============================================================
// SING DETECTOR
// ============================================================

bool singRequest(String s){
  s.toLowerCase();

  return
    s.indexOf("nyanyi")>=0||
    s.indexOf("bernyanyi")>=0||
    s.indexOf("nyanyikan")>=0;
}

// ============================================================
// PROCESS
// ============================================================

void processQuestion(const String &q){
  String answer=ask(q);

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

  if(downloadMP3(url,singMode?q:answer)){
    playMP3();
  }else{
    oledBase("READY","AUDIO ERROR");
  }

  singMode=false;
}

// ============================================================
// SETUP
// ============================================================

void setup(){
  Serial.begin(SERIAL_BAUD);

  Wire.begin(OLED_SDA,OLED_SCL);

  oledOK=oled.begin(
    SSD1306_SWITCHCAPVCC,
    OLED_ADDR
  );

  if(oledOK)
    oledBase("BOOT");

  LittleFS.begin(true);

  dacOK=initDAC();

  if(dacOK)
    testDAC();

  micOK=initMic();

  Serial.printf(
    "TARS: DAC %s\r\n",
    dacOK?"READY":"ERROR"
  );

  Serial.printf(
    "TARS: MIC %s\r\n",
    micOK?"READY":"ERROR"
  );

  Serial.println("TARS: BLUETOOTH DISABLED");

  wifiManagerBegin();

  if(wifiOK()){
    wifiManagerDisconnect();
    Serial.println("TARS: WIFI OFF");
  }

  oledBase("READY","WAITING...");
}

// ============================================================
// LOOP
// ============================================================

void loop(){
  if(playing){
    delay(10);
    return;
  }

  if(recordMic()){
    String q=stt();

    LittleFS.remove(STT_FILE);

    if(q.length())
      processQuestion(q);
    else
      oledBase("READY","NO INPUT");

    wifiManagerDisconnect();
    ntpOK=false;

  }else{
    oledBase("READY","WAITING...");

    if(WiFi.status()==WL_CONNECTED){
      wifiManagerDisconnect();
      ntpOK=false;
    }
  }

  delay(10);
}
