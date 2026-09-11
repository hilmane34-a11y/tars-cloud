#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Wire.h>
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
#define DAC_L 25
#define DAC_R 26

const uint32_t MIC_RATE=16000,PLAY_RATE=22050,RECORD_MS=4000;
const size_t BUF=1024;

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);
bool oledOK=false,micOK=false,dacOK=false,playing=false,singMode=false;
String oledText; size_t oledPos=0;
uint32_t oledTick=0;

void oledShow(const char* title,const String& text=""){
  if(!oledOK)return;
  oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1);
  oled.setCursor(42,0);oled.print("T A R S");
  oled.drawLine(0,9,127,9,SSD1306_WHITE);
  oled.setCursor(3,14);oled.print(title);
  if(text.length()){oled.setCursor(3,27);oled.print(text);}
  oled.display();
}

void oledType(){
  if(!oledOK||!oledText.length())return;
  if(millis()-oledTick<45)return;
  oledTick=millis();
  if(oledPos<oledText.length())oledPos++;
  oled.clearDisplay();oled.setTextColor(SSD1306_WHITE);oled.setTextSize(1);
  oled.setCursor(42,0);oled.print("T A R S");
  oled.drawLine(0,9,127,9,SSD1306_WHITE);
  oled.setCursor(3,14);oled.print(playing?"SPEAKING":"READY");
  oled.setCursor(3,27);
  for(size_t i=0;i<oledPos;i++)oled.print(oledText[i]);
  oled.display();
}

bool initDAC(){
  i2s_config_t c={};
  c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_TX|I2S_MODE_DAC_BUILT_IN);
  c.sample_rate=PLAY_RATE;c.bits_per_sample=I2S_BITS_PER_SAMPLE_16BIT;
  c.channel_format=I2S_CHANNEL_FMT_RIGHT_LEFT;
  c.communication_format=I2S_COMM_FORMAT_I2S_MSB;
  c.dma_buf_count=4;c.dma_buf_len=256;c.tx_desc_auto_clear=true;
  if(i2s_driver_install(DAC_PORT,&c,0,nullptr)!=ESP_OK)return false;
  if(i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN)!=ESP_OK)return false;
  i2s_zero_dma_buffer(DAC_PORT);return true;
}

bool initMic(){
  i2s_config_t c={};
  c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_RX);
  c.sample_rate=MIC_RATE;c.bits_per_sample=I2S_BITS_PER_SAMPLE_32BIT;
  c.channel_format=I2S_CHANNEL_FMT_ONLY_LEFT;
  c.communication_format=I2S_COMM_FORMAT_I2S;
  c.dma_buf_count=2;c.dma_buf_len=256;
  if(i2s_driver_install(MIC_PORT,&c,0,nullptr)!=ESP_OK)return false;
  i2s_pin_config_t p={MIC_SCK,MIC_WS,I2S_PIN_NO_CHANGE,MIC_SD};
  return i2s_set_pin(MIC_PORT,&p)==ESP_OK;
}

void put16(uint8_t*p,uint16_t v){p[0]=v;p[1]=v>>8;}
void put32(uint8_t*p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}

void wavHeader(File&f,uint32_t n){
  uint8_t h[44]={};
  memcpy(h,"RIFF",4);put32(h+4,n+36);memcpy(h+8,"WAVEfmt ",8);
  put32(h+16,16);put16(h+20,1);put16(h+22,1);
  put32(h+24,MIC_RATE);put32(h+28,MIC_RATE*2);
  put16(h+32,2);put16(h+34,16);memcpy(h+36,"data",4);put32(h+40,n);
  f.seek(0);f.write(h,44);
}

bool recordMic(){
  if(!micOK)return false;
  oledShow("LISTENING","SPEAK NOW");
  if(LittleFS.exists(STT_FILE))LittleFS.remove(STT_FILE);
  File f=LittleFS.open(STT_FILE,FILE_WRITE);if(!f)return false;
  uint8_t z[44]={};f.write(z,44);
  int32_t raw[BUF/4];int16_t pcm[BUF/4];uint32_t samples=0,start=millis();
  while(millis()-start<RECORD_MS){
    size_t n=0;
    if(i2s_read(MIC_PORT,raw,sizeof(raw),&n,100)==ESP_OK)
      for(size_t i=0;i<n/4;i++){
        int32_t v=raw[i]>>14;
        v=max((int32_t)-32768,min((int32_t)32767,v));
        pcm[i]=(int16_t)v;
      }
    size_t s=n/4;
    if(s){f.write((uint8_t*)pcm,s*2);samples+=s;}
    yield();
  }
  wavHeader(f,samples*2);f.close();return samples>0;
}

bool wifiOK(){
  if(WiFi.status()!=WL_CONNECTED)
    if(!wifiManagerConnect(false))return false;
  return true;
}

String stt(){
  if(!wifiOK())return "";
  File f=LittleFS.open(STT_FILE,FILE_READ);if(!f)return "";
  const char*b="----TARSSTT";
  String a="--"+String(b)+"\r\nContent-Disposition: form-data; name=\"file\"; filename=\"stt.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  String e="\r\n--"+String(b)+"--\r\n";
  WiFiClientSecure c;c.setInsecure();
  String host=String(TARS_CLOUD_URL).substring(String(TARS_CLOUD_URL).indexOf("://")+3);
  int slash=host.indexOf('/');if(slash>=0)host=host.substring(0,slash);
  if(!c.connect(host.c_str(),443)){f.close();return "";}
  size_t total=a.length()+f.size()+e.length();
  c.printf("POST /stt HTTP/1.1\r\nHost: %s\r\nContent-Type: multipart/form-data; boundary=%s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",host.c_str(),b,(unsigned)total);
  c.print(a);uint8_t buf[BUF];
  while(f.available()){
    size_t n=f.read(buf,sizeof(buf));if(!n)break;
    if(c.write(buf,n)!=n){f.close();c.stop();return "";}
    yield();
  }
  f.close();c.print(e);
  uint32_t t=millis();
  while(!c.available()&&c.connected()&&millis()-t<15000){delay(5);yield();}
  String r=c.readString();c.stop();
  int p=r.indexOf("\r\n\r\n");if(p<0)return "";
  JsonDocument j;if(deserializeJson(j,r.substring(p+4)))return "";
  String s;
  if(j["text"].is<const char*>())s=j["text"].as<const char*>();
  if(!s.length()&&j["transcript"].is<const char*>())s=j["transcript"].as<const char*>();
  s.trim();return s;
}

String ask(const String&q){
  if(!wifiOK())return "";
  oledShow("PROCESSING","ANALYZING...");
  WiFiClientSecure c;c.setInsecure();HTTPClient h;
  if(!h.begin(c,String(TARS_CLOUD_URL)+"/ask"))return "";
  h.setTimeout(30000);h.addHeader("Content-Type","application/json");
  JsonDocument j;j["text"]=q;String b;serializeJson(j,b);
  int code=h.POST(b);if(code<200||code>=300){h.end();return "";}
  String r=h.getString();h.end();JsonDocument x;
  if(deserializeJson(x,r))return "";
  String s=x["response"].as<String>();s.trim();return s;
}

bool downloadMP3(const String&url,const String&text){
  if(!wifiOK())return false;
  WiFiClientSecure c;c.setInsecure();HTTPClient h;
  if(!h.begin(c,url))return false;
  h.setTimeout(60000);h.addHeader("Content-Type","application/json");
  JsonDocument j;j["text"]=text;String b;serializeJson(j,b);
  int code=h.POST(b);if(code<200||code>=300){h.end();return false;}
  File f=LittleFS.open(MP3_FILE,FILE_WRITE);if(!f){h.end();return false;}
  WiFiClient*s=h.getStreamPtr();uint8_t buf[BUF];int len=h.getSize();size_t total=0;
  uint32_t t=millis();
  while(h.connected()&&(len>0||len==-1)){
    size_t n=s->available();
    if(n){
      n=min(n,sizeof(buf));int r=s->readBytes(buf,n);
      if(r>0){f.write(buf,r);total+=r;if(len>0)len-=r;t=millis();}
    }else{if(millis()-t>5000)break;delay(1);}
    yield();
  }
  f.close();h.end();return total>0;
}

class DACOut:public AudioStream{
  AudioInfo info;
  uint16_t b[BUF/2];
public:
  void setAudioInfo(AudioInfo i)override{info=i;AudioStream::setAudioInfo(i);}
  int availableForWrite()override{return BUF;}
  size_t write(const uint8_t*d,size_t n)override{
    if(!dacOK||!d||info.bits_per_sample!=16)return 0;
    size_t frames=n/(info.channels*2);if(frames>BUF/4)frames=BUF/4;
    for(size_t i=0;i<frames;i++){
      int16_t v;
      if(info.channels==1)v=(int16_t)(d[i*2]|((uint16_t)d[i*2+1]<<8));
      else{
        int16_t l=d[i*4]|((uint16_t)d[i*4+1]<<8),r=d[i*4+2]|((uint16_t)d[i*4+3]<<8);
        v=(int16_t)(((int32_t)l+r)/2);
      }
      int32_t x=(int32_t)(v*3.5f);
      x=max((int32_t)-32768,min((int32_t)32767,x));
      uint8_t u=(uint8_t)((x>>8)+128);
      b[i*2]=b[i*2+1]=(uint16_t)u<<8;
    }
    size_t w=0;i2s_write(DAC_PORT,b,frames*4,&w,portMAX_DELAY);
    return w?frames*info.channels*2:0;
  }
}dacOut;

MP3DecoderHelix decoder;
EncodedAudioStream mp3(&dacOut,&decoder);

bool playMP3(){
  File f=LittleFS.open(MP3_FILE,FILE_READ);if(!f)return false;
  playing=true;oledPos=0;
  if(!mp3.begin()){f.close();playing=false;return false;}
  StreamCopy copy(mp3,f,BUF);uint32_t t=millis();
  while(f.available()&&millis()-t<120000){
    copy.copy();oledType();yield();
  }
  mp3.end();f.close();playing=false;LittleFS.remove(MP3_FILE);
  oledShow("READY","WAITING...");
  return true;
}

bool singRequest(String s){
  s.toLowerCase();
  return s.indexOf("nyanyi")>=0||s.indexOf("bernyanyi")>=0||s.indexOf("nyanyikan")>=0;
}

void processQuestion(const String&q){
  String answer=ask(q);if(!answer.length()){oledShow("READY");return;}
  oledText=answer;oledPos=0;
  bool sing=singRequest(q);singMode=sing;
  String url=String(TARS_CLOUD_URL)+(sing?"/sing":"/tts");
  if(downloadMP3(url,sing?q:answer))playMP3();
  else oledShow("READY","AUDIO ERROR");
  singMode=false;
}

void setup(){
  Serial.begin(SERIAL_BAUD);
  Wire.begin(OLED_SDA,OLED_SCL);
  oledOK=oled.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR);
  if(oledOK)oledShow("BOOT");
  LittleFS.begin(true);
  dacOK=initDAC();
  micOK=initMic();
  Serial.println("TARS: BLUETOOTH DISABLED");
  wifiManagerBegin();
  wifiOK();
  oledShow("READY","WAITING...");
}

void loop(){
  if(WiFi.status()!=WL_CONNECTED&&!playing)wifiManagerConnect(false);
  if(!playing){
    if(recordMic()){
      String q=stt();LittleFS.remove(STT_FILE);
      if(q.length())processQuestion(q);
      else oledShow("READY","NO INPUT");
    }
  }
  delay(10);
}
