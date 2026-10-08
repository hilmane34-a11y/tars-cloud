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
#include <esp_heap_caps.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <OV7670.h>
#include <string.h>
#include <I2SCamera.h>

#include "vision_live.h"
#include "config.h"
#include "wifi_manager.h"
#include "Log.h"
#include "personality.h"
#include "autonomy.h"
#include "auto_speech.h"
#include "wheels.h"
#include "env.h"
#include "deep_sleep.h"
#include "sleep_oled.h"
#include <tars_emotion.h>
#include <stt.h>
#include <tts.h>
#include <oled.h>

#define MIC_PORT I2S_NUM_1
#define MIC_SCK 18
#define MIC_WS 2
#define MIC_SD 15

#define CAM_XCLK 4
#define CAM_SIOD 21
#define CAM_SIOC 22
#define CAM_D7 36
#define CAM_D6 39
#define CAM_D5 34
#define CAM_D4 35
#define CAM_D3 32
#define CAM_D2 33
#define CAM_D1 27
#define CAM_D0 25
#define CAM_VSYNC 13
#define CAM_HREF 14
#define CAM_PCLK 12

const uint32_t ALARM_DURATION_MS=180000;

enum TarsMode:uint8_t{MODE_OFFLINE,MODE_ONLINE};
TarsMode tarsMode=MODE_OFFLINE;

bool alarmRunning=false,greetingPlaying=false,cameraOK=false,cameraLive=false,ntpOK=false;
volatile bool ntpSyncEvent=false;
int alarmLastDay=-1;
uint8_t lastGreetingPeriod=255;
uint32_t ramDiagAt=0;
bool sleepPreparing=false;
uint32_t sleepPrepareAt=0;
volatile bool pendingVisionCheck=false;
uint32_t lastVisionEventAt=0;
const uint32_t VISION_EVENT_COOLDOWN_MS=120000;

OV7670*camera=nullptr;
SemaphoreHandle_t previewMux=nullptr,envMux=nullptr,cameraMux=nullptr;
static uint16_t cameraEnvironment[96*32];
uint8_t cameraPreview[128*64];
bool previewReady=false;
portMUX_TYPE visionEventMux=portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t personalityTaskHandle=nullptr;
static bool visionSceneLatched=false;

extern const uint8_t alarm_start[] asm("_binary_src_alarm_mp3_start");
extern const uint8_t alarm_end[] asm("_binary_src_alarm_mp3_end");
#define MP3SYM(n) extern const uint8_t n##_start[] asm("_binary_src_"#n"_mp3_start");extern const uint8_t n##_end[] asm("_binary_src_"#n"_mp3_end");
MP3SYM(follow) MP3SYM(online) MP3SYM(offline)
MP3SYM(hari) MP3SYM(pagi) MP3SYM(siang)
MP3SYM(sore) MP3SYM(malam)
bool wifiOK();
bool visionLiveEnabled(){return tarsMode==MODE_ONLINE;}
String normCmd(String);
String systemStatus();
String ask(const String&);
static void personalityTask(void*){
  for(;;){
    personalityUpdate(false,autonomyIsMoving(),false);
    tarsEmotionUpdate();
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}
void ramDiag(const char*tag){
  uint32_t f=ESP.getFreeHeap(),m=ESP.getMinFreeHeap(),a=ESP.getMaxAllocHeap();
  uint32_t i=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  uint32_t frag=f?100-(a*100/f):100;
  Serial.printf("TARS: RAM[%s] free=%u KB min=%u KB max=%u KB intmax=%u KB frag=%u%%\n",tag,f/1024,m/1024,a/1024,i/1024,frag);
}
void ramMonitor(){
  if(millis()-ramDiagAt<5000)return;
  ramDiagAt=millis();
  ramDiag("MONITOR");
}
void camDiag(const char*tag){
  Serial.printf("TARS: CAM[%s] live=%d ok=%d ptr=%p oled=%d play=%d text=%u RAM=%u/%u KB\n",tag,cameraLive,cameraOK,camera,oledOK,playing,(unsigned)oledText.length(),ESP.getFreeHeap()/1024,ESP.getMaxAllocHeap()/1024);
}
bool initCamera(){
  if(camera&&cameraOK&&cameraLive)return true;
  Serial.println("TARS: CAM INIT START");
  ramDiag("CAM-BEFORE-INIT");
  camera=new OV7670(OV7670::Mode::QVGA_RGB565,CAM_SIOD,CAM_SIOC,CAM_VSYNC,CAM_HREF,CAM_XCLK,CAM_PCLK,CAM_D0,CAM_D1,CAM_D2,CAM_D3,CAM_D4,CAM_D5,CAM_D6,CAM_D7);
  if(!camera){
    cameraOK=false;cameraLive=false;
    Serial.println("TARS: CAM INIT ALLOC FAILED");
    ramDiag("CAM-INIT-FAILED");
    return false;
  }

  Serial.printf("TARS: CAM OBJECT=%p RES=%dx%d\n",camera,camera->xres,camera->yres);

  if(camera->xres!=320||camera->yres!=240){
    delete camera;camera=nullptr;
    cameraOK=false;cameraLive=false;
    ramDiag("CAM-INVALID-RELEASED");
    return false;
  }

  cameraOK=true;
  cameraLive=true;
  ramDiag("CAM-AFTER-INIT");
  camDiag("LIVE");
  return true;
}
void stopCamera(){
  Serial.println("TARS: CAM STOP START");
  camDiag("BEFORE-OFF");
  if(!cameraMux){
    Serial.println("TARS: CAM MUTEX NULL");
    return;
  }
  if(xSemaphoreTake(cameraMux,pdMS_TO_TICKS(5000))!=pdTRUE){
    Serial.println("TARS: CAM MUTEX TIMEOUT");
    return;
  }
  cameraLive=false;
  if(camera){
    delete camera;
    camera=nullptr;
  }
  cameraOK=false;
  if(previewMux&&xSemaphoreTake(previewMux,pdMS_TO_TICKS(500))==pdTRUE){
    previewReady=false;
    memset(cameraPreview,0,sizeof(cameraPreview));
    xSemaphoreGive(previewMux);
  }
  xSemaphoreGive(cameraMux);
  delay(50);
  ramDiag("CAM-AFTER-DELETE");
  camDiag("OFF");
  Serial.println("TARS: CAM STOP DONE");
}
bool startCamera(){
  Serial.println("TARS: CAM START REQUEST");
  camDiag("START-BEFORE");
  if(!cameraMux){
    Serial.println("TARS: CAM MUTEX NULL");
    return false;
  }
  if(xSemaphoreTake(cameraMux,pdMS_TO_TICKS(5000))!=pdTRUE){
    Serial.println("TARS: CAM START MUTEX TIMEOUT");
    return false;
  }
  if(camera&&cameraOK&&cameraLive){
    xSemaphoreGive(cameraMux);
    Serial.println("TARS: CAM ALREADY LIVE");
    return true;
  }
  cameraLive=false;
  cameraOK=false;
  if(camera){
    delete camera;
    camera=nullptr;
  }
  if(previewMux&&xSemaphoreTake(previewMux,pdMS_TO_TICKS(500))==pdTRUE){
    previewReady=false;
    memset(cameraPreview,0,sizeof(cameraPreview));
    xSemaphoreGive(previewMux);
  }
  bool ok=initCamera();
  xSemaphoreGive(cameraMux);
  if(!ok){
    Serial.println("TARS: CAM RESTART FAILED");
    ramDiag("CAM-RESTART-FAILED");
    return false;
  }
  Serial.println("TARS: CAM RESTART SUCCESS");
  ramDiag("CAM-RESTART-DONE");
  camDiag("START-DONE");
  return true;
}
void cameraTask(void*){
  uint8_t frameErrors=0;
  for(;;){
    if(!cameraLive||!camera||!cameraOK||playing){
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    bool ok=false,analyzed=false;
    EnvState environment={};
    if(xSemaphoreTake(cameraMux,portMAX_DELAY)==pdTRUE){
      if(xSemaphoreTake(envMux,portMAX_DELAY)==pdTRUE){
        if(xSemaphoreTake(previewMux,portMAX_DELAY)==pdTRUE){
          ok=I2SCamera::captureFrameData(cameraEnvironment,cameraPreview);
          if(ok)previewReady=true;
          xSemaphoreGive(previewMux);
        }
        if(ok)
          analyzed=envAnalyze(cameraEnvironment,I2SCamera::dominantColor(),I2SCamera::dominantColorConfidence(),environment);
        xSemaphoreGive(envMux);
      }
      xSemaphoreGive(cameraMux);
    }

    if(ok&&analyzed){
      autonomySetEnvironment(environment);
      uint8_t people=I2SCamera::peopleCount();
      tarsEmotionPeople(people);
      if(tarsMode==MODE_ONLINE){
        if(environment.event==ENV_NONE)visionSceneLatched=false;
        if(environment.event==ENV_SCENE_CHANGED&&!visionSceneLatched&&millis()-lastVisionEventAt>=VISION_EVENT_COOLDOWN_MS){
          portENTER_CRITICAL(&visionEventMux);
          pendingVisionCheck=true;
          portEXIT_CRITICAL(&visionEventMux);
          visionSceneLatched=true;
          lastVisionEventAt=millis();
          Serial.println("TARS: SIGNIFICANT SCENE CHANGE -> VISION");
        }
      }
      frameErrors=0;
    }else frameErrors++;
    if(cameraLive&&!playing)autonomyUpdate(true,false);
    vTaskDelay(1);
  }
}
bool needsVision(String q){
  q=normCmd(q);

  static const char*words[]={
    "lihat","lihatkan","melihat","tunjukkan","perlihatkan","objek","benda","warna","foto","gambar","capture","potret","jepret","kamera","amati","mengamati","perhatikan","visual","vision","apa ini","ini apa","apa itu","itu apa","benda apa","objek apa","warna apa","lihat apa","terlihat apa","sedang melihat apa","kamu melihat","yang terlihat","di depan","ke depan","di sana","di situ","yang ada di","apa yang ada","lihat sekitar","amati sekitar","ambil gambar","ambil foto","ambil potret","foto sekarang","capture sekarang"
  };
  for(const char*w:words)
    if(q.indexOf(w)>=0)return true;
  return false;
}
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
  if(i2s_set_pin(MIC_PORT,&p)!=ESP_OK){
    i2s_driver_uninstall(MIC_PORT);
    return false;
  }
  i2s_zero_dma_buffer(MIC_PORT);
  Serial.println("TARS: INMP441 RIGHT READY");
  return true;
}
bool wifiOK(){
  return WiFi.status()==WL_CONNECTED||
    (wifiManagerConnect(false)&&WiFi.status()==WL_CONNECTED);
}
void ntpCallback(struct timeval*){ntpSyncEvent=true;}
bool syncTime(){
  if(ntpOK)return true;
  ntpSyncEvent=false;
  sntp_set_time_sync_notification_cb(ntpCallback);
  sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  configTime(7*3600,0,"pool.ntp.org","time.google.com","time.cloudflare.com");

  for(int a=1;a<=4;a++){
    Serial.printf("TARS: NTP %d/4\n",a);
    oledSetStatus("NTP "+String(a)+"/4");

    uint32_t st=millis();

    while(millis()-st<10000){
      if(ntpSyncEvent||sntp_get_sync_status()==SNTP_SYNC_STATUS_COMPLETED){
        time_t now=time(nullptr);

        if(now>=1704067200){
          struct tm t;
          localtime_r(&now,&t);

          Serial.printf("TARS: NTP VALID %04d-%02d-%02d %02d:%02d:%02d\n",t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec);

          ntpOK=true;
          oledSetStatus("NTP OK");
          return true;
        }
      }

      delay(100);
      yield();
    }

    if(a<4){
      ntpSyncEvent=false;
      sntp_restart();
      delay(1000);
    }
  }

  Serial.println("TARS: NTP FAILED 4/4");
  oledSetStatus("NTP FAILED");
  delay(1500);
  return false;
}

String normCmd(String s){
  s.toLowerCase();

  for(size_t i=0;i<s.length();i++)
    if(ispunct((unsigned char)s[i]))
      s.setCharAt(i,' ');

  while(s.indexOf("  ")>=0)s.replace("  "," ");
  s.trim();
  return s;
}

uint8_t greetingPeriod(){
  if(!ntpOK)return 255;

  time_t now=time(nullptr);
  if(now<1704067200)return 255;

  struct tm t;
  localtime_r(&now,&t);

  if(t.tm_hour>=5&&t.tm_hour<11)return 0;
  if(t.tm_hour>=11&&t.tm_hour<15)return 1;
  if(t.tm_hour>=15&&t.tm_hour<20)return 2;
  return 3;
}

const char*greetingText(uint8_t p){
  switch(p){
    case 0:return "Selamat pagi, tuan.";
    case 1:return "Selamat siang, tuan.";
    case 2:return "Selamat sore, tuan.";
    default:return "Selamat malam, tuan.";
  }
}

bool playTimeGreeting(uint8_t p){
  switch(p){
    case 0:return playLocalMP3(pagi_start,pagi_end,greetingText(p));
    case 1:return playLocalMP3(siang_start,siang_end,greetingText(p));
    case 2:return playLocalMP3(sore_start,sore_end,greetingText(p));
    case 3:return playLocalMP3(malam_start,malam_end,greetingText(p));
  }
  return false;
}

bool processOnlineRequest(const String&q,bool vision,bool status,bool automatic=false){
  wheelsStop();
  autonomyStop();
  ramDiag("BEFORE-CAMERA-CYCLE");

  if(!visionLivePause()){
    Serial.println("TARS: VISION PAUSE FAILED");
    wheelsStop();
    autonomyStop();
    oledSetStatus("CAMERA ERROR");
    return false;
  }

  if(!vision){
    stopCamera();

    if(camera||cameraLive||cameraOK){
      Serial.println("TARS: CAMERA OFF FAILED");
      visionLiveResume();
      startCamera();
      oledSetStatus("CAMERA ERROR");
      return false;
    }
  }

  wheelsStop();
  autonomyStop();
  ramDiag("CAMERA-OFF-BEFORE-TLS");

  String answer;

  if(vision)answer=visionLiveAsk(q);
  else if(status)answer=systemStatus();
  else answer=ask(q);

  if(!answer.length()){
    Serial.println("TARS: AI EMPTY RESPONSE");
    wheelsStop();
    autonomyStop();
    visionLiveResume();
    if(!vision)startCamera();
    oledSetStatus("AI ERROR");
    ramDiag("AFTER-AI-ERROR");

    closeSTT(STT_ERROR_COOLDOWN);
    sttDone=false;
    sttError=false;
    return false;
  }

  wheelsStop();
  autonomyStop();

  oledShowText(answer,automatic?"AUTO SPEECH":vision?"VISION":status?"STATUS":"ASK");
  delay(300);
  ramDiag("BEFORE-TTS");

  bool ok=streamAudio(String(TARS_CLOUD_URL)+"/tts",answer);

  audioStop();

  wheelsStop();
  autonomyStop();

  if(ok)tarsEmotionSpeechDone();

  ramDiag("AFTER-TTS-AUDIO-OFF");

  visionLiveResume();
  if(!vision)startCamera();

  wheelsStop();
  autonomyStop();

  closeSTT(STT_NORMAL_COOLDOWN);
  sttDone=false;
  sttError=false;

  oledSetStatus(ok?"LISTENING":"AUDIO ERROR");

  if(automatic&&ok)autoSpeechDone();

  ramDiag("AFTER-CAMERA-RESTART");

  Serial.printf("TARS: POST-TTS STT RESET connected=%d ready=%d\n",sttConnected,sttReady);
  return ok;
}

bool processEmotionEvent(){
  if(tarsMode!=MODE_ONLINE||playing||sttConnected||!tarsEmotionHasEvent())return false;

  TarsEmotionEvent e=tarsEmotionTakeEvent();
  String prompt=tarsEmotionPrompt(e);

  if(!prompt.length())return false;

  Serial.printf("TARS: EMOTION EVENT = %s\n",tarsEmotionName(e));

  return processOnlineRequest(prompt,false,false,false);
}

bool autoSpeechCallback(const String&prompt){
  if(tarsMode!=MODE_ONLINE||playing||sttConnected||WiFi.status()!=WL_CONNECTED)return false;

  if(prompt.startsWith("[AUTO_CHAT]")){
    String q=prompt.substring(11);
    q.trim();
    return processOnlineRequest(q,false,false,true);
  }

  if(prompt.startsWith("[AUTO_VISION]")){
    String q=prompt.substring(13);
    q.trim();
    return processOnlineRequest(q,true,false,true);
  }

  return false;
}

void processVisionEvent(){
  bool eventReady=false;

  portENTER_CRITICAL(&visionEventMux);
  if(pendingVisionCheck){
    pendingVisionCheck=false;
    eventReady=true;
  }
  portEXIT_CRITICAL(&visionEventMux);

  if(eventReady){
    autoSpeechNotifyVision("Kamera mendeteksi perubahan gerakan atau tampilan lingkungan. Periksa gambar terbaru sebelum memberikan komentar.");
    Serial.println("TARS: VISION EVENT QUEUED");
  }
}

bool isStatusQuery(const String&q){
  String s=normCmd(q);
  s.replace("statuse","status");

  if(s=="status"||s=="tars status"||s=="status tars"||s=="cek status"||s=="tars cek status"||s=="cek status tars"||s=="status kamu"||s=="tars status kamu"||s=="kondisi kamu"||s=="kondisi tars")return true;

  return s.indexOf("cek status")>=0||s.indexOf("status tars")>=0||s.indexOf("tars status")>=0||s.indexOf("status kamu")>=0||s.indexOf("kondisi kamu")>=0||s.indexOf("kondisi tars")>=0;
}

String systemStatus(){
  String s="DATA STATUS TARS SAAT INI:\n";

  s+="RAM bebas "+String(ESP.getFreeHeap()/1024.0,1)+" KB, minimum "+String(ESP.getMinFreeHeap()/1024.0,1)+" KB, blok terbesar "+String(ESP.getMaxAllocHeap()/1024.0,1)+" KB.\n";
  s+="Flash "+String(ESP.getFlashChipSize()/1048576.0,1)+" MB, sketch "+String(ESP.getSketchSize()/1024.0,1)+" KB, ruang sketch bebas "+String(ESP.getFreeSketchSpace()/1024.0,1)+" KB.\n";
  s+="LittleFS total "+String(LittleFS.totalBytes()/1024.0,1)+" KB, terpakai "+String(LittleFS.usedBytes()/1024.0,1)+" KB.\n";
  s+="CPU "+String(getCpuFrequencyMhz())+" MHz, uptime "+String(millis()/3600000UL)+" jam "+String((millis()/60000UL)%60)+" menit.\n";
  s+="Suhu ESP32 "+String(temperatureRead(),1)+" C.\n";

  s+="WiFi "+String(WiFi.status()==WL_CONNECTED?"terhubung":"terputus");

  if(WiFi.status()==WL_CONNECTED)
    s+="; RSSI "+String(WiFi.RSSI())+" dBm; IP "+WiFi.localIP().toString();

  s+=".\n";
  s+="NTP "+String(ntpOK?"valid":"belum valid")+
     ", OLED "+String(oledOK?"aktif":"error")+
     ", mic "+String(micOK?"aktif":"error")+
     ", DAC "+String(dacOK?"aktif":"off")+
     ", kamera "+String(cameraLive?"aktif":"off")+
     ", audio "+String(playing?"sedang berjalan":"idle")+
     ", sistem roda siap (tanpa sensor umpan balik gerak), STT "+
     String(sttReady?"ready":sttConnected?"connected":"idle")+".";

  return s;
}

bool cmdMatch(const String&s,const char*const*v,uint8_t n){
  for(uint8_t i=0;i<n;i++)
    if(s==v[i])return true;
  return false;
}

bool isDoorWord(String w){
  w.toLowerCase();
  w.trim();

  return w=="dor"||w=="door"||w=="doar"||(w.length()>=3&&w.length()<=6&&w.startsWith("dor"));
}

bool isDoorCmd(const String&s){
  String x=s;
  x.trim();

  if(!x.length())return false;

  int p=0,doors=0,tokens=0;

  while(p<x.length()){
    while(p<x.length()&&x[p]==' ')p++;
    if(p>=x.length())break;

    int e=x.indexOf(' ',p);
    if(e<0)e=x.length();

    String w=x.substring(p,e);
    tokens++;

    if(isDoorWord(w))doors++;
    else if(w=="tars"&&tokens==1){}
    else return false;

    if(tokens>7)return false;
    p=e+1;
  }

  return doors>0;
}

bool alarmDue(){
  if(!ntpOK||alarmRunning||deepSleepAlarmDone())return false;

  time_t now=time(nullptr);
  if(now<1704067200)return false;

  struct tm t;
  localtime_r(&now,&t);

  return t.tm_hour==6&&t.tm_min==0&&alarmLastDay!=t.tm_yday;
}

void runAlarm(){
  if(!alarmDue())return;

  time_t now=time(nullptr);
  struct tm t;
  localtime_r(&now,&t);

  alarmLastDay=t.tm_yday;
  alarmRunning=true;
  wheelsStop();

  uint32_t st=millis();
  bool played=false;

  while(millis()-st<ALARM_DURATION_MS){
    if(!playLocalAlarm())break;
    played=true;
    delay(500);
  }

  if(played)deepSleepMarkAlarmDone();

  audioStop();
  alarmRunning=false;
  wheelsStop();
  oledSetStatus(tarsMode==MODE_ONLINE?"LISTENING":"READY");
}

void checkTimeGreeting(){
  if(!ntpOK||playing||alarmRunning||greetingPlaying)return;

  uint8_t p=greetingPeriod();
  if(p==255||p==lastGreetingPeriod)return;

  greetingPlaying=true;
  String txt=greetingText(p);
  oledShowText(txt,"SALAM");

  bool ok=playTimeGreeting(p);
  if(ok)lastGreetingPeriod=p;

  greetingPlaying=false;
  wheelsStop();
  oledSetStatus(ok?(tarsMode==MODE_ONLINE?"LISTENING":"READY"):"AUDIO ERROR");
}

String ask(const String&q){
  if(!wifiOK())return "";

  WiFiClientSecure c;
  c.setInsecure();

  HTTPClient h;

  if(!h.begin(c,String(TARS_CLOUD_URL)+"/ask"))return "";

  h.setTimeout(12000);
  h.addHeader("Content-Type","application/json");

  String body;

  {
    JsonDocument j;
    j["question"]=q;
    serializeJson(j,body);
  }

  int code=h.POST(body);
  body="";

  Serial.printf("TARS: ASK HTTP=%d\n",code);

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

  Serial.printf("TARS: ASK RESPONSE LENGTH=%u\n",(unsigned)s.length());
  return s;
}

bool processOffline(const String&q){
  String s=normCmd(q);

  static const char*online[]={"online","on line","tars online","tars on line","mode online","tars mode online"};
  static const char*hari[]={"hari","hari ini","kata hari","kata kata","kata kata hari ini","kata hari ini tars","tars hari ini","tars kata hari ini"};

  if(cmdMatch(s,hari,sizeof(hari)/sizeof(*hari))){
    oledShowText("HARI INI","OFFLINE");
    playLocalMP3(hari_start,hari_end,"Kata-kata hari ini, Ooo celeng Celeng itu ga tau ilmu huruf.");
    oledSetStatus("READY");
    return true;
  }

  if(cmdMatch(s,online,sizeof(online)/sizeof(*online))){
    Serial.println("TARS: SWITCH OFFLINE -> ONLINE");

    oledShowText("ONLINE","OFFLINE");
    tarsMode=MODE_ONLINE;

    personalityResetSpeechTimer();
    autoSpeechResetTimer();
    tarsEmotionResetPending();

    portENTER_CRITICAL(&visionEventMux);
    pendingVisionCheck=false;
    portEXIT_CRITICAL(&visionEventMux);

    autoSpeechResetTimer();

    playLocalMP3(online_start,online_end,"Mode online aktif, tuan");

    Serial.println("TARS: MODE ONLINE");
    return true;
  }

  return false;
}

void processQuestion(const String&q){
  String nq=normCmd(q);

  if(tarsMode==MODE_OFFLINE){
    processOffline(q);
    return;
  }

  if(nq=="offline"||nq=="off line"||nq=="tars offline"||nq=="tars off line"||nq=="mode offline"||nq=="mode off line"||nq=="tars mode offline"||nq=="tars mode off line"){
    Serial.println("TARS: SWITCH ONLINE -> OFFLINE");

    closeSTT();
    tarsMode=MODE_OFFLINE;

    portENTER_CRITICAL(&visionEventMux);
    pendingVisionCheck=false;
    portEXIT_CRITICAL(&visionEventMux);

    autoSpeechResetTimer();

    sttReady=false;
    sttDone=false;
    sttError=false;

    oledShowText("OFFLINE","ONLINE");
    playLocalMP3(offline_start,offline_end,"Mode offline aktif, tuan");
    oledSetStatus("READY");

    Serial.println("TARS: MODE OFFLINE");
    return;
  }

  oledShowText(q,"STT");
  delay(300);

  tarsEmotionQuestion(q);

  if(tarsEmotionHasEvent()){
    TarsEmotionEvent e=tarsEmotionTakeEvent();
    String prompt=tarsEmotionPrompt(e);

    if(prompt.length()){
      prompt="Ucapan pengguna tadi: \""+q+"\". "+prompt;
      processOnlineRequest(prompt,false,false,false);
    }

    return;
  }

  bool vision=needsVision(q);
  bool status=isStatusQuery(q);
  String request=q;

  switch(random(0,5)){
    case 0:request+=" Jawab dengan gaya santai dan natural.";break;
    case 1:request+=" Gunakan pilihan kata yang segar dan tidak kaku.";break;
    case 2:request+=" Jawab dengan gaya cerdas dan sedikit humor jika sesuai.";break;
    case 3:request+=" Gunakan kalimat natural dan variasikan cara penyampaian.";break;
    default:request+=" Jawab secara ramah, singkat, dan tidak monoton.";break;
  }

  request+=" Jangan mengulang kalimat atau sapaan yang sama jika tidak diperlukan.";
  processOnlineRequest(request,vision,status,false);
}

void setup(){
  Serial.begin(SERIAL_BAUD);

  Wire.begin(OLED_SDA,OLED_SCL);
  Wire.setClock(400000);

  oledOK=oled.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR);

  if(oledOK){
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(2);
    oled.setCursor(36,0);
    oled.print("TARS");
    oled.setTextSize(1);
    oled.setCursor(3,27);
    oled.print("BOOT");
    oled.display();
  }

  wheelsBegin();

  previewMux=xSemaphoreCreateMutex();
  envMux=xSemaphoreCreateMutex();
  cameraMux=xSemaphoreCreateMutex();

  micOK=initMic();

  if(!LittleFS.begin(true))
    Serial.println("TARS: LITTLEFS ERROR");
  else
    Serial.printf("TARS: LITTLEFS READY %u/%u KB\n",(unsigned)(LittleFS.usedBytes()/1024),(unsigned)(LittleFS.totalBytes()/1024));

  Serial.println("TARS: DAC GPIO26 ULP DAC2");
  Serial.println("TARS: AUDIO MP3/WAV -> 22050Hz/16bit");
  Serial.println("TARS: INMP441 SCK=18 WS=2 SD=15");
  Serial.println("TARS: L9110S A=23/16 B=17/19");
  Serial.println("TARS: MIC PEAK=14000/8000 RMS=3000/1800 PREROLL=250 ms BUF=256");
  Serial.println("TARS: STT ONLINE REALTIME PCM");
  Serial.println("TARS: STT OFFLINE REALTIME PCM");
  Serial.println("TARS: VISION ONLINE ONLY");

  Serial.printf("TARS: OV7670 D0..D7=%d,%d,%d,%d,%d,%d,%d,%d XCLK=%d PCLK=%d VSYNC=%d HREF=%d SCCB=%d/%d\n",CAM_D0,CAM_D1,CAM_D2,CAM_D3,CAM_D4,CAM_D5,CAM_D6,CAM_D7,CAM_XCLK,CAM_PCLK,CAM_VSYNC,CAM_HREF,CAM_SIOD,CAM_SIOC);

  Serial.println("TARS: GPIO4 RESERVED FOR OV7670 XCLK");
  Serial.println("TARS: BLUETOOTH DISABLED");
  Serial.println("TARS: MODE OFFLINE");
  ramDiag("BOOT");

  if(oledOK)
    xTaskCreatePinnedToCore(oledTask,"TARS_OLED",4096,nullptr,1,nullptr,0);

  wifiManagerBegin();

  if(wifiManagerConnect(true))
    if(syncTime())
      checkTimeGreeting();

  oledSetStatus("READY");

  envBegin();
  visionLiveBegin();
  personalityBegin();
  tarsEmotionBegin();
  autonomyBegin();

  if(!personalityTaskHandle){
    BaseType_t result=xTaskCreate(personalityTask,"TARS_Personality",2048,nullptr,1,&personalityTaskHandle);

    if(result!=pdPASS){
      personalityTaskHandle=nullptr;
      Serial.println("TARS: PERSONALITY TASK FAILED");
    }else Serial.println("TARS: PERSONALITY TASK READY");
  }

  autoSpeechBegin(autoSpeechCallback);
  startCamera();

  xTaskCreatePinnedToCore(cameraTask,"TARS_EYE",4096,nullptr,2,nullptr,1);

  ramDiag("READY");
  wheelsStop();

  Serial.println("TARS: LIFE READY");
  Serial.println("TARS: PERSONALITY READY");
  Serial.println("TARS: AUTONOMY READY");
  Serial.println("TARS: AUTO SPEECH READY");
}

void enterTarsDeepSleep(){
  Serial.println("TARS: PREPARING DEEP SLEEP");

  sleepOLEDStop();
  wheelsStop();
  autonomyStop();
  closeSTT();
  audioStop();
  playing=false;

  visionLivePause();
  stopCamera();

  if(micOK){
    i2s_driver_uninstall(MIC_PORT);
    micOK=false;
  }

  if(oledOK){
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(2);
    oled.setCursor(25,25);
    oled.print("TARS SLEEP");
    oled.display();
  }

  ramDiag("BEFORE-DEEP-SLEEP");
  deepSleepEnter();
}

void loop(){
  time_t now=time(nullptr);
  struct tm t={};

  if(now>=1704067200){
    localtime_r(&now,&t);

    bool prepare=(t.tm_hour==21&&t.tm_min>=58)||(t.tm_hour==22&&t.tm_min==0);

    if(prepare&&!sleepPreparing){
      sleepPreparing=true;
      sleepPrepareAt=millis();
      sleepOLEDStart();
      Serial.println("TARS: SLEEP ANIMATION START");
    }

    if(sleepPreparing&&t.tm_hour>=22){
      sleepOLEDStop();
      enterTarsDeepSleep();
      return;
    }
  }

  ramMonitor();

  if(playing){
    wheelsStop();
    autonomyStop();
    delay(1);
    return;
  }

  if(tarsMode==MODE_ONLINE&&!wifiOK()){
    oledSetStatus("WIFI ERROR");
    delay(500);
    return;
  }

  if(alarmDue()){
    autonomyStop();
    runAlarm();
    return;
  }

  checkTimeGreeting();

  if(playing){
    wheelsStop();
    autonomyStop();
    return;
  }

  if(tarsMode==MODE_ONLINE&&!playing&&!sttConnected&&tarsEmotionHasEvent())
    if(processEmotionEvent())return;

  String q=tarsMode==MODE_ONLINE?recordRealtime():recordOffline();

  if(q.length()){
    wheelsStop();
    autonomyStop();
    processQuestion(q);
  }else if(!oledSpecial)
    oledSetStatus(tarsMode==MODE_ONLINE?"LISTENING":"READY");

  if(!playing&&tarsMode==MODE_ONLINE&&!sttConnected){
    processVisionEvent();
    autoSpeechUpdate(true,false,false);
  }

  delay(1);
}
