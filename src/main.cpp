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

const uint32_t MIC_RATE=16000,RECORD_MAX_MS=5000,RECORD_MIN_MS=500;
const uint32_t SILENCE_MS=900,LISTEN_MAX_MS=8000,PREROLL_MS=500;
const int32_t MIC_THRESHOLD=14000,MIC_SILENCE=12000;
const uint32_t OLED_TYPE_MS=35,OLED_WAVE_MS=70;
const size_t BUF=2048,PREROLL_SAMPLES=MIC_RATE*PREROLL_MS/1000;
const char*STT_FILE="/stt.wav";

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);
AnalogAudioStream analog;
MP3DecoderHelix codec;
EncodedAudioStream dec(&analog,&codec);
StreamCopy copier;

/* STT CONNECTION REUSE */
WiFiClientSecure sttClient;
String sttHost;
bool sttClientReady=false;

bool oledOK=false,micOK=false,dacOK=false,playing=false,ntpOK=false,singMode=false;

/* ================= OLED ================= */
enum OledMode:uint8_t{OLED_LISTENING,OLED_SPEAKING,OLED_STATUS};
volatile OledMode oledMode=OLED_LISTENING;
volatile bool oledSing=false;
volatile uint32_t oledSession=0;
char oledMsg[256]="";
portMUX_TYPE oledMux=portMUX_INITIALIZER_UNLOCKED;

void oledSetStatus(const char*t){
  if(!oledOK)return;
  portENTER_CRITICAL(&oledMux);
  oledMode=OLED_STATUS;
  strncpy(oledMsg,t,sizeof(oledMsg)-1);
  oledMsg[sizeof(oledMsg)-1]=0;
  oledSession++;
  portEXIT_CRITICAL(&oledMux);
}

void oledSetListening(){
  if(!oledOK)return;
  portENTER_CRITICAL(&oledMux);
  oledMode=OLED_LISTENING;
  oledSing=false;
  oledMsg[0]=0;
  oledSession++;
  portEXIT_CRITICAL(&oledMux);
}

void oledStartSpeak(const String&t){
  if(!oledOK)return;
  portENTER_CRITICAL(&oledMux);
  oledMode=OLED_SPEAKING;
  oledSing=singMode;
  strncpy(oledMsg,t.c_str(),sizeof(oledMsg)-1);
  oledMsg[sizeof(oledMsg)-1]=0;
  oledSession++;
  portEXIT_CRITICAL(&oledMux);
}

void oledHeader(){
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(42,0);
  oled.print("T A R S");
  oled.drawLine(0,9,127,9,SSD1306_WHITE);
}

size_t oledPageEnd(const char*s,size_t start){
  size_t i=start,n=strlen(s);
  int lines=1,x=3;
  while(i<n){
    char c=s[i];
    if(c=='\n'){
      lines++;x=3;i++;
      if(lines>3)break;
      continue;
    }
    if(x>121){
      lines++;x=3;
      if(lines>3)break;
    }
    x+=6;i++;
  }
  return i;
}

void oledTask(void*){
  uint32_t typeTick=0,waveTick=0,dotTick=0,session=0;
  size_t pos=0,page=0;
  uint8_t dots=1,phase=0;
  char text[256];

  for(;;){
    if(!oledOK){
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    OledMode mode;
    bool singing;
    uint32_t s;

    portENTER_CRITICAL(&oledMux);
    mode=oledMode;
    singing=oledSing;
    s=oledSession;
    strncpy(text,oledMsg,sizeof(text)-1);
    text[sizeof(text)-1]=0;
    portEXIT_CRITICAL(&oledMux);

    if(s!=session){
      session=s;pos=0;page=0;
      typeTick=waveTick=millis();
      phase=0;
    }

    if(mode==OLED_LISTENING){
      if(millis()-dotTick>=300){
        dotTick=millis();
        dots=dots>=4?1:dots+1;
      }
      oledHeader();
      oled.setCursor(3,14);oled.print("LISTENING");
      oled.setCursor(3,27);
      for(uint8_t i=0;i<dots;i++)oled.print('.');
      oled.display();
    }else if(mode==OLED_STATUS){
      oledHeader();
      oled.setCursor(3,27);oled.print(text);
      oled.display();
    }else{
      uint32_t now=millis();
      size_t len=strlen(text);

      if(now-typeTick>=OLED_TYPE_MS){
        size_t add=(now-typeTick)/OLED_TYPE_MS;
        typeTick+=add*OLED_TYPE_MS;
        if(pos<len)pos=min(len,pos+add);
      }

      size_t end=oledPageEnd(text,page);
      if(pos>=end&&end<len)page=end;

      if(now-waveTick>=OLED_WAVE_MS){
        waveTick=now;phase++;
      }

      oledHeader();

      if(singing){
        oled.setCursor(3,14);
        oled.print("SINGING");
      }

      int x=3,y=27;
      for(size_t i=page;i<pos;i++){
        char c=text[i];
        if(c=='\n'){
          x=3;y+=8;
          if(y>43)break;
          continue;
        }
        if(x>121){
          x=3;y+=8;
          if(y>43)break;
        }
        oled.setCursor(x,y);
        oled.write(c);
        x=oled.getCursorX();
      }

      oled.fillRect(0,55,128,9,SSD1306_BLACK);
      for(int x=3,i=0;x<125;x+=6,i++){
        int h=1+((i*7+phase*3)%4);
        oled.drawLine(x,63-h,x,63,SSD1306_WHITE);
      }
      oled.display();
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

/* ================= DAC ================= */
bool initDAC(){
  auto cfg=analog.defaultConfig(TX_MODE);
  cfg.channels=2;
  if(!analog.begin(cfg))return false;
  Serial.println("TARS: PAM RIGHT GPIO26 READY");
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

  if(i2s_driver_install(MIC_PORT,&c,0,nullptr)!=ESP_OK)return false;

  i2s_pin_config_t p={};
  p.bck_io_num=MIC_SCK;
  p.ws_io_num=MIC_WS;
  p.data_out_num=I2S_PIN_NO_CHANGE;
  p.data_in_num=MIC_SD;

  if(i2s_set_pin(MIC_PORT,&p)!=ESP_OK)return false;
  i2s_zero_dma_buffer(MIC_PORT);
  Serial.println("TARS: INMP441 RIGHT READY");
  return true;
}

/* ================= WAV ================= */
void put16(uint8_t*p,uint16_t v){p[0]=v;p[1]=v>>8;}
void put32(uint8_t*p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}

void wavHeader(File&f,uint32_t n){
  uint8_t h[44]={};
  memcpy(h,"RIFF",4);put32(h+4,n+36);
  memcpy(h+8,"WAVEfmt ",8);put32(h+16,16);
  put16(h+20,1);put16(h+22,1);put32(h+24,MIC_RATE);
  put32(h+28,MIC_RATE*2);put16(h+32,2);put16(h+34,16);
  memcpy(h+36,"data",4);put32(h+40,n);
  f.seek(0);f.write(h,44);
}

/* ================= RECORD ================= */
bool recordMic(){
  if(!micOK)return false;
  oledSetListening();

  if(LittleFS.exists(STT_FILE))LittleFS.remove(STT_FILE);
  File f=LittleFS.open(STT_FILE,FILE_WRITE);
  if(!f)return false;

  uint8_t z[44]={};f.write(z,44);

  static int16_t pre[PREROLL_SAMPLES];
  size_t prePos=0,preCount=0;
  int32_t raw[BUF/4];
  int16_t pcm[BUF/4];

  uint32_t listenStart=millis(),voiceStart=0,lastVoice=0,samples=0;
  bool voice=false;

  while((!voice&&millis()-listenStart<LISTEN_MAX_MS)||
        (voice&&millis()-voiceStart<RECORD_MAX_MS)){

    size_t n=0;
    if(i2s_read(MIC_PORT,raw,sizeof(raw),&n,pdMS_TO_TICKS(50))!=ESP_OK)continue;

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
        if(preCount<PREROLL_SAMPLES)preCount++;
      }

      if(peak>=MIC_THRESHOLD){
        voice=true;
        voiceStart=lastVoice=millis();
        size_t start=preCount==PREROLL_SAMPLES?prePos:0;

        for(size_t i=0;i<preCount;i++){
          size_t k=(start+i)%PREROLL_SAMPLES;
          f.write((uint8_t*)&pre[k],2);
        }

        samples+=preCount;
        f.write((uint8_t*)pcm,count*2);
        samples+=count;
        Serial.println("TARS: VOICE DETECTED");
      }
    }else{
      f.write((uint8_t*)pcm,count*2);
      samples+=count;

      if(peak>=MIC_SILENCE)lastVoice=millis();
      if(millis()-voiceStart>=RECORD_MIN_MS&&
         millis()-lastVoice>=SILENCE_MS)break;
    }
    yield();
  }

  wavHeader(f,samples*2);
  f.close();

  Serial.printf("TARS: RECORD TIME=%lu ms\n",
                (unsigned long)(millis()-listenStart));

  if(!voice||!samples){
    LittleFS.remove(STT_FILE);
    Serial.println("TARS: MIC AUDIO TOO LOW");
    return false;
  }

  Serial.printf("TARS: WAV=%lu BYTES\n",(unsigned long)(samples*2));
  return true;
}

/* ================= NTP ================= */
bool syncTime(){
  if(ntpOK)return true;
  configTime(7*3600,0,"pool.ntp.org","time.nist.gov","time.google.com");

  for(int a=1;a<=4;a++){
    Serial.printf("TARS: NTP ATTEMPT %d/4\n",a);

    for(int i=0;i<20;i++){
      time_t now=time(nullptr);
      if(now>=1704067200){
        struct tm t;
        localtime_r(&now,&t);
        Serial.printf("TARS: NTP VALID %04d-%02d-%02d %02d:%02d:%02d\n",
          t.tm_year+1900,t.tm_mon+1,t.tm_mday,
          t.tm_hour,t.tm_min,t.tm_sec);
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
  if(WiFi.status()==WL_CONNECTED)return true;
  if(!wifiManagerConnect(false))return false;
  return WiFi.status()==WL_CONNECTED;
}

bool bootWiFi(){
  Serial.println("TARS: WIFI CONNECTING...");
  uint32_t start=millis();

  while(WiFi.status()!=WL_CONNECTED&&millis()-start<30000){
    wifiManagerConnect(false);
    delay(200);
    Serial.print(".");
    yield();
  }

  Serial.println();

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
  String line,body;
  bool chunked=false;
  int len=-1;

  while(c.connected()){
    line=c.readStringUntil('\n');
    line.trim();
    if(!line.length())break;

    String x=line;
    x.toLowerCase();

    if(x.startsWith("content-length:"))len=x.substring(15).toInt();
    if(x.indexOf("transfer-encoding:")>=0&&x.indexOf("chunked")>=0)
      chunked=true;
  }

  if(chunked){
    while(c.connected()){
      line=c.readStringUntil('\n');
      line.trim();
      if(!line.length())continue;

      int n=strtol(line.c_str(),nullptr,16);
      if(n<=0){
        c.readStringUntil('\n');
        break;
      }

      while(n>0){
        uint8_t b[BUF];
        size_t w=min((int)sizeof(b),n);
        size_t r=c.readBytes(b,w);
        if(!r)break;
        body.concat((char*)b,r);
        n-=r;
      }
      c.readStringUntil('\n');
    }
  }else if(len>=0){
    while((int)body.length()<len&&c.connected()){
      uint8_t b[BUF];
      int rem=len-body.length();
      size_t w=min((int)sizeof(b),rem);
      size_t r=c.readBytes(b,w);
      if(!r)break;
      body.concat((char*)b,r);
    }
  }else body=c.readString();

  return body;
}

/* ================= STT ================= */
String stt(){
  if(!wifiOK())return "";

  File f=LittleFS.open(STT_FILE,FILE_READ);
  if(!f)return "";

  const char*b="----TARSSTT";

  String head="--"+String(b)+
    "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"stt.wav\""+
    "\r\nContent-Type: audio/wav\r\n\r\n";

  String tail="\r\n--"+String(b)+"--\r\n";

  /* Ambil host sekali */
  if(!sttClientReady){
    sttHost=String(TARS_CLOUD_URL);
    int p=sttHost.indexOf("://");
    if(p>=0)sttHost=sttHost.substring(p+3);
    p=sttHost.indexOf('/');
    if(p>=0)sttHost=sttHost.substring(0,p);

    sttClient.setInsecure();
    sttClient.setTimeout(5000);
    sttClientReady=true;
  }

  uint32_t t0=millis();

  /* REUSE TLS CONNECTION */
  if(!sttClient.connected()){
    if(!sttClient.connect(sttHost.c_str(),443)){
      f.close();
      Serial.printf("TARS: STT CONNECT FAILED TIME=%lu ms\n",
                    (unsigned long)(millis()-t0));
      return "";
    }
  }

  Serial.printf("TARS: STT CONNECT=%lu ms\n",
                (unsigned long)(millis()-t0));

  size_t total=head.length()+f.size()+tail.length();

  sttClient.printf(
    "POST /stt HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Content-Type: multipart/form-data; boundary=%s\r\n"
    "Content-Length: %u\r\n"
    "Connection: keep-alive\r\n\r\n",
    sttHost.c_str(),b,(unsigned)total);

  sttClient.print(head);

  uint32_t uploadStart=millis();
  uint8_t buf[BUF];

  while(f.available()){
    size_t n=f.read(buf,sizeof(buf));
    if(!n)break;

    if(sttClient.write(buf,n)!=n){
      f.close();
      sttClient.stop();
      return "";
    }
    yield();
  }

  f.close();
  sttClient.print(tail);

  Serial.printf("TARS: STT UPLOAD=%lu ms\n",
                (unsigned long)(millis()-uploadStart));

  uint32_t waitStart=millis();

  while(!sttClient.available()&&sttClient.connected()&&
        millis()-waitStart<20000){
    delay(2);
    yield();
  }

  Serial.printf("TARS: STT WAIT=%lu ms\n",
                (unsigned long)(millis()-waitStart));

  if(!sttClient.available()){
    sttClient.stop();
    return "";
  }

  String status=sttClient.readStringUntil('\n');
  status.trim();

  Serial.print("TARS: STT HTTP = ");
  Serial.println(status);

  String body=readHTTPBody(sttClient);

  if(!body.length()||status.indexOf(" 200 ")<0){
    /* Jika server menutup koneksi, request berikutnya akan reconnect */
    if(!sttClient.connected())sttClient.stop();
    return "";
  }

  JsonDocument j;
  if(deserializeJson(j,body))return "";

  String s=j["text"].as<String>();
  if(!s.length())s=j["transcript"].as<String>();
  s.trim();

  if(s.length()){
    Serial.print("TARS: YOU SAID = ");
    Serial.println(s);
  }

  return s;
}

/* ================= ASK ================= */
String ask(const String&q){
  if(!wifiOK())return "";

  WiFiClientSecure c;
  c.setInsecure();
  HTTPClient h;

  if(!h.begin(c,String(TARS_CLOUD_URL)+"/ask"))return "";

  h.setTimeout(12000);
  h.addHeader("Content-Type","application/json");

  JsonDocument j;
  j["question"]=q;

  String b;
  serializeJson(j,b);

  uint32_t t=millis();
  int code=h.POST(b);

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

/* ================= AUDIO ================= */
bool streamAudio(const String&url,const String&text){
  if(!wifiOK())return false;

  bool singing=url.endsWith("/sing");

  WiFiClientSecure c;
  c.setInsecure();
  HTTPClient h;

  if(!h.begin(c,url))return false;

  h.setTimeout(60000);
  h.addHeader("Content-Type","application/json");

  JsonDocument j;
  if(singing)j["prompt"]=text;
  else j["text"]=text;

  String body;
  serializeJson(j,body);

  uint32_t t=millis();
  int code=h.POST(body);

  Serial.printf("TARS: AUDIO HTTP=%d TIME=%lu ms\n",
                code,(unsigned long)(millis()-t));

  if(code<200||code>=300){
    h.end();
    return false;
  }

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

  dec.begin();
  copier.begin(dec,*stream);
  playing=true;

  bool audioStarted=false;
  uint32_t start=millis(),lastData=start;

  while(h.connected()&&millis()-start<120000){
    bool copied=copier.copy();

    if(copied){
      lastData=millis();

      if(!audioStarted){
        audioStarted=true;
        oledStartSpeak(text);
      }
    }

    if(millis()-lastData>10000)break;
    yield();
  }

  dec.end();
  h.end();
  playing=false;
  oledSetListening();
  return true;
}

/* ================= SING ================= */
bool singRequest(String s){
  s.toLowerCase();
  return s.indexOf("nyanyi")>=0||
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
  String url=String(TARS_CLOUD_URL)+(singMode?"/sing":"/tts");
  String payload=singMode?q:answer;

  uint32_t t=millis();
  bool ok=streamAudio(url,payload);

  Serial.printf("TARS: TTS TIME=%lu ms\n",
                (unsigned long)(millis()-t));
  Serial.printf("TARS: TOTAL=%lu ms\n",
                (unsigned long)(millis()-total));

  if(!ok)oledSetStatus("AUDIO ERROR");
  singMode=false;
}

/* ================= SETUP ================= */
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

  LittleFS.begin(true);
  dacOK=initDAC();
  micOK=initMic();

  Serial.printf("TARS: DAC=%s MIC=%s\n",
                dacOK?"READY":"ERROR",
                micOK?"READY":"ERROR");

  Serial.println("TARS: PAM RIGHT GPIO26");
  Serial.println("TARS: INMP441 RIGHT GPIO34");
  Serial.println("TARS: BLUETOOTH DISABLED");
  Serial.println("TARS: MP3 STREAMING ENABLED");

  if(oledOK)
    xTaskCreatePinnedToCore(oledTask,"TARS_OLED",4096,nullptr,1,nullptr,0);

  wifiManagerBegin();

  if(bootWiFi())syncTime();

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
    if(q.length())processQuestion(q);
    else oledSetStatus("NO INPUT");
  }else{
    oledSetListening();
  }

  delay(1);
}
