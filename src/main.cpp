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

#define MIC_I2S I2S_NUM_1
#define DAC_I2S I2S_NUM_0
#define MIC_RATE 16000
#define PLAY_RATE 22050
#define MIC_BUF 2048
#define MP3_BUF 1024
#define RECORD_MS 4000
#define STT_FILE "/stt.wav"

Adafruit_SSD1306 oled(128,64,&Wire,-1);
bool oledOK=false,micOK=false,dacOK=false,playing=false,singMode=false;
bool typing=false,syncText=false,firstAudio=false;
uint32_t firstAudioAt=0,recordStart=0,lastWifi=0,lastNtp=0,oledTick=0;
uint32_t recordSamples=0;
String answer="",question="";
size_t chars=0;
File rec;

int32_t micRaw[MIC_BUF/4];
int16_t micPCM[MIC_BUF/4];

static void oledDraw(const char *title,const String &txt="",bool speak=false){
  if(!oledOK)return;
  oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1);
  oled.setCursor(45,0); oled.print("T A R S");
  oled.drawLine(0,9,127,9,SSD1306_WHITE);
  oled.setCursor(3,14); oled.print("> "); oled.print(title);
  oled.setCursor(3,25);
  int line=0,col=0;
  for(size_t i=0;i<chars && i<txt.length();i++){
    char c=txt[i];
    if(c=='\n'){line++;col=0;if(line>3)break;oled.setCursor(3,25+line*9);continue;}
    if(col>=21){line++;col=0;if(line>3)break;oled.setCursor(3,25+line*9);}
    oled.write(c);col++;
  }
  int y=61;
  if(speak){
    for(int i=0;i<7;i++){
      int h=4+((i+(millis()/70))%5)*4;
      oled.drawLine(5+i*19,y,5+i*19,y-h,SSD1306_WHITE);
    }
  }else{
    for(int i=0;i<24;i++){
      int h=2+((i+(millis()/150))%4)*2;
      oled.drawLine(2+i*5,y,2+i*5,y-h,SSD1306_WHITE);
    }
  }
  oled.display();
}

static void oledReady(){chars=0;answer="";oledDraw("READY");}
static void oledListen(){chars=0;oledDraw("LISTENING","SPEAK NOW");}
static void oledProcess(){chars=0;oledDraw("PROCESSING","ANALYZING...");}

static void oledUpdate(){
  if(!oledOK)return;
  uint32_t now=millis();
  if(now-oledTick<45)return;
  oledTick=now;
  if(syncText && firstAudio && now-firstAudioAt>50){
    syncText=false;typing=true;chars=0;
  }
  if(typing && chars<answer.length())chars++;
  if(typing || playing)oledDraw(playing?"SPEAKING":"READY",answer,playing);
}

static bool initDAC(){
  i2s_config_t c={};
  c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_TX|I2S_MODE_DAC_BUILT_IN);
  c.sample_rate=PLAY_RATE;c.bits_per_sample=I2S_BITS_PER_SAMPLE_16BIT;
  c.channel_format=I2S_CHANNEL_FMT_RIGHT_LEFT;
  c.communication_format=I2S_COMM_FORMAT_I2S_MSB;
  c.dma_buf_count=4;c.dma_buf_len=256;c.tx_desc_auto_clear=true;
  if(i2s_driver_install(DAC_I2S,&c,0,NULL)!=ESP_OK)return false;
  return i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN)==ESP_OK;
}

class DACOut:public AudioStream{
  AudioInfo info; uint16_t b[MP3_BUF/2];
public:
  void setAudioInfo(AudioInfo i)override{
    info=i;AudioStream::setAudioInfo(i);
    if(dacOK)i2s_set_clk(DAC_I2S,i.sample_rate,I2S_BITS_PER_SAMPLE_16BIT,I2S_CHANNEL_STEREO);
  }
  int availableForWrite()override{return MP3_BUF;}
  size_t write(const uint8_t*d,size_t n)override{
    if(!dacOK||!d||n<2||info.bits_per_sample!=16)return 0;
    size_t f=n/(info.channels*2);f=min(f,(size_t)(MP3_BUF/4));
    int peak=0;
    for(size_t i=0;i<f;i++){
      int16_t s;
      if(info.channels==1)s=(int16_t)(d[i*2]|d[i*2+1]<<8);
      else{
        int16_t l=(int16_t)(d[i*4]|d[i*4+1]<<8);
        int16_t r=(int16_t)(d[i*4+2]|d[i*4+3]<<8);s=(l+r)/2;
      }
      peak=max(peak,abs((int)s));
      int32_t v=(int32_t)(s*3.5f);v=max(-32768L,min(32767L,v));
      uint16_t x=(uint8_t)((v>>8)+128)<<8;b[i*2]=x;b[i*2+1]=x;
    }
    size_t w=0;
    if(i2s_write(DAC_I2S,b,f*4,&w,portMAX_DELAY)!=ESP_OK)return 0;
    if(w&&!firstAudio){firstAudio=true;firstAudioAt=millis();}
    return f*info.channels*2;
  }
};

DACOut dac;
MP3DecoderHelix decoder;
EncodedAudioStream mp3(&dac,&decoder);

static bool initMic(){
  i2s_config_t c={};
  c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_RX);
  c.sample_rate=MIC_RATE;c.bits_per_sample=I2S_BITS_PER_SAMPLE_32BIT;
  c.channel_format=I2S_CHANNEL_FMT_ONLY_LEFT;
  c.communication_format=I2S_COMM_FORMAT_I2S;
  c.dma_buf_count=2;c.dma_buf_len=256;
  if(i2s_driver_install(MIC_I2S,&c,0,NULL)!=ESP_OK)return false;
  i2s_pin_config_t p={18,19,I2S_PIN_NO_CHANGE,34};
  return i2s_set_pin(MIC_I2S,&p)==ESP_OK;
}

static void wavHeader(File&f,uint32_t size){
  uint8_t h[44]={};
  memcpy(h,"RIFF",4);memcpy(h+8,"WAVE",4);memcpy(h+12,"fmt ",4);
  h[16]=16;h[20]=1;h[22]=1;
  h[24]=MIC_RATE&255;h[25]=MIC_RATE>>8;h[26]=MIC_RATE>>16;h[27]=MIC_RATE>>24;
  uint32_t br=MIC_RATE*2;h[28]=br&255;h[29]=br>>8;h[30]=br>>16;h[31]=br>>24;
  h[32]=2;h[34]=16;memcpy(h+36,"data",4);
  uint32_t rs=36+size;h[4]=rs&255;h[5]=rs>>8;h[6]=rs>>16;h[7]=rs>>24;
  h[40]=size&255;h[41]=size>>8;h[42]=size>>16;h[43]=size>>24;
  f.seek(0);f.write(h,44);
}

static bool startRecord(){
  if(!micOK)return false;
  if(LittleFS.exists(STT_FILE))LittleFS.remove(STT_FILE);
  rec=LittleFS.open(STT_FILE,FILE_WRITE);
  if(!rec)return false;
  uint8_t z[44]={};rec.write(z,44);recordSamples=0;recordStart=millis();oledListen();return true;
}

static bool recordStep(){
  size_t n=0;
  if(i2s_read(MIC_I2S,micRaw,sizeof(micRaw),&n,0)!=ESP_OK)return true;
  for(size_t i=0;i<n/4;i++){
    int32_t v=micRaw[i]>>14;v=max(-32768L,min(32767L,v));micPCM[i]=v;
  }
  size_t samples=n/4;rec.write((uint8_t*)micPCM,samples*2);recordSamples+=samples;
  if(millis()-recordStart<RECORD_MS)return true;
  wavHeader(rec,recordSamples*2);rec.close();return false;
}

static bool wifiOK(){
  if(WiFi.status()==WL_CONNECTED)return true;
  return wifiManagerConnect(false);
}

static void wifiMaintain(){
  uint32_t n=millis();
  if(n-lastWifi<3000)return;
  lastWifi=n;
  if(WiFi.status()!=WL_CONNECTED)wifiManagerConnect(false);
  if(WiFi.status()==WL_CONNECTED && n-lastNtp>30000){
    lastNtp=n;
    if(time(nullptr)<1577836800)
      configTime(7*3600,0,"pool.ntp.org","time.nist.gov");
  }
}

static String httpJson(const char*url,const String&text){
  if(!wifiOK())return "";
  WiFiClientSecure c;c.setInsecure();HTTPClient h;
  if(!h.begin(c,url))return "";
  h.addHeader("Content-Type","application/json");
  StaticJsonDocument<384> j;j["text"]=text;String body;serializeJson(j,body);
  int code=h.POST(body);if(code<200||code>=300){h.end();return "";}
  String r=h.getString();h.end();return r;
}

static String ask(const String&q){
  oledProcess();String r=httpJson(ASK_URL,q);StaticJsonDocument<768>j;
  if(deserializeJson(j,r))return "";String a=j["response"]|"";
  a.trim();return a;
}

static String stt(){
  if(!wifiOK())return "";
  File f=LittleFS.open(STT_FILE,FILE_READ);if(!f)return "";
  const char*b="----TARSSTT";String pre="--"+String(b)+"\r\nContent-Disposition: form-data; name=\"file\"; filename=\"stt.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  String post="\r\n--"+String(b)+"--\r\n";size_t total=pre.length()+f.size()+post.length();
  WiFiClientSecure c;c.setInsecure();
  String host=TARS_CLOUD_URL;int x=host.indexOf("://");if(x>=0)host=host.substring(x+3);
  x=host.indexOf('/');String path=x>=0?host.substring(x):"";if(x>=0)host=host.substring(0,x);
  if(!c.connect(host.c_str(),443)){f.close();return "";}
  c.printf("POST %s/stt HTTP/1.1\r\nHost: %s\r\nContent-Type: multipart/form-data; boundary=%s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",path.c_str(),host.c_str(),b,(unsigned)total);
  c.print(pre);uint8_t buf[MP3_BUF];
  while(f.available()){size_t n=f.read(buf,sizeof(buf));if(n)c.write(buf,n);yield();}
  f.close();c.print(post);
  uint32_t t=millis();while(c.connected()&&!c.available()&&millis()-t<15000)yield();
  String r;while(c.available())r+=c.readStringUntil('\n');
  c.stop();x=r.indexOf("\r\n\r\n");if(x>=0)r=r.substring(x+4);
  StaticJsonDocument<512>j;if(deserializeJson(j,r))return "";
  String s=j["text"]|j["transcript"]|"";s.trim();return s;
}

static bool downloadMP3(const char*url,const String&text){
  if(!wifiOK())return false;
  WiFiClientSecure c;c.setInsecure();HTTPClient h;
  if(!h.begin(c,url))return false;
  h.addHeader("Content-Type","application/json");
  StaticJsonDocument<384>j;j["text"]=text;String body;serializeJson(j,body);
  int code=h.POST(body);if(code<200||code>=300){h.end();return false;}
  if(LittleFS.exists(MP3_FILE))LittleFS.remove(MP3_FILE);
  File f=LittleFS.open(MP3_FILE,FILE_WRITE);if(!f){h.end();return false;}
  WiFiClient*s=h.getStreamPtr();uint8_t buf[MP3_BUF];int len=h.getSize();size_t total=0;uint32_t t=millis();
  while(h.connected()&&(len>0||len==-1)){
    size_t a=s->available();
    if(a){size_t n=min(a,sizeof(buf));int r=s->readBytes(buf,n);if(r>0){f.write(buf,r);total+=r;if(len>0)len-=r;t=millis();}}
    else{if(millis()-t>5000)break;yield();}
  }
  f.close();h.end();return total>0;
}

static bool playMP3(){
  File f=LittleFS.open(MP3_FILE,FILE_READ);if(!f)return false;
  playing=true;firstAudio=false;syncText=true;firstAudioAt=0;
  mp3.begin();StreamCopy cp(mp3,f,MP3_BUF);uint32_t t=millis();
  while(f.available()&&millis()-t<120000){cp.copy();oledUpdate();yield();}
  mp3.end();f.close();playing=false;syncText=false;typing=false;singMode=false;
  LittleFS.remove(MP3_FILE);oledReady();return true;
}

static void processQuestion(String q){
  q.trim();if(!q.length())return;
  question=q;answer=ask(q);if(!answer.length()){oledReady();return;}
  chars=0;typing=false;syncText=false;
  bool sing=q.indexOf("nyanyi")>=0||q.indexOf("bernyanyi")>=0||q.indexOf("nyanyikan")>=0;
  singMode=sing;
  if(downloadMP3(sing?SING_URL:TTS_URL,sing?q:answer))playMP3();
  else oledReady();
}

enum State{IDLE,REC,STT,ASK};
State state=IDLE;

void setup(){
  Serial.begin(SERIAL_BAUD);
  Wire.begin(OLED_SDA,OLED_SCL);
  oledOK=oled.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR);
  if(oledOK)oledReady();

  LittleFS.begin(true);
  dacOK=initDAC();
  micOK=initMic();

  Serial.println("TARS: INIT");
  Serial.println("TARS: BLUETOOTH DISABLED");
  Serial.println("TARS: SERIAL MONITOR ONLY");

  if(wifiManagerBegin()){
    if(wifiManagerConnect(false)){
      configTime(7*3600,0,"pool.ntp.org","time.nist.gov");
      Serial.println("TARS: WIFI ON");
    }
  }
  oledReady();
  Serial.println("TARS: READY");
}

void loop(){
  wifiMaintain();
  oledUpdate();

  if(state==IDLE){
    if(WiFi.status()==WL_CONNECTED&&startRecord())state=REC;
  }
  else if(state==REC){
    if(!recordStep())state=STT;
  }
  else if(state==STT){
    question=stt();LittleFS.remove(STT_FILE);
    if(question.length())state=ASK;
    else state=IDLE,oledReady();
  }
  else if(state==ASK){
    processQuestion(question);
    question="";answer="";
    state=IDLE;
  }

  yield();
}
