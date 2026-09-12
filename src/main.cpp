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

const uint32_t MIC_RATE=16000,PLAY_RATE=22050,RECORD_MS=4000;
const size_t BUF=1024,DAC_BUF=16384;
const int32_t MIC_THRESHOLD=12000;
static const char *STT_FILE="/stt.wav";

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);
bool oledOK=false,micOK=false,dacOK=false,playing=false,singMode=false,ntpOK=false;
String oledText;
size_t oledPos=0;
uint32_t oledTick=0,dotTick=0;
uint8_t dotState=1;

// ================= OLED =================
void oledBase(const char *title,const String &text=""){
  if(!oledOK)return;
  oled.clearDisplay();oled.setTextColor(SSD1306_WHITE);oled.setTextSize(1);
  oled.setCursor(42,0);oled.print("T A R S");
  oled.drawLine(0,9,127,9,SSD1306_WHITE);
  oled.setCursor(3,14);oled.print(title);
  if(text.length()){oled.setCursor(3,27);oled.print(text);}
  oled.display();
}

void oledListening(){
  if(!oledOK||millis()-dotTick<350)return;
  dotTick=millis();dotState=dotState>=4?1:dotState+1;
  String d;for(uint8_t i=0;i<dotState;i++)d+=".";
  oledBase("LISTENING",d);
}

void oledType(){
  if(!oledOK||!oledText.length()||millis()-oledTick<65)return;
  oledTick=millis();if(oledPos<oledText.length())oledPos++;
  oled.clearDisplay();oled.setTextColor(SSD1306_WHITE);oled.setTextSize(1);
  oled.setCursor(42,0);oled.print("T A R S");
  oled.drawLine(0,9,127,9,SSD1306_WHITE);
  oled.setCursor(3,14);oled.print(playing?(singMode?"SINGING":"SPEAKING"):"READY");
  oled.setCursor(3,27);
  for(size_t i=0;i<oledPos;i++)oled.print(oledText[i]);
  oled.display();
}

// ================= DAC =================
bool initDAC(){
  dacWrite(AUDIO_DAC_PIN,128);delay(5);dacWrite(AUDIO_DAC_PIN,128);
  Serial.println("TARS: DIRECT DAC GPIO26 READY");
  return true;
}

// ================= INMP441 =================
bool initMic(){
  i2s_config_t c={};
  c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_RX);
  c.sample_rate=MIC_RATE;
  c.bits_per_sample=I2S_BITS_PER_SAMPLE_32BIT;
  c.channel_format=I2S_CHANNEL_FMT_ONLY_RIGHT;
  c.communication_format=I2S_COMM_FORMAT_I2S;
  c.dma_buf_count=2;c.dma_buf_len=256;

  if(i2s_driver_install(MIC_PORT,&c,0,nullptr)!=ESP_OK)return false;

  i2s_pin_config_t p={};
  p.mck_io_num=I2S_PIN_NO_CHANGE;
  p.bck_io_num=MIC_SCK;
  p.ws_io_num=MIC_WS;
  p.data_out_num=I2S_PIN_NO_CHANGE;
  p.data_in_num=MIC_SD;
  return i2s_set_pin(MIC_PORT,&p)==ESP_OK;
}

// ================= WAV =================
void put16(uint8_t *p,uint16_t v){p[0]=v;p[1]=v>>8;}
void put32(uint8_t *p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}

void wavHeader(File &f,uint32_t n){
  uint8_t h[44]={};
  memcpy(h,"RIFF",4);put32(h+4,n+36);memcpy(h+8,"WAVEfmt ",8);
  put32(h+16,16);put16(h+20,1);put16(h+22,1);put32(h+24,MIC_RATE);
  put32(h+28,MIC_RATE*2);put16(h+32,2);put16(h+34,16);
  memcpy(h+36,"data",4);put32(h+40,n);
  f.seek(0);f.write(h,44);
}

// ================= RECORD =================
bool recordMic(){
  if(!micOK){Serial.println("TARS: MIC NOT READY");return false;}

  dotState=1;dotTick=millis();oledBase("LISTENING",".");
  Serial.println("TARS: RECORDING");

  if(LittleFS.exists(STT_FILE))LittleFS.remove(STT_FILE);
  File f=LittleFS.open(STT_FILE,FILE_WRITE);
  if(!f){Serial.println("TARS: WAV OPEN ERROR");return false;}
  uint8_t z[44]={};f.write(z,44);

  int32_t raw[BUF/4];int16_t pcm[BUF/4];
  uint32_t samples=0,reads=0,errors=0,start=millis();
  int32_t peak=0,minV=32767,maxV=-32768;

  while(millis()-start<RECORD_MS){
    oledListening();
    size_t n=0;
    esp_err_t err=i2s_read(MIC_PORT,raw,sizeof(raw),&n,pdMS_TO_TICKS(100));
    if(err!=ESP_OK){errors++;continue;}
    reads++;size_t count=n/sizeof(int32_t);

    for(size_t i=0;i<count;i++){
      int32_t v=raw[i]>>16;
      v=max((int32_t)-32768,min((int32_t)32767,v));
      pcm[i]=(int16_t)v;
      int32_t av=abs(v);
      if(av>peak)peak=av;if(v<minV)minV=v;if(v>maxV)maxV=v;
    }

    if(count){f.write((uint8_t*)pcm,count*2);samples+=count;}
    yield();
  }

  wavHeader(f,samples*2);f.close();
  Serial.printf("TARS: MIC READ=%lu ERROR=%lu SAMPLES=%lu\r\n",
                (unsigned long)reads,(unsigned long)errors,(unsigned long)samples);
  Serial.printf("TARS: MIC MIN=%ld MAX=%ld PEAK=%ld\r\n",
                (long)minV,(long)maxV,(long)peak);

  if(peak<MIC_THRESHOLD){
    Serial.printf("TARS: MIC AUDIO TOO LOW (<%ld)\r\n",(long)MIC_THRESHOLD);
    return false;
  }

  Serial.println("TARS: MIC AUDIO OK");
  return samples>0;
}

// ================= NTP =================
bool syncTime(){
  configTime(7*3600,0,"pool.ntp.org","time.nist.gov","time.google.com");

  for(int attempt=1;attempt<=4;attempt++){
    Serial.printf("TARS: NTP SYNC %d/4\r\n",attempt);

    for(int i=0;i<20;i++){
      time_t now=time(nullptr);
      if(now>=1704067200){
        struct tm t;localtime_r(&now,&t);
        Serial.printf("TARS: NTP VALID = %04d-%02d-%02d %02d:%02d:%02d\r\n",
                      t.tm_year+1900,t.tm_mon+1,t.tm_mday,
                      t.tm_hour,t.tm_min,t.tm_sec);
        ntpOK=true;return true;
      }
      delay(500);
    }

    if(attempt<4)Serial.println("TARS: NTP INVALID, RETRY");
  }

  ntpOK=false;
  Serial.println("TARS: NTP FAILED AFTER 4 ATTEMPTS");
  return false;
}

// ================= WIFI =================
bool wifiOK(){
  if(WiFi.status()!=WL_CONNECTED)
    if(!wifiManagerConnect(false))return false;

  if(!ntpOK&&!syncTime())return false;
  return true;
}

// ================= HTTP BODY =================
String readHTTPBody(WiFiClientSecure &c){
  String line,body;bool chunked=false;int contentLength=-1;

  while(c.connected()){
    line=c.readStringUntil('\n');line.trim();
    if(!line.length())break;
    String low=line;low.toLowerCase();

    if(low.startsWith("content-length:"))contentLength=low.substring(15).toInt();
    if(low.indexOf("transfer-encoding:")>=0&&low.indexOf("chunked")>=0)chunked=true;
  }

  if(chunked){
    while(c.connected()){
      line=c.readStringUntil('\n');line.trim();
      if(!line.length())continue;

      int size=(int)strtol(line.c_str(),nullptr,16);
      if(size<=0){c.readStringUntil('\n');break;}

      while(size>0){
        uint8_t buf[BUF];
        size_t want=min((int)sizeof(buf),size);
        size_t n=c.readBytes(buf,want);
        if(!n)break;
        body.concat((const char*)buf,n);size-=n;
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
  }else body=c.readString();

  return body;
}

// ================= STT =================
String stt(){
  if(!wifiOK())return "";

  File f=LittleFS.open(STT_FILE,FILE_READ);
  if(!f){Serial.println("TARS: STT WAV OPEN ERROR");return "";}

  const char *b="----TARSSTT";
  String a="--"+String(b)+
    "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"stt.wav\""
    "\r\nContent-Type: audio/wav\r\n\r\n";
  String e="\r\n--"+String(b)+"--\r\n";

  WiFiClientSecure c;c.setInsecure();c.setTimeout(15000);

  String host=TARS_CLOUD_URL;
  int proto=host.indexOf("://");
  if(proto>=0)host=host.substring(proto+3);
  int slash=host.indexOf('/');
  if(slash>=0)host=host.substring(0,slash);

  Serial.println("TARS: STT CONNECTING");

  if(!c.connect(host.c_str(),443)){
    Serial.println("TARS: STT CONNECT ERROR");f.close();return "";
  }

  size_t total=a.length()+f.size()+e.length();

  c.printf("POST /stt HTTP/1.1\r\nHost: %s\r\n"
           "Content-Type: multipart/form-data; boundary=%s\r\n"
           "Content-Length: %u\r\nConnection: close\r\n\r\n",
           host.c_str(),b,(unsigned)total);
  c.print(a);

  uint8_t buf[BUF];

  while(f.available()){
    size_t n=f.read(buf,sizeof(buf));
    if(!n)break;

    if(c.write(buf,n)!=n){
      Serial.println("TARS: STT UPLOAD ERROR");
      f.close();c.stop();return "";
    }
    yield();
  }

  f.close();c.print(e);
  Serial.println("TARS: STT WAITING");

  uint32_t t=millis();
  while(!c.available()&&c.connected()&&millis()-t<20000){
    delay(5);yield();
  }

  if(!c.available()){
    Serial.println("TARS: STT TIMEOUT");c.stop();return "";
  }

  String status=c.readStringUntil('\n');status.trim();
  Serial.print("TARS: STT HTTP = ");Serial.println(status);

  String body=readHTTPBody(c);c.stop();

  if(!body.length()){Serial.println("TARS: STT EMPTY RESPONSE");return "";}
  if(status.indexOf(" 200 ")<0){Serial.println("TARS: STT HTTP ERROR");return "";}

  JsonDocument j;
  if(deserializeJson(j,body)){
    Serial.println("TARS: STT JSON ERROR");return "";
  }

  String s;
  if(j["text"].is<const char*>())s=j["text"].as<const char*>();
  if(!s.length()&&j["transcript"].is<const char*>())
    s=j["transcript"].as<const char*>();
  s.trim();

  if(s.length()){
    Serial.print("TARS: YOU SAID = ");Serial.println(s);
  }else Serial.println("TARS: STT NO TEXT");

  return s;
}

// ================= ASK =================
String ask(const String &q){
  if(!wifiOK())return "";

  Serial.print("TARS: ASK = ");Serial.println(q);
  oledBase("PROCESSING","ANALYZING...");

  WiFiClientSecure c;c.setInsecure();
  HTTPClient h;

  if(!h.begin(c,String(TARS_CLOUD_URL)+"/ask")){
    Serial.println("TARS: ASK BEGIN ERROR");return "";
  }

  h.setTimeout(30000);h.addHeader("Content-Type","application/json");

  JsonDocument j;j["text"]=q;
  String b;serializeJson(j,b);

  int code=h.POST(b);
  Serial.printf("TARS: ASK HTTP = %d\r\n",code);

  if(code<200||code>=300){h.end();return "";}

  String r=h.getString();h.end();
  JsonDocument x;

  if(deserializeJson(x,r)){
    Serial.println("TARS: ASK JSON ERROR");return "";
  }

  String s=x["response"].as<String>();s.trim();
  Serial.print("TARS: ANSWER = ");Serial.println(s);
  return s;
}

// ================= TTS / SING =================
bool downloadMP3(const String &url,const String &text){
  if(!wifiOK())return false;

  Serial.println(url.endsWith("/sing")?"TARS: SING":"TARS: TTS");

  WiFiClientSecure c;c.setInsecure();
  HTTPClient h;

  if(!h.begin(c,url)){
    Serial.println("TARS: AUDIO BEGIN ERROR");return false;
  }

  h.setTimeout(60000);h.addHeader("Content-Type","application/json");

  JsonDocument j;j["text"]=text;
  String b;serializeJson(j,b);

  int code=h.POST(b);
  Serial.printf("TARS: AUDIO HTTP = %d\r\n",code);

  if(code<200||code>=300){h.end();return false;}

  if(LittleFS.exists(MP3_FILE))LittleFS.remove(MP3_FILE);

  File f=LittleFS.open(MP3_FILE,FILE_WRITE);
  if(!f){Serial.println("TARS: MP3 FILE ERROR");h.end();return false;}

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
        f.write(buf,r);total+=r;
        if(len>0)len-=r;
        t=millis();
      }
    }else{
      if(millis()-t>5000)break;
      delay(1);
    }
    yield();
  }

  f.close();h.end();
  Serial.printf("TARS: MP3 BYTES = %lu\r\n",(unsigned long)total);
  return total>0;
}

// ================= DAC RING BUFFER =================
class DACOut:public AudioStream{
  AudioInfo info;
  int16_t buffer[DAC_BUF];
  volatile size_t head=0,tail=0;
  volatile bool active=false;
  TaskHandle_t task=nullptr;

  uint32_t calls=0,pcmBytes=0,dacSamples=0,errors=0;
  int32_t peak=0;
  uint32_t sampleRate=PLAY_RATE;

  size_t count(){
    size_t h=head,t=tail;
    return h>=t?h-t:DAC_BUF-t+h;
  }

  size_t freeSpace(){
    return DAC_BUF-1-count();
  }

  void run(){
    uint32_t next=micros();

    while(active){
      if(head==tail){
        delayMicroseconds(50);
        continue;
      }

      int16_t sample=buffer[tail];
      tail=(tail+1)%DAC_BUF;

      int32_t v=sample,av=abs(v);
      if(av>peak)peak=av;

      uint8_t out=(uint8_t)((v+32768)>>8);

      while((int32_t)(next-micros())>0)
        delayMicroseconds(2);

      dacWrite(AUDIO_DAC_PIN,out);
      dacSamples++;

      uint32_t step=1000000UL/sampleRate;
      next+=step;

      if((int32_t)(micros()-next)>50000)
        next=micros()+step;
    }

    dacWrite(AUDIO_DAC_PIN,128);
  }

  static void taskFunc(void *arg){
    ((DACOut*)arg)->run();
    vTaskDelete(nullptr);
  }

public:
  void startDAC(){
    head=tail=0;active=true;

    if(!task){
      xTaskCreatePinnedToCore(
        taskFunc,"TARS_DAC",4096,this,3,&task,0
      );
    }
  }

  void stopDAC(){
    active=false;
    while(task)vTaskDelay(1);
    dacWrite(AUDIO_DAC_PIN,128);
  }

  bool empty(){return head==tail;}

  void resetStats(){
    calls=pcmBytes=dacSamples=errors=0;peak=0;
    head=tail=0;
  }

  void printStats(){
    Serial.printf("TARS: DAC CALL=%lu PCM=%lu SAMPLES=%lu PEAK=%ld ERROR=%lu\r\n",
                  (unsigned long)calls,(unsigned long)pcmBytes,
                  (unsigned long)dacSamples,(long)peak,(unsigned long)errors);
  }

  void setAudioInfo(AudioInfo i)override{
    info=i;AudioStream::setAudioInfo(i);
    sampleRate=info.sample_rate?info.sample_rate:PLAY_RATE;

    Serial.printf("TARS: DAC AUDIO %uHz %ubit %uch DIRECT GPIO26\r\n",
                  info.sample_rate,info.bits_per_sample,info.channels);
    Serial.printf("TARS: DAC BUFFER=%u SAMPLES\r\n",(unsigned)DAC_BUF);
  }

  int availableForWrite()override{return freeSpace()*2;}

  size_t write(const uint8_t *d,size_t n)override{
    calls++;pcmBytes+=n;

    if(!dacOK||!d||!n||info.bits_per_sample!=16||
       (info.channels!=1&&info.channels!=2)){
      errors++;return 0;
    }

    size_t frames=n/(info.channels*2);
    if(!frames)return 0;

    size_t can=freeSpace();
    if(frames>can)frames=can;

    for(size_t i=0;i<frames;i++){
      int16_t sample;

      if(info.channels==1){
        sample=(int16_t)(d[i*2]|((uint16_t)d[i*2+1]<<8));
      }else{
        sample=(int16_t)(d[i*4+2]|((uint16_t)d[i*4+3]<<8));
      }

      buffer[head]=sample;
      head=(head+1)%DAC_BUF;
    }

    return frames*info.channels*2;
  }
};

DACOut dacOut;
MP3DecoderHelix decoder;
EncodedAudioStream mp3(&dacOut,&decoder);

// ================= PLAY MP3 =================
bool playMP3(){
  File f=LittleFS.open(MP3_FILE,FILE_READ);
  if(!f){Serial.println("TARS: MP3 FILE ERROR");return false;}

  Serial.printf("TARS: PLAY MP3 SIZE=%u\r\n",(unsigned)f.size());

  dacOut.resetStats();
  playing=true;
  oledPos=0;oledTick=millis();

  if(!dacOK){
    dacOK=initDAC();
    if(!dacOK){
      f.close();playing=false;
      Serial.println("TARS: DAC NOT READY");
      return false;
    }
  }

  if(!mp3.begin()){
    f.close();playing=false;
    Serial.println("TARS: MP3 DECODER ERROR");
    return false;
  }

  oledBase(singMode?"SINGING":"SPEAKING");
  dacOut.startDAC();

  StreamCopy copy(mp3,f,BUF);
  uint32_t start=millis();

  while(f.available()&&millis()-start<120000){
    size_t copied=copy.copy();
    if(!copied)delay(1);
    oledType();
    yield();
  }

  mp3.end();
  f.close();

  // Tunggu PCM terakhir selesai keluar ke DAC
  uint32_t drainStart=millis();
  while(!dacOut.empty()&&millis()-drainStart<5000){
    oledType();
    delay(1);
  }

  dacOut.stopDAC();
  playing=false;
  dacWrite(AUDIO_DAC_PIN,128);
  dacOut.printStats();

  LittleFS.remove(MP3_FILE);
  oledBase("READY","WAITING...");
  Serial.println("TARS: PLAYBACK DONE");

  return true;
}

// ================= SING DETECTOR =================
bool singRequest(String s){
  s.toLowerCase();
  return s.indexOf("nyanyi")>=0||
         s.indexOf("bernyanyi")>=0||
         s.indexOf("nyanyikan")>=0;
}

// ================= PROCESS =================
void processQuestion(const String &q){
  String answer=ask(q);

  if(!answer.length()){
    oledBase("READY","ASK ERROR");
    return;
  }

  oledText=answer;oledPos=0;singMode=singRequest(q);

  String url=String(TARS_CLOUD_URL)+(singMode?"/sing":"/tts");

  if(downloadMP3(url,singMode?q:answer))
    playMP3();
  else
    oledBase("READY","AUDIO ERROR");

  singMode=false;
}

// ================= SETUP =================
void setup(){
  Serial.begin(SERIAL_BAUD);

  Wire.begin(OLED_SDA,OLED_SCL);
  oledOK=oled.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR);
  if(oledOK)oledBase("BOOT");

  LittleFS.begin(true);

  dacOK=initDAC();
  micOK=initMic();

  Serial.printf("TARS: DAC %s\r\n",dacOK?"READY":"ERROR");
  Serial.printf("TARS: MIC %s\r\n",micOK?"READY":"ERROR");
  Serial.printf("TARS: MIC THRESHOLD=%ld\r\n",(long)MIC_THRESHOLD);
  Serial.println("TARS: DIRECT AUDIO GPIO26");
  Serial.println("TARS: BLUETOOTH DISABLED");
  Serial.println("TARS: POWER ON BOOT");

  wifiManagerBegin();

  if(wifiOK())Serial.println("TARS: WIFI READY");

  oledBase("READY","WAITING...");
}

// ================= LOOP =================
void loop(){
  if(playing){delay(10);return;}

  if(WiFi.status()!=WL_CONNECTED){
    ntpOK=false;

    if(!wifiOK()){
      oledBase("READY","WIFI ERROR");
      delay(1000);
      return;
    }
  }

  if(recordMic()){
    String q=stt();
    LittleFS.remove(STT_FILE);

    if(q.length())
      processQuestion(q);
    else
      oledBase("READY","NO INPUT");
  }else{
    oledListening();
  }

  delay(10);
}
