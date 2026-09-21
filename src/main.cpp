#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Wire.h>
#include <time.h>
#include <math.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <esp_sntp.h>
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

const uint32_t MIC_RATE=16000,RECORD_MIN_MS=500,SILENCE_MS=1000,PREROLL_MS=700;
const uint32_t OLED_TYPE_MS=39,OLED_WAVE_MS=70,AUDIO_IDLE_MS=2500,OLED_PAGE_MS=2200;
const uint32_t STREAM_EOF_IDLE_MS=5000;
const int32_t MIC_THRESHOLD=10500,MIC_SILENCE=8000;
const size_t BUF=512,PREROLL_SAMPLES=MIC_RATE*PREROLL_MS/1000;
const int MP3_COPY_BUFFER=512;
const float MP3_VOLUME=.67f;
const size_t AUDIO_RING_SIZE=8192,AUDIO_PREBUFFER=2048;
const char* STT_HOST="tars-cloud-v1.hilmane34.workers.dev";
const uint32_t ALARM_DURATION_MS=120000;
bool alarmRunning=false;int alarmLastDay=-1;

extern const uint8_t alarm_start[] asm("_binary_src_alarm_mp3_start");
extern const uint8_t alarm_end[] asm("_binary_src_alarm_mp3_end");

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);
AnalogAudioStream analog;
MP3DecoderHelix codec;
WAVDecoder wav;
WebSocketsClient sttWS;

bool oledOK=false,micOK=false,dacOK=false,playing=false,ntpOK=false;
bool sttConnected=false,sttReady=false,sttDone=false,sttError=false;
volatile bool ntpSyncEvent=false;
String sttFinal,sttPartial,oledText,oledStatus="READY";
uint32_t oledTypePos=0,oledLastType=0,oledLastWave=0,oledPage=0,oledLastPage=0;
static int32_t rawBuf[BUF/4];
static int16_t pcmBuf[BUF/4],preBuf[PREROLL_SAMPLES],sendBuf[256];

class AlarmStream:public Stream{
  size_t pos=0;
public:
  void begin(){pos=0;}
  int available()override{return (int)(alarm_end-alarm_start-pos);}
  int read()override{return available()?alarm_start[pos++]:-1;}
  int read(uint8_t*b,size_t n){
    size_t left=alarm_end-alarm_start-pos;n=min(n,left);
    if(n){memcpy(b,alarm_start+pos,n);pos+=n;}return n;
  }
  int peek()override{return available()?alarm_start[pos]:-1;}
  void flush()override{}
  size_t write(uint8_t)override{return 0;}
  size_t write(const uint8_t*,size_t)override{return 0;}
}alarmStream;

class AudioRingStream:public Stream{
  uint8_t b[AUDIO_RING_SIZE];volatile size_t h=0,t=0,n=0;
  volatile bool done=false,stopFlag=false,running=false;
  TaskHandle_t task=nullptr;portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
  WiFiClient*src=nullptr;int expected=-1,received=0;uint32_t lastRx=0;bool gotData=false;
  static void entry(void*p){((AudioRingStream*)p)->rx();vTaskDelete(nullptr);}
  void rx(){
    uint8_t tmp[1024];lastRx=millis();gotData=false;
    while(!stopFlag){
      int av=src?src->available():0;
      if(av){
        size_t f=AUDIO_RING_SIZE-available();if(!f){vTaskDelay(1);continue;}
        size_t want=min((size_t)av,sizeof(tmp));want=min(want,f);
        int r=src->read(tmp,want);
        if(r>0){push(tmp,r);received+=r;lastRx=millis();gotData=true;if(expected>=0&&received>=expected)break;}
      }else{
        if(expected>=0&&received>=expected)break;
        if(expected<0&&gotData&&millis()-lastRx>=STREAM_EOF_IDLE_MS)break;
        if(src&&!src->connected()&&gotData)break;vTaskDelay(1);
      }
    }
    portENTER_CRITICAL(&mux);done=true;running=false;portEXIT_CRITICAL(&mux);task=nullptr;
  }
  size_t push(const uint8_t*p,size_t x){
    if(!p||!x)return 0;portENTER_CRITICAL(&mux);
    size_t f=AUDIO_RING_SIZE-n;x=min(x,f);size_t z=min(x,AUDIO_RING_SIZE-h);
    memcpy(b+h,p,z);if(x>z)memcpy(b,p+z,x-z);h=(h+x)%AUDIO_RING_SIZE;n+=x;
    portEXIT_CRITICAL(&mux);return x;
  }
public:
  void start(WiFiClient&s,int len=-1){
    stop();portENTER_CRITICAL(&mux);h=t=n=0;done=false;stopFlag=false;running=true;portEXIT_CRITICAL(&mux);
    src=&s;expected=len;received=0;lastRx=millis();gotData=false;
    xTaskCreatePinnedToCore(entry,"TARS_RX",3072,this,2,&task,0);
  }
  void stop(){
    stopFlag=true;uint32_t st=millis();while(running&&millis()-st<1500)vTaskDelay(1);
    if(task){vTaskDelete(task);task=nullptr;}
    portENTER_CRITICAL(&mux);running=false;done=true;portEXIT_CRITICAL(&mux);src=nullptr;
  }
  bool finished(){return done&&available()==0;}
  int available()override{portENTER_CRITICAL(&mux);int r=n;portEXIT_CRITICAL(&mux);return r;}
  int read()override{uint8_t c;return read(&c,1)==1?c:-1;}
  int read(uint8_t*p,size_t x){
    if(!p||!x)return 0;portENTER_CRITICAL(&mux);size_t take=min((size_t)n,x);
    if(take){size_t z=min(take,AUDIO_RING_SIZE-t);memcpy(p,b+t,z);if(take>z)memcpy(p+z,b,take-z);t=(t+take)%AUDIO_RING_SIZE;n-=take;}
    portEXIT_CRITICAL(&mux);return take;
  }
  int peek()override{portENTER_CRITICAL(&mux);int r=n?b[t]:-1;portEXIT_CRITICAL(&mux);return r;}
  void flush()override{portENTER_CRITICAL(&mux);h=t=n=0;portEXIT_CRITICAL(&mux);}
  size_t write(uint8_t)override{return 0;}
  size_t write(const uint8_t*,size_t)override{return 0;}
}audioRing;

class PCMProbeStream:public AudioStream{
  AudioStream*out;AudioInfo info;uint64_t decBytes=0,dacBytes=0;uint32_t t0=0;
public:
  PCMProbeStream(AudioStream&o):out(&o){}
  bool begin()override{return true;}void end()override{}
  void reset(){decBytes=dacBytes=0;t0=0;info=AudioInfo();}
  void setAudioInfo(AudioInfo x)override{
    AudioStream::setAudioInfo(x);info=x;out->setAudioInfo(x);
    Serial.printf("TARS: PCM->DAC %lu Hz / %d ch / %d bit\n",(unsigned long)x.sample_rate,x.channels,x.bits_per_sample);
  }
  size_t write(const uint8_t*p,size_t n)override{
    if(!p||!n)return 0;if(!t0)t0=micros();decBytes+=n;size_t d=0;
    while(d<n){size_t w=out->write(p+d,n-d);if(w)d+=w;else{delay(1);yield();}}
    dacBytes+=d;return d;
  }
  int availableForWrite()override{return out->availableForWrite();}
  void report(){Serial.printf("TARS: PCM BYTES=%llu DAC=%llu\n",(unsigned long long)decBytes,(unsigned long long)dacBytes);}
};

PCMProbeStream pcmProbe(analog);
ResampleStream mp3Resample(pcmProbe);
EncodedAudioStream dec(&mp3Resample,&codec);
EncodedAudioStream wavDec(&pcmProbe,&wav);
StreamCopy copier(MP3_COPY_BUFFER);

void oledSetStatus(const String&s){oledStatus=s;oledText="";oledTypePos=0;oledPage=0;oledLastPage=millis();}
void oledSetListening(){oledSetStatus("LISTENING");}
void oledStartSpeak(const String&s){oledStatus="SPEAKING";oledText=s;oledTypePos=0;oledPage=0;oledLastType=millis();oledLastPage=millis();}

void oledTask(void*){
  for(;;){
    if(!oledOK){vTaskDelay(50);continue;}uint32_t now=millis();
    if(oledText.length()&&oledTypePos<oledText.length()&&now-oledLastType>=OLED_TYPE_MS)oledTypePos++,oledLastType=now;
    if(now-oledLastWave>=OLED_WAVE_MS){
      oledLastWave=now;oled.clearDisplay();oled.setTextColor(SSD1306_WHITE);
      oled.setTextSize(2);oled.setCursor(36,0);oled.print("TARS");
      oled.setTextSize(1);oled.setCursor(3,17);oled.print(oledStatus);
      if(oledText.length()){
        String s=oledText.substring(0,min(oledTypePos,(uint32_t)oledText.length()));
        uint32_t lineNo=0,target=oledPage*4;uint8_t shown=0;String line;bool next=false;
        for(size_t i=0;i<=s.length();i++){
          char c=i<s.length()?s[i]:'\0';
          if(c=='\n'||c=='\0'){
            if(lineNo>=target&&shown<4)oled.setCursor(3,29+shown++*8),oled.print(line);
            line="";lineNo++;if(shown>=4){next=i<s.length();break;}continue;
          }
          line+=c;
          if(line.length()>=20){
            int cut=line.lastIndexOf(' ');
            if(cut>0){String rest=line.substring(cut+1);line=line.substring(0,cut);
              if(lineNo>=target&&shown<4)oled.setCursor(3,29+shown++*8),oled.print(line);
              line=rest;lineNo++;if(shown>=4){next=i+1<s.length();break;}}
          }
        }
        if(oledStatus=="SPEAKING"&&now-oledLastPage>=OLED_PAGE_MS){if(next)oledPage++;oledLastPage=now;}
      }
      if(oledStatus=="LISTENING"){int x=64+(int)(sin(now/120.0)*25);oled.drawCircle(x,56,4,SSD1306_WHITE);}
      else if(oledStatus=="SPEAKING"){int w=8+(now/40)%18;oled.fillRect(64-w/2,51,w,6,SSD1306_WHITE);}
      oled.display();
    }vTaskDelay(10);
  }
}

bool initDAC(){
  auto cfg=analog.defaultConfig(TX_MODE);cfg.sample_rate=44100;cfg.channels=1;cfg.bits_per_sample=16;
  if(!analog.begin(cfg)){Serial.println("TARS: DAC ERROR");return false;}
  dec.addNotifyAudioChange(mp3Resample);mp3Resample.addNotifyAudioChange(pcmProbe);wavDec.addNotifyAudioChange(pcmProbe);
  Serial.println("TARS: DAC GPIO26 RIGHT READY");Serial.println("TARS: MP3 44.1k -> RESAMPLE 22.05k/16bit -> GPIO26");Serial.println("TARS: WAV -> PCM -> GPIO26");
  return true;
}

bool initMic(){
  i2s_config_t c={};c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_RX);c.sample_rate=MIC_RATE;
  c.bits_per_sample=I2S_BITS_PER_SAMPLE_32BIT;c.channel_format=I2S_CHANNEL_FMT_ONLY_RIGHT;
  c.communication_format=I2S_COMM_FORMAT_STAND_I2S;c.intr_alloc_flags=ESP_INTR_FLAG_LEVEL1;
  c.dma_buf_count=2;c.dma_buf_len=256;c.use_apll=false;c.tx_desc_auto_clear=false;c.fixed_mclk=0;
  if(i2s_driver_install(MIC_PORT,&c,0,nullptr)!=ESP_OK)return false;
  i2s_pin_config_t p={};p.bck_io_num=MIC_SCK;p.ws_io_num=MIC_WS;p.data_out_num=I2S_PIN_NO_CHANGE;p.data_in_num=MIC_SD;
  if(i2s_set_pin(MIC_PORT,&p)!=ESP_OK)return false;i2s_zero_dma_buffer(MIC_PORT);
  Serial.println("TARS: INMP441 RIGHT READY");return true;
}

bool wifiOK(){return WiFi.status()==WL_CONNECTED||(wifiManagerConnect(false)&&WiFi.status()==WL_CONNECTED);}

bool bootWiFi(){
  oledSetStatus("WIFI CONNECTING");Serial.println("TARS: WIFI CONNECTING...");
  uint32_t st=millis();while(WiFi.status()!=WL_CONNECTED&&millis()-st<30000){wifiManagerConnect(false);delay(100);yield();}
  if(WiFi.status()!=WL_CONNECTED){Serial.println("TARS: WIFI BOOT FAILED");oledSetStatus("WIFI FAILED");return false;}
  Serial.print("TARS: WIFI CONNECTED IP=");Serial.println(WiFi.localIP());oledSetStatus("WIFI CONNECTED");delay(1200);return true;
}

void ntpCallback(struct timeval*){ntpSyncEvent=true;}

bool syncTime(){
  if(ntpOK)return true;ntpSyncEvent=false;sntp_set_time_sync_notification_cb(ntpCallback);sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  Serial.println("TARS: NTP START");oledSetStatus("NTP START");
  configTime(7*3600,0,"pool.ntp.org","time.google.com","time.cloudflare.com");
  for(int a=1;a<=4;a++){
    Serial.printf("TARS: NTP ATTEMPT %d/4\n",a);oledSetStatus("NTP "+String(a)+"/4");uint32_t st=millis();
    while(millis()-st<10000){
      if(ntpSyncEvent||sntp_get_sync_status()==SNTP_SYNC_STATUS_COMPLETED){
        time_t now=time(nullptr);
        if(now>=1704067200){
          struct tm t;localtime_r(&now,&t);
          Serial.printf("TARS: NTP VALID %04d-%02d-%02d %02d:%02d:%02d\n",t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec);
          ntpOK=true;
          String d=String(t.tm_mday<10?"0":"")+String(t.tm_mday)+"/"+String(t.tm_mon+1<10?"0":"")+String(t.tm_mon+1)+"/"+String(t.tm_year+1900)+" "+String(t.tm_hour<10?"0":"")+String(t.tm_hour)+":"+String(t.tm_min<10?"0":"")+String(t.tm_min)+":"+String(t.tm_sec<10?"0":"")+String(t.tm_sec);
          oledSetStatus("NTP OK");oledText=d;oledTypePos=d.length();delay(2500);return true;
        }
      }
      delay(100);yield();
    }
    if(a<4){Serial.println("TARS: NTP RETRY");oledSetStatus("NTP RETRY");ntpSyncEvent=false;sntp_restart();delay(1000);}
  }
  Serial.println("TARS: NTP FAILED 4/4");oledSetStatus("NTP FAILED");delay(1500);return false;
}

void sttEvent(WStype_t type,uint8_t*payload,size_t length){
  if(type==WStype_CONNECTED){sttConnected=true;Serial.println("TARS: STT WS CONNECTED");oledSetStatus("STT CONNECTED");return;}
  if(type==WStype_DISCONNECTED){sttConnected=false;if(!sttDone)sttError=true;Serial.println("TARS: STT WS DISCONNECTED");oledSetStatus("STT DISCONNECTED");return;}
  if(type==WStype_ERROR){sttError=true;Serial.println("TARS: STT WS ERROR");oledSetStatus("STT ERROR");return;}
  if(type!=WStype_TEXT)return;
  String msg;msg.reserve(length+1);for(size_t i=0;i<length;i++)msg+=(char)payload[i];
  JsonDocument j;if(deserializeJson(j,msg))return;String t=j["type"].as<String>();
  if(t=="ready"){sttReady=true;Serial.println("TARS: STT REALTIME READY");oledSetStatus("STT READY");}
  else if(t=="partial"){sttPartial=j["text"].as<String>();sttPartial.trim();if(sttPartial.length()){Serial.print("TARS: STT PARTIAL = ");Serial.println(sttPartial);}}
  else if(t=="final"){sttFinal=j["text"].as<String>();sttFinal.trim();sttDone=true;Serial.print("TARS: YOU SAID = ");Serial.println(sttFinal);}
  else if(t=="error"){sttError=true;sttDone=true;Serial.print("TARS: STT ERROR = ");Serial.println(j["error"].as<String>());oledSetStatus("STT ERROR");}
}

bool startSTT(){
  if(!wifiOK())return false;
  sttConnected=sttReady=sttDone=sttError=false;sttFinal="";sttPartial="";
  sttWS.disconnect();sttWS.onEvent(sttEvent);sttWS.setReconnectInterval(0);sttWS.enableHeartbeat(15000,5000,2);sttWS.beginSSL(STT_HOST,443,"/stt");
  uint32_t st=millis();while(!sttReady&&!sttError&&millis()-st<20000){sttWS.loop();delay(2);yield();}
  if(!sttReady){Serial.println("TARS: STT REALTIME TIMEOUT");sttWS.disconnect();return false;}return true;
}

String stopSTT(uint32_t samples){
  if(!sttConnected)return "";JsonDocument j;j["type"]="end";j["timestamp"]=(double)samples/MIC_RATE;
  String msg;serializeJson(j,msg);sttWS.sendTXT(msg);Serial.println("TARS: STT END SENT");
  uint32_t st=millis();while(!sttDone&&!sttError&&millis()-st<6000){sttWS.loop();delay(2);yield();}
  String r=sttFinal;sttWS.disconnect();return r;
}

String recordRealtime(){
  if(!micOK||!startSTT())return "";oledSetListening();
  size_t prePos=0,preCount=0;uint32_t voiceStart=0,lastVoice=0,samples=0;bool voice=false;
  Serial.println("TARS: REALTIME LISTENING");
  for(;;){
    sttWS.loop();if(sttError)break;size_t bytes=0;
    if(i2s_read(MIC_PORT,rawBuf,sizeof(rawBuf),&bytes,pdMS_TO_TICKS(30))!=ESP_OK)continue;
    size_t count=bytes/4;int32_t peak=0;uint64_t sum=0;
    for(size_t i=0;i<count;i++){int32_t v=constrain(rawBuf[i]>>16,-32768,32767);pcmBuf[i]=(int16_t)v;int32_t a=abs(v);if(a>peak)peak=a;sum+=(uint64_t)a*a;}
    uint32_t rms=count?(uint32_t)sqrt((double)sum/count):0;
    if(!voice){
      for(size_t i=0;i<count;i++){preBuf[prePos]=pcmBuf[i];prePos=(prePos+1)%PREROLL_SAMPLES;if(preCount<PREROLL_SAMPLES)preCount++;}
      if(peak>=MIC_THRESHOLD||rms>=1800){
        voice=true;voiceStart=lastVoice=millis();size_t start=preCount==PREROLL_SAMPLES?prePos:0,nsend=0;
        for(size_t i=0;i<preCount;i++){sendBuf[nsend++]=preBuf[(start+i)%PREROLL_SAMPLES];if(nsend==256){if(!sttWS.sendBIN((uint8_t*)sendBuf,nsend*2)){sttError=true;break;}nsend=0;}}
        if(nsend&&!sttError)sttWS.sendBIN((uint8_t*)sendBuf,nsend*2);samples+=preCount;
        Serial.printf("TARS: VOICE DETECTED PEAK=%ld RMS=%lu\n",(long)peak,(unsigned long)rms);
      }
    }else{
      if(!sttWS.sendBIN((uint8_t*)pcmBuf,count*2)){Serial.println("TARS: STT PCM SEND FAILED");sttError=true;break;}
      samples+=count;if(peak>=MIC_SILENCE||rms>=1200)lastVoice=millis();
      if(millis()-voiceStart>=RECORD_MIN_MS&&millis()-lastVoice>=SILENCE_MS)break;
    }
    yield();
  }
  if(!voice||sttError){sttWS.disconnect();if(!voice)Serial.println("TARS: MIC AUDIO TOO LOW");return "";}
  return stopSTT(samples);
}

String ask(const String&q){
  if(!wifiOK())return "";WiFiClientSecure c;c.setInsecure();HTTPClient h;
  if(!h.begin(c,String(TARS_CLOUD_URL)+"/ask"))return "";h.setTimeout(12000);h.addHeader("Content-Type","application/json");
  JsonDocument j;j["question"]=q;String body;serializeJson(j,body);uint32_t st=millis();int code=h.POST(body);
  Serial.printf("TARS: ASK HTTP=%d TIME=%lu ms\n",code,(unsigned long)(millis()-st));
  if(code<200||code>=300){h.end();return "";}String r=h.getString();h.end();JsonDocument x;if(deserializeJson(x,r))return "";
  String s=x["response"].as<String>();s.trim();return s;
}

bool streamAudio(const String&url,const String&text){
  if(!wifiOK())return false;WiFiClientSecure c;c.setInsecure();c.setTimeout(20000);HTTPClient h;uint32_t total=millis();
  if(!h.begin(c,url))return false;h.setTimeout(20000);h.addHeader("Content-Type","application/json");
  const char*keys[]={"Content-Type","X-TARS-TTS","X-TARS-TTS-FORMAT"};h.collectHeaders(keys,3);
  JsonDocument j;j["text"]=text;String body;serializeJson(j,body);uint32_t st=millis();int code=h.POST(body);
  Serial.printf("TARS: AUDIO HTTP=%d TIME=%lu ms\n",code,(unsigned long)(millis()-st));
  if(code<200||code>=300){h.end();return false;}
  String ct=h.header("Content-Type"),fmt=h.header("X-TARS-TTS-FORMAT"),engine=h.header("X-TARS-TTS");ct.toLowerCase();
  Serial.print("TARS: TTS CONTENT-TYPE=");Serial.println(ct);Serial.print("TARS: TTS STATUS=");Serial.println(engine);Serial.print("TARS: TTS FORMAT=");Serial.println(fmt);
  WiFiClient*stream=h.getStreamPtr();if(!stream){h.end();return false;}if(!dacOK)dacOK=initDAC();if(!dacOK){h.end();return false;}
  int contentLen=h.getSize();Serial.printf("TARS: AUDIO CONTENT-LENGTH=%d\n",contentLen);audioRing.start(*stream,contentLen);playing=true;
  bool isWav=ct.indexOf("wav")>=0||fmt.equalsIgnoreCase("WAV");
  if(!isWav){
    Serial.println("TARS: MP3 AUTO FORMAT");Serial.println("TARS: MP3 RESAMPLE TARGET=22050 Hz / 16 bit");
    size_t target=AUDIO_PREBUFFER;if(contentLen>0)target=min(target,(size_t)contentLen);uint32_t ps=millis();
    while(audioRing.available()<(int)target&&!audioRing.finished()){if(millis()-ps>10000){Serial.println("TARS: MP3 PREBUFFER TIMEOUT");audioRing.stop();h.end();playing=false;return false;}delay(1);yield();}
    int pre=audioRing.available();if(!pre){audioRing.stop();h.end();playing=false;Serial.println("TARS: MP3 EMPTY");return false;}
    Serial.printf("TARS: MP3 PREBUFFER=%d/%u BYTES\n",pre,(unsigned)target);pcmProbe.reset();dec.begin();AudioInfo src=codec.audioInfo();
    if(!mp3Resample.begin(src,22050)){Serial.println("TARS: MP3 RESAMPLER ERROR");dec.end();audioRing.stop();h.end();playing=false;return false;}
    Serial.printf("TARS: MP3 SOURCE %lu Hz / %d ch / %d bit\n",(unsigned long)src.sample_rate,src.channels,src.bits_per_sample);
    Serial.println("TARS: MP3 TARGET 22050 Hz / 16 bit");copier.begin(dec,audioRing);bool started=false;uint32_t start=millis();
    while(true){int before=audioRing.available();bool copied=copier.copy();int after=audioRing.available();if(copied&&!started){started=true;oledStartSpeak(text);}
      if(audioRing.finished()&&audioRing.available()==0)break;if(before==after)yield();}
    mp3Resample.flush();audioRing.stop();dec.end();mp3Resample.end();pcmProbe.report();h.end();playing=false;
    Serial.printf("TARS: AUDIO STREAM=%lu ms\n",(unsigned long)(millis()-start));Serial.printf("TARS: AUDIO TOTAL=%lu ms\n",(unsigned long)(millis()-total));oledSetListening();return started;
  }
  Serial.println("TARS: WAV AUTO FORMAT");oledStartSpeak(text);pcmProbe.reset();wavDec.begin();copier.begin(wavDec,audioRing);
  bool started=false;uint32_t start=millis();
  while(true){int before=audioRing.available();bool copied=copier.copy();int after=audioRing.available();
    if(copied&&!started){started=true;AudioInfo ai=wav.audioInfo();Serial.printf("TARS: WAV %lu Hz / %d ch / %d bit\n",(unsigned long)ai.sample_rate,ai.channels,ai.bits_per_sample);}
    if(audioRing.finished()&&audioRing.available()==0)break;if(before==after)yield();}
  audioRing.stop();wavDec.end();pcmProbe.report();h.end();playing=false;
  Serial.printf("TARS: WAV STREAM=%lu ms\n",(unsigned long)(millis()-start));Serial.printf("TARS: AUDIO TOTAL=%lu ms\n",(unsigned long)(millis()-total));oledSetListening();return started;
}

/* ===== STATUS TARS ===== */
bool isStatusQuery(const String&q){
  String s=q;s.toLowerCase();
  return s.indexOf("cek status")>=0||s.indexOf("status kamu")>=0||s.indexOf("status tars")>=0||
         s.indexOf("kondisi kamu")>=0||s.indexOf("kondisi tars")>=0;
}

String systemStatus(){
  String s="DATA STATUS TARS SAAT INI:\n";
  s+="RAM bebas "+String(ESP.getFreeHeap()/1024.0,1)+" KB, minimum "+String(ESP.getMinFreeHeap()/1024.0,1)+" KB, blok terbesar "+String(ESP.getMaxAllocHeap()/1024.0,1)+" KB.\n";
  s+="Flash "+String(ESP.getFlashChipSize()/1024.0/1024.0,1)+" MB, sketch "+String(ESP.getSketchSize()/1024.0,1)+" KB, ruang sketch bebas "+String(ESP.getFreeSketchSpace()/1024.0,1)+" KB.\n";
  if(LittleFS.begin(true))s+="LittleFS total "+String(LittleFS.totalBytes()/1024.0,1)+" KB, terpakai "+String(LittleFS.usedBytes()/1024.0,1)+" KB.\n";
  s+="CPU "+String(getCpuFrequencyMhz())+" MHz, uptime "+String(millis()/3600000UL)+" jam "+String((millis()/60000UL)%60)+" menit.\n";
  s+="Suhu ESP32 "+String(temperatureRead(),1)+" C.\n";
  s+="WiFi "+String(WiFi.status()==WL_CONNECTED?"terhubung":"terputus");
  if(WiFi.status()==WL_CONNECTED)s+="; RSSI "+String(WiFi.RSSI())+" dBm; IP "+WiFi.localIP().toString();
  s+=".\n";
  s+="NTP "+String(ntpOK?"valid":"belum valid")+", OLED "+String(oledOK?"aktif":"error")+
     ", mic "+String(micOK?"aktif":"error")+", DAC "+String(dacOK?"aktif":"error")+
     ", audio "+String(playing?"sedang berjalan":"idle")+
     ", STT "+String(sttReady?"ready":(sttConnected?"connected":"idle"))+".";
  return s;
}

/* ===== ALARM OFFLINE ===== */
bool alarmDue(){
  if(!ntpOK||alarmRunning)return false;
  time_t now=time(nullptr);if(now<1704067200)return false;
  struct tm t;localtime_r(&now,&t);
  return t.tm_hour==6&&t.tm_min==0&&alarmLastDay!=t.tm_yday;
}

bool playLocalAlarm(){
  if(!dacOK)dacOK=initDAC();if(!dacOK)return false;
  Serial.printf("TARS: OFFLINE ALARM PLAY %u BYTES\n",(unsigned)(alarm_end-alarm_start));
  oledSetStatus("ALARM");playing=true;alarmStream.begin();pcmProbe.reset();
  dec.begin();AudioInfo src=codec.audioInfo();
  bool ok=mp3Resample.begin(src,22050);
  if(ok){
    copier.begin(dec,alarmStream);oledStartSpeak("Tuan, waktunya bangun.");
    while(alarmStream.available()>0)copier.copy();
    mp3Resample.flush();mp3Resample.end();
  }
  dec.end();pcmProbe.report();playing=false;oledSetListening();
  Serial.println(ok?"TARS: OFFLINE ALARM DONE":"TARS: OFFLINE ALARM ERROR");
  return ok;
}

void runAlarm(){
  if(!alarmDue())return;
  time_t now=time(nullptr);struct tm t;localtime_r(&now,&t);
  alarmLastDay=t.tm_yday;alarmRunning=true;Serial.println("TARS: ALARM 06:00 WIB");
  uint32_t st=millis();
  while(millis()-st<ALARM_DURATION_MS){if(!playLocalAlarm())break;delay(500);}
  playing=false;alarmRunning=false;oledSetListening();Serial.println("TARS: ALARM SELESAI");
}

void processQuestion(const String&q){
  String askQ=q;
  if(isStatusQuery(q))askQ="Tuan meminta laporan status sistem TARS. Gunakan DATA STATUS berikut dan jelaskan seluruh kondisi secara singkat, natural, dan mudah dipahami.\n"+systemStatus();
  String answer=ask(askQ);if(!answer.length()){oledSetStatus("ASK ERROR");return;}
  uint32_t st=millis();bool ok=streamAudio(String(TARS_CLOUD_URL)+"/tts",answer);
  Serial.printf("TARS: AUDIO FUNCTION TIME=%lu ms\n",(unsigned long)(millis()-st));
  oledSetStatus(ok?"LISTENING":"AUDIO ERROR");
}

void setup(){
  Serial.begin(SERIAL_BAUD);Wire.begin(OLED_SDA,OLED_SCL);Wire.setClock(400000);
  oledOK=oled.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR);
  if(oledOK){oled.clearDisplay();oled.setTextColor(SSD1306_WHITE);oled.setTextSize(2);oled.setCursor(36,0);oled.print("TARS");oled.setTextSize(1);oled.setCursor(3,27);oled.print("BOOT");oled.display();}
  dacOK=initDAC();micOK=initMic();
  Serial.printf("TARS: DAC=%s MIC=%s\n",dacOK?"READY":"ERROR",micOK?"READY":"ERROR");
  Serial.println("TARS: DAC GPIO26 INTERNAL DAC RIGHT");Serial.println("TARS: MP3 44.1k -> 22.05k/16bit");
  Serial.println("TARS: WAV AUTO FORMAT");Serial.println("TARS: INMP441 RIGHT GPIO34");
  Serial.printf("TARS: MIC THRESHOLD=%ld\n",(long)MIC_THRESHOLD);Serial.printf("TARS: MIC SILENCE=%ld\n",(long)MIC_SILENCE);
  Serial.println("TARS: MIC RMS TRIGGER=1800");Serial.println("TARS: MIC RMS SILENCE=1200");Serial.println("TARS: PREROLL=700 ms");
  Serial.println("TARS: STT REALTIME PCM");Serial.println("TARS: BLUETOOTH DISABLED");
  Serial.printf("TARS: AUDIO RING=%u BYTES\n",(unsigned)AUDIO_RING_SIZE);Serial.printf("TARS: AUDIO PREBUFFER=%u BYTES\n",(unsigned)AUDIO_PREBUFFER);
  Serial.printf("TARS: STREAM EOF IDLE=%lu ms\n",(unsigned long)STREAM_EOF_IDLE_MS);Serial.println("TARS: OFFLINE ALARM=06:00 WIB");
  Serial.println("TARS: SYSTEM STATUS CHECK READY");
  if(oledOK)xTaskCreatePinnedToCore(oledTask,"TARS_OLED",4096,nullptr,1,nullptr,0);
  wifiManagerBegin();if(bootWiFi())syncTime();oledSetListening();
}

void loop(){
  if(playing){delay(1);return;}
  if(WiFi.status()!=WL_CONNECTED){if(!wifiOK()){oledSetStatus("WIFI ERROR");delay(500);return;}}
  if(alarmDue()){runAlarm();return;}
  String q=recordRealtime();if(q.length())processQuestion(q);else oledSetListening();delay(1);
}
