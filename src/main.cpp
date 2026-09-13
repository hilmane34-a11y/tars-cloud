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
#include "TARS_ElevenLabs_MP3.h"
#include "config.h"
#include "wifi_manager.h"

#define I2S_PORT I2S_NUM_1
#define BCLK 18
#define WS 19
#define SD 34
#define DAC 26
#define RATE 16000
#define PLAY_RATE 22050
#define STT_FILE "/stt.wav"
#define PLAY_FILE "/tars.mp3"
#define IO_BUF 1024
#define DAC_BUF 1024

#define PREROLL_MS 800
#define REC_MAX 5000
#define REC_MIN 900
#define SILENCE 900
#define LISTEN_MAX 8000
#define MIC_TH 6000
#define MIC_SIL 4000

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);
bool oledOK=false,micOK=false,dacOK=false,playing=false,singMode=false,ntpOK=false;
String oledText;
size_t oledPos=0,oledPage=0;
uint32_t oledTick=0,dotTick=0;
byte dots=1;
static int16_t pre[RATE*PREROLL_MS/1000];

void oledHeader(const char*s){
 if(!oledOK)return;
 oled.clearDisplay();oled.setTextColor(SSD1306_WHITE);oled.setTextSize(1);
 oled.setCursor(42,0);oled.print("T A R S");oled.drawLine(0,9,127,9,SSD1306_WHITE);
 oled.setCursor(3,14);oled.print(s);
}
void oledBase(const char*a,const String&b=""){
 if(!oledOK)return;
 oledHeader(a);if(b.length()){oled.setCursor(3,27);oled.print(b);}oled.display();
}
void oledListen(){
 if(!oledOK||millis()-dotTick<350)return;
 dotTick=millis();dots=dots>=4?1:dots+1;
 String s;for(byte i=0;i<dots;i++)s+='.';
 oledBase("LISTENING",s);
}
void oledType(){
 if(!oledOK||oledPos>=oledText.length()||millis()-oledTick<15)return;
 oledTick=millis();oledHeader(singMode?"SINGING":"SPEAKING");
 int x=3,y=27;size_t i=oledPage;
 while(i<oledPos){
  char c=oledText[i++];
  if(c=='\n'||x>121){x=3;y+=8;if(c=='\n')continue;}
  if(y>59){oledPage=i-1;i=oledPage;x=3;y=27;oledHeader(singMode?"SINGING":"SPEAKING");}
  if(y<=59){oled.setCursor(x,y);oled.write(c);x=oled.getCursorX();}
 }
 byte n=0;
 while(oledPos<oledText.length()&&n<5){
  char c=oledText[oledPos++];
  if(c=='\n'||x>121){x=3;y+=8;if(c=='\n')continue;}
  if(y>59){oledPage=oledPos-1;x=3;y=27;oledHeader(singMode?"SINGING":"SPEAKING");continue;}
  oled.setCursor(x,y);oled.write(c);x=oled.getCursorX();n++;
 }
 oled.display();
}

/* INMP441: SCK18 WS19 SD34 / RIGHT */
bool readMic(int16_t*p,size_t maxN,size_t&n,int32_t&peak){
 int32_t raw[IO_BUF/4];size_t bytes=0;
 if(i2s_read(I2S_PORT,raw,sizeof(raw),&bytes,portMAX_DELAY)!=ESP_OK)return false;
 n=min(bytes/4,maxN);peak=0;
 for(size_t i=0;i<n;i++){
  int32_t s=raw[i]>>16;
  p[i]=constrain(s,-32768,32767);
  int32_t a=abs((int)s);if(a>peak)peak=a;
 }
 return true;
}
bool initMic(){
 i2s_config_t c={};
 c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_RX);
 c.sample_rate=RATE;c.bits_per_sample=I2S_BITS_PER_SAMPLE_32BIT;
 c.channel_format=I2S_CHANNEL_FMT_ONLY_RIGHT;
 c.communication_format=I2S_COMM_FORMAT_STAND_I2S;
 c.intr_alloc_flags=ESP_INTR_FLAG_LEVEL1;
 c.dma_buf_count=4;c.dma_buf_len=256;c.use_apll=false;
 if(i2s_driver_install(I2S_PORT,&c,0,nullptr)!=ESP_OK)return false;
 i2s_pin_config_t p={};
 p.mck_io_num=I2S_PIN_NO_CHANGE;p.bck_io_num=BCLK;p.ws_io_num=WS;
 p.data_out_num=I2S_PIN_NO_CHANGE;p.data_in_num=SD;
 if(i2s_set_pin(I2S_PORT,&p)!=ESP_OK)return false;
 i2s_zero_dma_buffer(I2S_PORT);return true;
}

/* RECORD */
bool recordSTT(){
 LittleFS.remove(STT_FILE);
 File f=LittleFS.open(STT_FILE,FILE_WRITE);if(!f)return false;
 uint8_t z[44]={0};f.write(z,44);

 int16_t pcm[IO_BUF/4];
 const size_t PN=sizeof(pre)/sizeof(pre[0]);
 size_t pp=0,pc=0,samples=0;
 bool voice=false;
 byte trigger=0;
 uint32_t wait=millis(),vs=0,lastVoice=0;

 oledBase("LISTENING","SPEAK NOW");
 Serial.println("TARS: LISTENING");

 while((!voice&&millis()-wait<LISTEN_MAX)||
       (voice&&millis()-vs<REC_MAX)){

  size_t n;int32_t peak;
  if(!readMic(pcm,IO_BUF/4,n,peak))continue;

  if(!voice){
   /* Simpan preroll terus-menerus */
   for(size_t i=0;i<n;i++){
    pre[pp]=pcm[i];
    pp=(pp+1)%PN;
    if(pc<PN)pc++;
   }

   if(peak>=MIC_TH){
    if(++trigger>=2){
     voice=true;
     vs=lastVoice=millis();

     /* Tulis hanya preroll, blok trigger tidak ditulis lagi */
     size_t st=pc==PN?pp:0;
     for(size_t i=0;i<pc;i++){
      int16_t s=pre[(st+i)%PN];
      f.write((uint8_t*)&s,2);
     }
     samples+=pc;
    }
   }else if(trigger)trigger--;
  }else{
   f.write((uint8_t*)pcm,n*2);
   samples+=n;

   if(peak>=MIC_SIL)lastVoice=millis();
   if(millis()-vs>=REC_MIN&&millis()-lastVoice>=SILENCE)break;
  }
  yield();
 }

 uint8_t h[44]={};
 auto p16=[](uint8_t*p,uint16_t v){p[0]=v;p[1]=v>>8;};
 auto p32=[](uint8_t*p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;};

 memcpy(h,"RIFF",4);p32(h+4,samples*2+36);
 memcpy(h+8,"WAVEfmt ",8);p32(h+16,16);
 p16(h+20,1);p16(h+22,1);p32(h+24,RATE);
 p32(h+28,RATE*2);p16(h+32,2);p16(h+34,16);
 memcpy(h+36,"data",4);p32(h+40,samples*2);

 f.seek(0);f.write(h,44);f.close();

 if(!voice||!samples){
  LittleFS.remove(STT_FILE);return false;
 }

 Serial.printf("TARS: RECORDED %u samples\r\n",(unsigned)samples);
 Serial.printf("TARS: WAV SIZE %u bytes\r\n",(unsigned)(samples*2+44);
 return true;
}

/* WIFI + NTP */
void ensureWiFi(){
 while(WiFi.status()!=WL_CONNECTED){
  oledBase("BOOT","WAITING WIFI...");
  if(!wifiManagerConnect(false))delay(1000);
 }
}
bool syncTime(){
 configTzTime("WIB-7","pool.ntp.org","time.google.com","time.cloudflare.com");
 for(byte a=1;a<=4;a++){
  Serial.printf("TARS: NTP %d/4\r\n",a);
  struct tm t;
  if(getLocalTime(&t,4000)&&t.tm_year>=124){
   Serial.printf("TARS: WIB %02d:%02d:%02d\r\n",t.tm_hour,t.tm_min,t.tm_sec);
   return true;
  }
  delay(500);
 }
 Serial.println("TARS: NTP FAILED");return false;
}
bool wifiOK(){
 return WiFi.status()==WL_CONNECTED||wifiManagerConnect(false);
}

/* STT */
String stt(){
 if(!wifiOK()||!ntpOK)return "";
 File f=LittleFS.open(STT_FILE);if(!f)return "";

 String b="----TARSSTT";
 String head="--"+b+"\r\nContent-Disposition: form-data; name=\"file\"; filename=\"stt.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
 String tail="\r\n--"+b+"--\r\n";

 WiFiClientSecure c;c.setInsecure();c.setTimeout(15000);

 String host=TARS_CLOUD_URL;
 int x=host.indexOf("://");
 if(x>=0)host=host.substring(x+3);
 x=host.indexOf('/');
 if(x>=0)host=host.substring(0,x);

 if(!c.connect(host.c_str(),443)){f.close();return "";}

 size_t total=head.length()+f.size()+tail.length();

 c.printf("POST /stt HTTP/1.1\r\nHost: %s\r\nContent-Type: multipart/form-data; boundary=%s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
          host.c_str(),b.c_str(),(unsigned)total);
 c.print(head);

 uint8_t z[IO_BUF];
 while(f.available()){
  size_t n=f.read(z,sizeof(z));
  if(c.write(z,n)!=n){f.close();c.stop();return "";}
 }
 f.close();c.print(tail);

 uint32_t t=millis();
 while(!c.available()&&c.connected()&&millis()-t<20000)delay(5);
 if(!c.available()){c.stop();return "";}

 String status=c.readStringUntil('\n');status.trim();
 Serial.printf("STT HTTP: %s\r\n",status.c_str());

 while(c.connected()){
  String l=c.readStringUntil('\n');
  if(l=="\r"||!l.length())break;
 }

 String body;uint32_t last=millis();
 while(c.connected()||c.available()){
  while(c.available()){body+=(char)c.read();last=millis();}
  if(millis()-last>3000)break;
  delay(1);
 }

 c.stop();body.trim();
 Serial.printf("STT BODY: %s\r\n",body.c_str());

 if(status.indexOf(" 200 ")<0)return "";

 JsonDocument j;
 if(deserializeJson(j,body)){Serial.println("STT JSON ERROR");return "";}
 String q=j["text"]|String("");q.trim();return q;
}

/* AI */
String ask(const String&q){
 if(!wifiOK()||!ntpOK)return "";
 WiFiClientSecure c;c.setInsecure();
 HTTPClient h;
 if(!h.begin(c,String(TARS_CLOUD_URL)+"/ask"))return "";
 h.setTimeout(15000);h.addHeader("Content-Type","application/json");

 JsonDocument j;j["question"]=q;
 String body;serializeJson(j,body);
 int code=h.POST(body);
 if(code<200||code>=300){h.end();return "";}

 String r=h.getString();h.end();
 JsonDocument x;
 if(deserializeJson(x,r))return "";

 String a=x["response"]|String("");
 if(!a.length())a=x["answer"]|String("");
 return a;
}

/* DAC */
bool initDAC(){pinMode(DAC,OUTPUT);dacWrite(DAC,0);return true;}
void dacMute(){dacWrite(DAC,0);}

class DACOut:public AudioStream{
 AudioInfo info;
 int16_t buf[DAC_BUF];
 volatile size_t head=0,tail=0;
 volatile bool active=false;
 TaskHandle_t task=nullptr;
 uint32_t rate=PLAY_RATE;

 size_t count(){return head>=tail?head-tail:DAC_BUF-tail+head;}
 size_t freeBuf(){return DAC_BUF-1-count();}
 static void taskFn(void*x){((DACOut*)x)->run();}

 void run(){
  uint32_t us=1000000UL/(rate?rate:PLAY_RATE),next=micros();
  while(active){
   if(head==tail){vTaskDelay(1);continue;}
   int16_t s=buf[tail];tail=(tail+1)%DAC_BUF;
   int32_t v=(int32_t)s*825/1000;
   dacWrite(DAC,constrain((v+32768+128)>>8,0,255));
   while((int32_t)(next-micros())>0)delayMicroseconds(1);
   next+=us;
  }
  dacMute();active=false;task=nullptr;vTaskDelete(nullptr);
 }

public:
 void setAudioInfo(AudioInfo i)override{
  info=i;AudioStream::setAudioInfo(i);
  rate=i.sample_rate?i.sample_rate:PLAY_RATE;
 }
 int availableForWrite()override{return freeBuf()*2;}

 void start(){
  head=tail=0;dacMute();active=true;
  if(!task)xTaskCreatePinnedToCore(taskFn,"TARS_DAC",2048,this,2,&task,1);
 }

 bool empty(){return head==tail;}

 void stop(){
  active=false;uint32_t t=millis();
  while(task&&millis()-t<2000)vTaskDelay(1);
  dacMute();
 }

 size_t write(const uint8_t*d,size_t n)override{
  if(!d||!active||info.bits_per_sample!=16)return 0;
  size_t ch=info.channels,frames=n/(ch*2),done=0;

  while(done<frames){
   size_t space=freeBuf();
   if(!space){vTaskDelay(1);continue;}

   size_t c=min(space,frames-done);

   for(size_t i=0;i<c;i++){
    size_t k=done+i;int16_t s;
    if(ch==1)s=d[k*2]|((uint16_t)d[k*2+1]<<8);
    else{
     int16_t l=d[k*4]|((uint16_t)d[k*4+1]<<8);
     int16_t r=d[k*4+2]|((uint16_t)d[k*4+3]<<8);
     s=(l+r)/2;
    }
    buf[head]=s;head=(head+1)%DAC_BUF;
   }
   done+=c;taskYIELD();
  }
  return n;
 }
};

DACOut dacOut;
MP3DecoderHelix decoder;
EncodedAudioStream mp3(&dacOut,&decoder);

/* MP3 */
bool playMP3(){
 File f=LittleFS.open(PLAY_FILE);if(!f)return false;

 playing=true;oledPos=oledPage=0;oledTick=millis();
 if(!dacOK)dacOK=initDAC();

 if(!dacOK||!mp3.begin()){
  f.close();playing=false;return false;
 }

 oledBase(singMode?"SINGING":"SPEAKING");
 dacOut.start();

 StreamCopy cp(mp3,f,IO_BUF);
 uint32_t t=millis();

 while(f.available()&&millis()-t<120000){
  cp.copy();oledType();yield();
 }

 mp3.end();f.close();

 while(!dacOut.empty()){oledType();delay(1);}

 dacOut.stop();playing=false;
 LittleFS.remove(PLAY_FILE);
 oledBase("STANDBY","LISTENING...");
 return true;
}

bool downloadMP3(const String&url,const String&text){
 if(!wifiOK()||!ntpOK)return false;

 WiFiClientSecure c;c.setInsecure();
 HTTPClient h;
 if(!h.begin(c,url))return false;

 h.setTimeout(60000);h.addHeader("Content-Type","application/json");

 JsonDocument j;
 if(url.endsWith("/sing"))j["prompt"]=text;
 else j["text"]=text;

 String body;serializeJson(j,body);
 int code=h.POST(body);

 if(code<200||code>=300){h.end();return false;}

 LittleFS.remove(PLAY_FILE);
 File f=LittleFS.open(PLAY_FILE,FILE_WRITE);
 if(!f){h.end();return false;}

 WiFiClient*s=h.getStreamPtr();
 uint8_t z[IO_BUF];
 int len=h.getSize();
 size_t total=0;
 uint32_t last=millis();

 while(h.connected()&&(len>0||len==-1)){
  size_t n=s->available();

  if(n){
   n=min(n,sizeof(z));
   int r=s->readBytes(z,n);
   if(r>0){
    f.write(z,r);total+=r;
    if(len>0)len-=r;
    last=millis();
   }
  }else{
   if(millis()-last>10000)break;
   delay(1);
  }
  yield();
 }

 f.close();h.end();
 return total>0;
}

bool embeddedMP3(){
 LittleFS.remove(PLAY_FILE);
 File f=LittleFS.open(PLAY_FILE,FILE_WRITE);
 if(!f)return false;
 size_t n=f.write(TARS_ELEVENLABS_MP3,TARS_ELEVENLABS_MP3_LEN);
 f.close();
 return n==TARS_ELEVENLABS_MP3_LEN;
}

/* COMMAND */
bool isSing(const String&q){
 String s=q;s.toLowerCase();
 return s.indexOf("nyanyi")>=0||
        s.indexOf("bernyanyi")>=0||
        s.indexOf("nyanyikan")>=0;
}

bool hasTARSStart(const String&q){
 String s=q;s.trim();s.toLowerCase();
 return s=="tars"||s.startsWith("tars ")||s.startsWith("tars,")||
        s.startsWith("tars?")||s.startsWith("tars.")||s.startsWith("tars!");
}

bool hasTARSEnd(const String&q){
 String s=q;s.trim();s.toLowerCase();
 return s=="tars"||s.endsWith(" tars")||s.endsWith(" tars?")||
        s.endsWith(" tars.")||s.endsWith(" tars!")||s.endsWith(" tars,");
}

bool calledTARS(const String&q){return hasTARSStart(q)||hasTARSEnd(q);}

String cleanTARS(String q){
 q.trim();
 if(hasTARSStart(q))q=q.substring(4);
 else if(hasTARSEnd(q))q=q.substring(0,q.length()-4);
 q.trim();
 while(q.length()&&strchr(" ,.!?",q[0]))q.remove(0,1);
 q.trim();
 return q;
}

/* LOCAL TIME */
String localTimeText(){
 time_t n=time(nullptr);
 struct tm t;localtime_r(&n,&t);

 const char*d[]={"Minggu","Senin","Selasa","Rabu","Kamis","Jumat","Sabtu"};
 const char*m[]={"Januari","Februari","Maret","April","Mei","Juni","Juli",
                 "Agustus","September","Oktober","November","Desember"};

 char s[150];
 snprintf(s,sizeof(s),
  "Pukul %02d lewat %02d menit WIB. %s, %02d %s tahun %04d.",
  t.tm_hour,t.tm_min,t.tm_wday>=0?d[t.tm_wday]:"",
  t.tm_mday,m[t.tm_mon],t.tm_year+1900);
 return String(s);
}

/* ANSWER */
void answer(const String&q){
 String l=q;l.toLowerCase();l.trim();

 if(l.indexOf("hidup jokowi")>=0){
  Serial.println("TARS: HIDUP JOKOWI -> OFFLINE");
  Serial.println("TARS: Saya akan lawan.");

  singMode=false;oledText="Saya akan lawan.";
  oledPos=oledPage=0;

  if(embeddedMP3())playMP3();
  else oledBase("STANDBY","AUDIO ERROR");
  return;
 }

 if(!calledTARS(q)){
  Serial.println("TARS: UNKNOWN -> BLOCKED");
  oledBase("UNKNOWN","BLOCKED");
  delay(500);
  oledBase("STANDBY","LISTENING...");
  return;
 }

 String clean=cleanTARS(q);
 if(!clean.length()){oledBase("UNKNOWN","SAY SOMETHING");return;}

 String lq=clean;lq.toLowerCase();

 if(lq.indexOf("jam")>=0||lq.indexOf("waktu")>=0||
    lq.indexOf("tanggal")>=0||lq.indexOf("hari")>=0){
  Serial.println("TARS: TIME -> LOCAL");

  if(!ntpOK){oledBase("STANDBY","TIME ERROR");return;}

  singMode=false;oledText=localTimeText();
  oledPos=oledPage=0;
  oledBase("TIME",oledText);
  return;
 }

 singMode=isSing(clean);

 if(singMode){
  Serial.println("TARS: ONLINE SING");
  if(downloadMP3(String(TARS_CLOUD_URL)+"/sing",clean))
   playMP3();
  else oledBase("STANDBY","AUDIO ERROR");
  singMode=false;
  return;
 }

 Serial.println("TARS: ONLINE AI");

 String a=ask(clean);
 if(!a.length()){oledBase("STANDBY","ASK ERROR");return;}

 oledText=a;oledPos=oledPage=0;

 if(downloadMP3(String(TARS_CLOUD_URL)+"/tts",a))
  playMP3();
 else oledBase("STANDBY","AUDIO ERROR");

 singMode=false;
}

/* SETUP */
void setup(){
 Serial.begin(SERIAL_BAUD);

 Wire.begin(OLED_SDA,OLED_SCL);
 Wire.setClock(400000);

 oledOK=oled.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR);
 if(oledOK)oledBase("BOOT");

 LittleFS.begin(true);

 dacOK=initDAC();
 micOK=initMic();

 Serial.println("TARS: BLUETOOTH DISABLED");
 Serial.println("TARS: DIRECT STT MODE");
 Serial.println("TARS: VOICE -> STT -> TEXT");
 Serial.println("TARS: STT LANGUAGE = INDONESIAN");
 Serial.println("TARS: TARS = ONLINE");
 Serial.println("TARS: HIDUP JOKOWI = OFFLINE");
 Serial.println("TARS: OTHER SPEECH = BLOCKED");

 wifiManagerBegin();
 ensureWiFi();

 while(!syncTime()){
  oledBase("BOOT","NTP RETRY...");
  delay(2000);
  ensureWiFi();
 }

 ntpOK=true;

 Serial.println("TARS: NTP READY WIB");
 Serial.println("TARS: STT READY");

 oledBase("STANDBY","LISTENING...");
}

/* LOOP */
void loop(){
 if(playing)return;

 if(WiFi.status()!=WL_CONNECTED)
  ensureWiFi();

 if(!recordSTT()){
  oledListen();
  return;
 }

 Serial.println("TARS: SENDING AUDIO TO STT...");
 oledBase("STT","PROCESSING...");

 String q=stt();
 LittleFS.remove(STT_FILE);

 if(!q.length()){
  Serial.println("STT: [NO TEXT]");
  oledBase("STANDBY","NO TEXT");
  delay(300);
  return;
 }

 Serial.println("================================");
 Serial.printf("STT: %s\r\n",q.c_str());
 Serial.println("================================");

 answer(q);
 delay(1);
}
