// TARS Cloud + OLED + WiFi/NTP + TTS/SING + A2DP + Helix
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Wire.h>
#include <time.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BluetoothA2DPSource.h>
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "config.h"
#include "wifi_manager.h"

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_ADDR 0x3C

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);
bool oledReadyFlag=false;
static String oledAnswer="";
static size_t oledTypedChars=0;
static uint32_t oledLastType=0;
static const uint32_t OLED_TYPE_INTERVAL=44;
static bool oledTyping=false;
volatile bool a2dpFirstAudioCallback=false;
volatile uint32_t a2dpFirstAudioMillis=0;
static bool oledAudioSyncPending=false;
static const uint32_t OLED_AUDIO_SYNC_DELAY_MS=50;
static uint32_t oledLastAnim=0;
static uint8_t oledMechanicalFrame=0;
static const uint32_t OLED_ANIM_INTERVAL=80;

static const char *ASK_URL="https://tars-cloud-v1.hilmane34.workers.dev/ask";
static const char *TTS_URL="https://tars-cloud-v1.hilmane34.workers.dev/tts";
static const char *SING_URL="https://tars-cloud-v1.hilmane34.workers.dev/sing";
static const char *MP3_PATH="/tts.mp3";
static const char *NTP_SERVER_1="pool.ntp.org";
static const char *NTP_SERVER_2="time.nist.gov";
static const long GMT_OFFSET_SEC=7*3600;
static const int DAYLIGHT_OFFSET_SEC=0;
static const uint32_t WIFI_TIMEOUT_MS=15000;
static const uint32_t NTP_TIMEOUT_MS=15000;
static const uint32_t BT_TIMEOUT_MS=20000;
static const uint32_t PLAY_TIMEOUT_MS=120000;
static const uint32_t BT_DISCONNECT_TIMEOUT_MS=3000;
static const uint32_t INPUT_SAMPLE_RATE=22050;
static const uint8_t INPUT_CHANNELS=1;
static const uint8_t OUTPUT_CHANNELS=2;
static const uint8_t BITS_PER_SAMPLE=16;
static const size_t PCM_RING_SIZE=16384;
static const size_t MP3_COPY_BUFFER=1024;
static const size_t PCM_OUTPUT_CHUNK=1024;
static const float PCM_GAIN=3.5f;
static const uint32_t A2DP_TAIL_MS=1000;
static const char *BT_DEVICE_NAME="I7-TWS";

BluetoothA2DPSource *a2dpSource=nullptr;
volatile bool btConnected=false,btAudioStarted=false,playbackRunning=false;
volatile uint32_t btCallbackCalls=0;
bool ntpSynced=false,singMode=false;
volatile uint8_t singLevel=0;

void printHeap(const char *l){Serial.printf("HEAP[%s]: free=%u largest=%u internal=%u\n",l,(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getMaxAllocHeap(),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));}

void oledHeader(const char *s){
  if(!oledReadyFlag)return;
  oled.setTextColor(SSD1306_WHITE);oled.setTextSize(1);oled.setCursor(45,0);oled.print("T A R S");
  oled.drawLine(0,9,27,9,SSD1306_WHITE);oled.drawLine(34,9,61,9,SSD1306_WHITE);
  oled.drawLine(67,9,94,9,SSD1306_WHITE);oled.drawLine(101,9,127,9,SSD1306_WHITE);
  oled.setCursor(3,13);oled.print("> ");oled.print(s);
}

void oledDrawMechanical(bool speaking){
  if(!oledReadyFlag)return;
  const int b=62;oled.drawLine(2,b,125,b,SSD1306_WHITE);
  if(!speaking){
    static const uint8_t p[24]={2,2,5,5,5,2,2,4,4,2,2,5,5,5,2,2,4,4,2,2,5,5,2,2};
    for(int i=0;i<24;i++){int x=3+i*5;if(x>123)break;uint8_t n=(i+oledMechanicalFrame)%24;int h=p[n];
      oled.drawLine(x,b-h,x+3,b-h,SSD1306_WHITE);if(i<23){int nx=x+5;if(nx<=125)oled.drawLine(x+3,b-h,nx,b-p[(n+1)%24],SSD1306_WHITE);}}
    int x=45+(oledMechanicalFrame%17);oled.drawLine(x,57,x+3,57,SSD1306_WHITE);return;
  }
  for(int i=0;i<6;i++){int x=8+i*22,h=3;if(i==(oledMechanicalFrame%6))h=8;else if(i==((oledMechanicalFrame+5)%6)||i==((oledMechanicalFrame+1)%6))h=5;
    oled.drawLine(x,b-h,x,b,SSD1306_WHITE);oled.drawLine(x-3,b-h,x,b,SSD1306_WHITE);oled.drawLine(x,b,x+3,b-h,SSD1306_WHITE);}
  int x=61+(((oledMechanicalFrame%3)-1)*3);oled.drawLine(x,56,x,59,SSD1306_WHITE);
}

void oledShowReady(){if(!oledReadyFlag)return;oled.clearDisplay();oledHeader("READY");oled.setCursor(3,27);oled.print("WAITING FOR");oled.setCursor(3,36);oled.print("COMMAND...");oledDrawMechanical(false);oled.display();oledLastAnim=millis();}
void oledShowListening(){if(!oledReadyFlag)return;oled.clearDisplay();oledHeader("LISTENING");oled.setCursor(3,27);oled.print("INPUT RECEIVED");oled.setCursor(3,36);oled.print("AWAITING QUERY");oledDrawMechanical(false);oled.display();}
void oledShowProcessing(){if(!oledReadyFlag)return;oled.clearDisplay();oledHeader("PROCESSING");oled.setCursor(3,27);oled.print("ANALYZING...");oled.setCursor(3,36);oled.print("GENERATING RESPONSE");oledDrawMechanical(false);oled.display();}
void oledShowOnline(){if(!oledReadyFlag)return;oled.clearDisplay();oledHeader("ONLINE");oled.setCursor(3,27);oled.print("SYSTEM INITIALIZED");oled.setCursor(3,36);oled.print("A2DP : STANDBY");oled.setCursor(3,45);oled.print("VOICE: READY");oledDrawMechanical(false);oled.display();}
void oledUpdateReadyAnimation(){if(!oledReadyFlag)return;uint32_t n=millis();if(n-oledLastAnim<OLED_ANIM_INTERVAL)return;oledLastAnim=n;oledMechanicalFrame++;oled.clearDisplay();oledHeader("READY");oled.setCursor(3,27);oled.print("WAITING FOR");oled.setCursor(3,36);oled.print("COMMAND...");oledDrawMechanical(false);oled.display();}
void oledPrepareTyping(const String &t){if(!oledReadyFlag)return;oledAnswer=t;oledTypedChars=0;oledLastType=millis();oledMechanicalFrame=0;oledTyping=false;oledAudioSyncPending=true;}
void oledStartTypingNow(){if(!oledReadyFlag)return;oledTypedChars=0;oledLastType=millis();oledMechanicalFrame=0;oledTyping=true;oledAudioSyncPending=false;Serial.println("TARS: OLED TYPING START - AUDIO SYNC");}

void oledDrawTypedText(bool speaking){
  if(!oledReadyFlag)return;
  oled.clearDisplay();oledHeader(speaking?"SPEAKING":"READY");oled.setTextColor(SSD1306_WHITE);oled.setTextSize(1);
  String v=oledAnswer.substring(0,oledTypedChars);const int sx=3,sy=23,mc=20,ml=4;int line=0,col=0;oled.setCursor(sx,sy);oled.print("> ");col=2;
  for(size_t i=0;i<v.length();i++){char c=v[i];if(c=='\r')continue;if(c=='\n'){line++;col=0;if(line>=ml)break;oled.setCursor(sx,sy+line*9);continue;}if(col>=mc){line++;col=0;if(line>=ml)break;oled.setCursor(sx,sy+line*9);}oled.write(c);col++;}
  oledDrawMechanical(speaking);oled.display();
}

void oledDrawSing(){
  if(!oledReadyFlag)return;
  oled.clearDisplay();uint8_t level=singLevel;
  for(int i=0;i<6;i++){int variation=((int)i*13+(millis()/90)%17)%18-9;int h=4+(level*(70+variation)/255);if(h<4)h=4;if(h>55)h=55;int x=16+i*19;oled.fillRect(x,63-h,8,h,SSD1306_WHITE);}
  oled.display();
}

void oledUpdateTyping(bool speaking){
  if(!oledReadyFlag)return;
  if(singMode){oledDrawSing();return;}
  uint32_t n=millis();
  if(oledAudioSyncPending&&a2dpFirstAudioCallback&&n-a2dpFirstAudioMillis>=OLED_AUDIO_SYNC_DELAY_MS){oledStartTypingNow();oledDrawTypedText(true);}
  bool redraw=false;
  if(oledTyping&&n-oledLastType>=OLED_TYPE_INTERVAL){oledLastType=n;if(oledTypedChars<oledAnswer.length())oledTypedChars++;else oledTyping=false;redraw=true;}
  if(speaking&&n-oledLastAnim>=OLED_ANIM_INTERVAL){oledLastAnim=n;oledMechanicalFrame++;redraw=true;}
  if(redraw)oledDrawTypedText(speaking);
}

class PCMRingBuffer{
  uint8_t *buffer=nullptr;size_t capacity=0;volatile size_t readIndex=0,writeIndex=0,used=0;portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
public:
  bool begin(size_t s){if(buffer){free(buffer);buffer=nullptr;}buffer=(uint8_t*)malloc(s);if(!buffer){capacity=0;return false;}capacity=s;clear();return true;}
  void end(){if(buffer){free(buffer);buffer=nullptr;}capacity=readIndex=writeIndex=used=0;}
  void clear(){portENTER_CRITICAL(&mux);readIndex=writeIndex=used=0;portEXIT_CRITICAL(&mux);}
  size_t available(){size_t v;portENTER_CRITICAL(&mux);v=used;portEXIT_CRITICAL(&mux);return v;}
  size_t freeSpace(){size_t v;portENTER_CRITICAL(&mux);v=capacity-used;portEXIT_CRITICAL(&mux);return v;}
  size_t write(const uint8_t *src,size_t len){if(!buffer||!src||!len)return 0;size_t w=0;portENTER_CRITICAL(&mux);size_t f=capacity-used;if(len>f)len=f;if(len){size_t a=capacity-writeIndex;if(a>len)a=len;memcpy(buffer+writeIndex,src,a);size_t b=len-a;if(b)memcpy(buffer,src+a,b);writeIndex=(writeIndex+len)%capacity;used+=len;w=len;}portEXIT_CRITICAL(&mux);return w;}
  size_t read(uint8_t *dst,size_t len){if(!buffer||!dst||!len)return 0;size_t r=0;portENTER_CRITICAL(&mux);if(len>used)len=used;if(len){size_t a=capacity-readIndex;if(a>len)a=len;memcpy(dst,buffer+readIndex,a);size_t b=len-a;if(b)memcpy(dst+a,buffer,b);readIndex=(readIndex+len)%capacity;used-=len;r=len;}portEXIT_CRITICAL(&mux);return r;}
  bool isReady(){return buffer&&capacity;}
} pcmRing;

class PCMOutputStream:public AudioStream{
  AudioInfo currentInfo;uint8_t outputBuffer[PCM_OUTPUT_CHUNK];
  static int16_t applyGain(int16_t s){int32_t v=(int32_t)((float)s*PCM_GAIN);if(v>32767)v=32767;if(v<-32768)v=-32768;return(int16_t)v;}
public:
  void setAudioInfo(AudioInfo i)override{currentInfo=i;AudioStream::setAudioInfo(i);Serial.printf("PCM format: %d Hz, %d ch, %d bit\n",i.sample_rate,i.channels,i.bits_per_sample);}
  int availableForWrite()override{size_t f=pcmRing.freeSpace();if(currentInfo.sample_rate==INPUT_SAMPLE_RATE&&currentInfo.channels==INPUT_CHANNELS&&currentInfo.bits_per_sample==BITS_PER_SAMPLE){size_t n=f/4;if(n>512)n=512;return(int)n;}return(int)min(f,(size_t)512);}
  size_t write(const uint8_t *data,size_t size)override{
    if(!data||!size||!pcmRing.isReady())return 0;
    if(currentInfo.sample_rate!=INPUT_SAMPLE_RATE||currentInfo.channels!=INPUT_CHANNELS||currentInfo.bits_per_sample!=BITS_PER_SAMPLE){Serial.printf("PCM ERROR: unsupported %d Hz %d ch %d bit\n",currentInfo.sample_rate,currentInfo.channels,currentInfo.bits_per_sample);return 0;}
    size_t off=0;
    while(off<size){size_t samples=(size-off)/2,maxSamples=sizeof(outputBuffer)/8;if(samples>maxSamples)samples=maxSamples;if(!samples)break;size_t req=samples*8;uint32_t ws=millis();
      while(pcmRing.freeSpace()<req){oledUpdateTyping(true);if(millis()-ws>5000){Serial.println("PCM ERROR: ring timeout");return 0;}delay(2);}
      int16_t *in=(int16_t*)(data+off),*out=(int16_t*)outputBuffer;size_t oi=0;int peak=0;
      for(size_t i=0;i<samples;i++){int16_t s=applyGain(in[i]);int a=abs((int)s);if(a>peak)peak=a;out[oi++]=s;out[oi++]=s;out[oi++]=s;out[oi++]=s;}
      singLevel=(uint8_t)min(255,peak>>7);if(pcmRing.write(outputBuffer,req)!=req){Serial.println("PCM ERROR: ring write");return 0;}off+=samples*2;
    }return off;
  }
};

PCMOutputStream pcmOutput;MP3DecoderHelix mp3Decoder;EncodedAudioStream mp3Stream(&pcmOutput,&mp3Decoder);

bool isTimeValid(){return time(nullptr)>1577836800;}

bool syncNTP(){
  Serial.println("TARS: NTP START");configTime(GMT_OFFSET_SEC,DAYLIGHT_OFFSET_SEC,NTP_SERVER_1,NTP_SERVER_2);
  uint32_t s=millis();int a=0;while(!isTimeValid()&&millis()-s<NTP_TIMEOUT_MS){Serial.printf("TARS: NTP attempt %d\n",++a);delay(1000);}
  if(!isTimeValid()){Serial.println("TARS: NTP FAILED");ntpSynced=false;return false;}
  time_t now=time(nullptr);struct tm t;localtime_r(&now,&t);Serial.printf("TARS: NTP OK %04d-%02d-%02d %02d:%02d:%02d\n",t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec);ntpSynced=true;return true;
}

bool ensureTimeValid(){
  if(ntpSynced&&isTimeValid())return true;
  if(isTimeValid()){ntpSynced=true;return true;}
  return syncNTP();
}

bool connectWiFi(bool requireTime=false){
  if(WiFi.status()==WL_CONNECTED){
    if(!requireTime||ntpSynced)return true;
    return ensureTimeValid();
  }
  Serial.println("TARS: WiFi ON");
  if(!wifiManagerConnect(false)){Serial.println("TARS: WIFI FAILED");return false;}
  if(requireTime&&!ensureTimeValid()){Serial.println("TARS: NTP FAILED");return false;}
  Serial.print("TARS: IP = ");Serial.println(WiFi.localIP());
  return true;
}

void disconnectWiFi(){wifiManagerDisconnect();}

String askAI(const String &question){
  if(!connectWiFi(true)||!ensureTimeValid())return "";
  oledShowProcessing();WiFiClientSecure client;client.setInsecure();HTTPClient http;
  if(!http.begin(client,ASK_URL))return "";http.addHeader("Content-Type","application/json");
  StaticJsonDocument<512> req;req["text"]=question;String body;serializeJson(req,body);
  int code=http.POST(body);Serial.printf("ASK HTTP: %d\n",code);if(code<200||code>=300){http.end();return "";}
  String response=http.getString();http.end();StaticJsonDocument<1024> json;if(deserializeJson(json,response)){Serial.println("TARS: JSON ERROR");return "";}
  String answer=json["response"]|"";Serial.println("TARS RESPONSE:");Serial.println(answer);return answer;
}

bool downloadAudio(const char *url,const String &text){
  if(!connectWiFi(true)||!ensureTimeValid())return false;
  WiFiClientSecure client;client.setInsecure();HTTPClient http;if(!http.begin(client,url))return false;
  http.addHeader("Content-Type","application/json");StaticJsonDocument<512> req;req["text"]=text;String body;serializeJson(req,body);
  int code=http.POST(body);Serial.printf("AUDIO HTTP: %d\n",code);if(code<200||code>=300){http.end();return false;}
  int len=http.getSize();if(LittleFS.exists(MP3_PATH))LittleFS.remove(MP3_PATH);File file=LittleFS.open(MP3_PATH,FILE_WRITE);if(!file){http.end();return false;}
  WiFiClient *stream=http.getStreamPtr();uint8_t buffer[1024];size_t total=0;uint32_t last=millis();
  while(http.connected()&&(len>0||len==-1)){size_t av=stream->available();if(av){size_t n=min(av,sizeof(buffer));int r=stream->readBytes(buffer,n);if(r>0){file.write(buffer,r);total+=r;last=millis();if(len>0)len-=r;}}else{if(millis()-last>5000)break;delay(1);}}
  file.flush();file.close();http.end();Serial.printf("TARS: MP3 bytes = %u\n",(unsigned)total);return total>0;
}

int32_t getAudioDataNoop(uint8_t *d,int32_t l){if(d&&l>0)memset(d,0,l);return l;}
int32_t getAudioData(uint8_t *d,int32_t l){
  if(!d||l<=0)return 0;btCallbackCalls++;size_t got=pcmRing.read(d,l);
  if(got&&!a2dpFirstAudioCallback){a2dpFirstAudioCallback=true;a2dpFirstAudioMillis=millis();Serial.println("TARS: A2DP FIRST REAL AUDIO CALLBACK");}
  if(got<(size_t)l)memset(d+got,0,l-got);return l;
}

void onBTConnectionState(esp_a2d_connection_state_t s,void*){Serial.printf("TARS: A2DP STATE = %d\n",(int)s);btConnected=s==ESP_A2D_CONNECTION_STATE_CONNECTED;}
void onBTAudioState(esp_a2d_audio_state_t s,void*){Serial.printf("TARS: A2DP AUDIO = %d\n",(int)s);btAudioStarted=s==ESP_A2D_AUDIO_STATE_STARTED;}

bool ensureBluetoothObject(){
  if(a2dpSource)return true;Serial.println("TARS: A2DP OBJECT CREATE");printHeap("BEFORE_BT_OBJECT");a2dpSource=new BluetoothA2DPSource();if(!a2dpSource)return false;
  a2dpSource->set_event_queue_size(4);a2dpSource->set_event_stack_size(3072);a2dpSource->set_auto_reconnect(false);a2dpSource->set_data_callback(getAudioDataNoop);
  a2dpSource->set_on_connection_state_changed(onBTConnectionState);a2dpSource->set_on_audio_state_changed(onBTAudioState);printHeap("AFTER_BT_OBJECT");return true;
}

void rebootTARS(const char *r){Serial.println();Serial.printf("TARS: REBOOT - %s\n",r);delay(500);Serial.flush();esp_restart();while(true)delay(1000);}

bool startBluetooth(){
  Serial.println("TARS: Bluetooth START");if(!ensureBluetoothObject())return false;a2dpSource->set_data_callback(getAudioData);
  btConnected=false;btAudioStarted=false;btCallbackCalls=0;a2dpFirstAudioCallback=false;a2dpFirstAudioMillis=0;printHeap("BEFORE_BT");uint32_t s=millis();a2dpSource->start(BT_DEVICE_NAME);
  while(!btConnected&&millis()-s<BT_TIMEOUT_MS){delay(20);yield();}if(!btConnected){printHeap("BT_FAILED");rebootTARS("BT CONNECTION FAILED");return false;}
  Serial.println("TARS: Bluetooth READY - PLAY NOW");printHeap("BT_READY");return true;
}

void releaseBluetooth(){
  Serial.println("TARS: Bluetooth DISCONNECT START");if(a2dpSource){a2dpSource->set_data_callback(getAudioDataNoop);if(btConnected){a2dpSource->disconnect();uint32_t s=millis();while(btConnected&&millis()-s<BT_DISCONNECT_TIMEOUT_MS){delay(10);yield();}Serial.println(btConnected?"TARS: A2DP DISCONNECT TIMEOUT":"TARS: A2DP DISCONNECTED");}}
  btAudioStarted=false;btConnected=false;Serial.println("TARS: Bluetooth SESSION CLOSED");printHeap("BT_SESSION_CLOSED");
}

void stopBluetooth(){Serial.println("TARS: Bluetooth STOP");releaseBluetooth();Serial.printf("TARS: A2DP callbacks = %u\n",(unsigned)btCallbackCalls);}

void cleanupAudioSession(){
  Serial.println("TARS: AUDIO CLEANUP");pcmRing.end();if(LittleFS.exists(MP3_PATH)){LittleFS.remove(MP3_PATH);Serial.println("TARS: MP3 FILE REMOVED");}
  oledTyping=false;oledTypedChars=oledAnswer.length();singMode=false;singLevel=0;printHeap("AFTER_AUDIO_CLEANUP");
}

bool playMP3(){
  if(!LittleFS.exists(MP3_PATH)){Serial.println("TARS: MP3 NOT FOUND");return false;}File mp3File=LittleFS.open(MP3_PATH,FILE_READ);if(!mp3File)return false;
  size_t mp3Size=mp3File.size();if(!mp3Size){mp3File.close();LittleFS.remove(MP3_PATH);return false;}
  Serial.printf("TARS: PLAY START%s\n",singMode?" [SING]":"");playbackRunning=true;singLevel=0;
  if(!pcmRing.begin(PCM_RING_SIZE)){mp3File.close();playbackRunning=false;cleanupAudioSession();return false;}printHeap("AFTER_PCM_RING");
  if(!startBluetooth()){mp3File.close();playbackRunning=false;cleanupAudioSession();return false;}printHeap("AFTER_BT_BEFORE_HELIX");
  mp3Decoder.setMaxPCMSize(2048);mp3Decoder.setMaxFrameSize(1024);
  if(!mp3Stream.begin()){mp3File.close();playbackRunning=false;cleanupAudioSession();rebootTARS("MP3 DECODER FAILED");return false;}
  StreamCopy copier(mp3Stream,mp3File,MP3_COPY_BUFFER);copier.setCheckAvailableForWrite(false);copier.setCheckAvailable(true);
  uint32_t ps=millis(),lp=millis();bool finished=false;
  while(millis()-ps<PLAY_TIMEOUT_MS){
    oledUpdateTyping(true);size_t pos=mp3File.position(),av=pcmRing.available(),freePCM=pcmRing.freeSpace();
    if(millis()-lp>1000){Serial.printf("AUDIO: MP3=%u/%u PCM=%u/%u FREE=%u BT=%d AUDIO=%d CB=%u\n",(unsigned)pos,(unsigned)mp3Size,(unsigned)av,(unsigned)PCM_RING_SIZE,(unsigned)freePCM,btConnected,btAudioStarted,(unsigned)btCallbackCalls);lp=millis();}
    if(!btConnected)break;if(pos>=mp3Size){finished=true;Serial.println("TARS: MP3 EOF");break;}if(freePCM<4096){delay(2);yield();continue;}size_t copied=copier.copy();if(!copied)delay(2);else yield();
  }
  mp3Stream.end();mp3File.close();Serial.println("TARS: PCM DRAIN");uint32_t ds=millis();while(pcmRing.available()>0&&millis()-ds<10000){oledUpdateTyping(true);delay(10);}
  Serial.printf("TARS: PCM %s\n",pcmRing.available()?"DRAIN TIMEOUT":"EMPTY");uint32_t ts=millis();while(millis()-ts<A2DP_TAIL_MS){oledUpdateTyping(true);delay(10);if(!btConnected)break;}
  stopBluetooth();cleanupAudioSession();playbackRunning=false;if(!finished){rebootTARS("PLAYBACK FAILED");return false;}while(oledTyping){oledUpdateTyping(false);delay(5);}
  oledShowReady();Serial.println("TARS: PLAY DONE");printHeap("PLAY_DONE");rebootTARS("PLAYBACK COMPLETE");return true;
}

bool isSingRequest(String q){q.toLowerCase();return q.indexOf("nyanyi")>=0||q.indexOf("bernyanyi")>=0||q.indexOf("nyanyikan")>=0||q.indexOf("nyanyiin")>=0;}

void handleQuestion(const String &question){
  if(!question.length())return;Serial.println("\nTARS: COMMAND RECEIVED");Serial.println(question);oledShowListening();
  if(!connectWiFi(true)){oledShowReady();return;}String answer=askAI(question);if(!answer.length()){oledShowReady();return;}
  bool sing=isSingRequest(question);oledPrepareTyping(answer);
  if(!downloadAudio(sing?SING_URL:TTS_URL,answer)){Serial.println("TARS: AUDIO DOWNLOAD FAILED");oledShowReady();return;}
  singMode=sing;disconnectWiFi();playMP3();
}

void setup(){
  Serial.begin(SERIAL_BAUD);delay(1000);Serial.println("\n================================");Serial.println("        TARS ESP32 START");Serial.println("================================");printHeap("BOOT");
  Wire.begin(21,22);if(oled.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR)){oledReadyFlag=true;oledShowOnline();}
  if(!LittleFS.begin(true)){Serial.println("LittleFS FAILED");return;}Serial.println("LittleFS OK");
  Serial.println("PCM  : LAZY 16KB BUFFER");Serial.println("HELIX: 2048 PCM / 1024 FRAME");Serial.println("BT   : QUEUE 4 / STACK 3072");Serial.println("BT   : AUTO RECONNECT OFF");Serial.println("A2DP : LAZY OBJECT");Serial.println("OLED : AUDIO SYNC 50 MS");
  printHeap("BEFORE_WIFI");
  Serial.println("TARS: WIFI MANAGER START");
  wifiManagerBegin();
  if(WiFi.status()!=WL_CONNECTED){if(!connectWiFi(true))Serial.println("TARS: INITIAL WIFI FAILED");}
  else if(!ntpSynced)ensureTimeValid();
  printHeap("READY");oledShowReady();
  Serial.println("================================");Serial.println("TARS: READY FOR QUESTION");Serial.println("================================");
}

void loop(){
  oledUpdateReadyAnimation();
  if(Serial.available()){String q=Serial.readStringUntil('\n');q.trim();if(q.length())handleQuestion(q);}
  delay(10);
}
