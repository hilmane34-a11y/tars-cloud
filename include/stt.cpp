#include "stt.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <math.h>
#include <string.h>
#include "config.h"
#include "tars_emotion.h"

#define MIC_PORT I2S_NUM_1
#define MIC_RATE 16000
#define MIC_THRESHOLD 7000
#define MIC_SILENCE 4000
#define RECORD_MIN_MS 500
#define SILENCE_MS 800
#define PREROLL_MS 165
#define STT_CONNECT_TIMEOUT 20000

static const size_t BUF=256;
static const size_t PREROLL_SAMPLES=MIC_RATE*PREROLL_MS/1000;
static int32_t rawBuf[BUF/4];
static int16_t pcmBuf[BUF/4];
static int16_t preBuf[PREROLL_SAMPLES];
static int16_t sendBuf[256];

static WebSocketsClient sttWS;
static bool sttStarted=false;

static const char* STT_HOST_LOCAL=
  "tars-cloud-v1.hilmane34.workers.dev";

String sttFinal,sttPartial;
bool micOK=false;
bool sttConnected=false;
bool sttReady=false;
bool sttDone=false;
bool sttError=false;
bool sttClosing=false;

extern bool wifiOK();
extern void oledSetStatus(const String&);
extern void oledSetListening();

// Dipertahankan agar kompatibel dengan kode lama.
// Tidak ada cooldown normal.
bool sttCooling(){
  return false;
}

// PANGGIL TERUS-MENERUS DARI loop() DI main.cpp
void sttLoop(){
  if(!sttStarted || sttClosing) return;
  sttWS.loop();
}

static void sttEvent(WStype_t type,uint8_t*payload,size_t length){
  if(type==WStype_CONNECTED){
    sttConnected=true;
    sttReady=false;
    sttError=false;
    Serial.println("TARS: STT WS CONNECTED");
    oledSetStatus("STT CONNECTED");
    return;
  }

  if(type==WStype_DISCONNECTED){
    sttConnected=false;
    sttReady=false;
    if(!sttClosing) sttError=true;
    Serial.println(sttClosing
      ? "TARS: STT WS DISCONNECTED (MANUAL)"
      : "TARS: STT WS LOST - AUTO RECONNECT");
    return;
  }

  if(type==WStype_ERROR){
    sttConnected=false;
    sttReady=false;
    if(!sttClosing) sttError=true;
    Serial.println("TARS: STT WS ERROR - AUTO RECONNECT");
    oledSetStatus("STT RECONNECT");
    return;
  }

  if(type!=WStype_TEXT || !payload || !length) return;

  String msg;
  if(!msg.reserve(length+1)){
    Serial.println("TARS: STT JSON ALLOC FAILED");
    sttError=true;
    return;
  }

  for(size_t i=0;i<length;i++) msg+=(char)payload[i];

  JsonDocument j;
  if(deserializeJson(j,msg)){
    Serial.println("TARS: STT JSON PARSE FAILED");
    return;
  }

  const char*t=j["type"]|"";

  if(strcmp(t,"ready")==0){
    sttReady=true;
    sttError=false;
    Serial.println("TARS: STT REALTIME READY");
    oledSetStatus("STT READY");
  }
  else if(strcmp(t,"partial")==0){
    sttPartial=j["text"]|"";
    sttPartial.trim();
    if(sttPartial.length())
      Serial.printf("TARS: STT PARTIAL = %s\n",
                    sttPartial.c_str());
  }
  else if(strcmp(t,"final")==0){
    sttFinal=j["text"]|"";
    sttFinal.trim();
    sttDone=true;
    Serial.printf("TARS: YOU SAID = %s\n",
                  sttFinal.c_str());
  }
  else if(strcmp(t,"error")==0){
    sttError=true;
    sttDone=true;
    Serial.printf("TARS: STT ERROR = %s\n",
                  j["error"]|"unknown");
    oledSetStatus("STT ERROR");
  }
}

// Hanya untuk penutupan yang memang diminta kode lain.
// Tidak dipanggil saat ucapan selesai atau idle timeout.
void closeSTT(uint32_t cooldown){
  (void)cooldown;
  sttClosing=true;
  if(sttStarted) sttWS.disconnect();
  sttConnected=false;
  sttReady=false;
  sttStarted=false;
  sttDone=false;
  sttError=false;
  sttClosing=false;
}

bool startSTT(){
  if(!wifiOK()){
    Serial.printf("TARS: STT WAIT WIFI status=%d\n",
                  WiFi.status());
    return false;
  }

  if(!micOK){
    Serial.println("TARS: STT BLOCK MIC=false");
    return false;
  }

  // Pakai koneksi lama jika masih siap.
  if(sttConnected && sttReady){
    sttDone=false;
    sttError=false;
    sttFinal="";
    sttPartial="";
    return true;
  }

  // beginSSL cukup sekali. Reconnect selanjutnya ditangani
  // WebSocketsClient, bukan membuat objek/sesi dari awal di sini.
  if(!sttStarted){
    Serial.printf(
      "TARS: STT FIRST CONNECT heap=%u maxBlock=%u RSSI=%d\n",
      ESP.getFreeHeap(),ESP.getMaxAllocHeap(),WiFi.RSSI());

    sttWS.onEvent(sttEvent);
    sttWS.setReconnectInterval(4000);
    sttWS.enableHeartbeat(15000,5000,2);
    sttWS.beginSSL(STT_HOST_LOCAL,443,"/stt");
    sttStarted=true;
  }

  sttDone=false;
  sttError=false;
  sttFinal="";
  sttPartial="";

  uint32_t startedAt=millis();

  while(!sttReady &&
        millis()-startedAt<STT_CONNECT_TIMEOUT){
    sttWS.loop();

    if(!wifiOK()){
      delay(20);
      continue;
    }

    delay(2);
    yield();
  }

  if(!sttConnected || !sttReady){
    Serial.printf(
      "TARS: STT WAIT RECONNECT heap=%u maxBlock=%u\n",
      ESP.getFreeHeap(),ESP.getMaxAllocHeap());
    return false;
  }

  sttError=false;
  sttDone=false;
  sttFinal="";
  sttPartial="";
  Serial.println("TARS: ONLINE STT READY - PERSISTENT");
  return true;
}

String stopSTT(uint32_t samples){
  if(!sttConnected || !sttReady) return "";

  JsonDocument j;
  j["type"]="end";
  j["timestamp"]=(double)samples/MIC_RATE;

  String msg;
  if(!serializeJson(j,msg)){
    Serial.println("TARS: STT END JSON FAILED");
    return "";
  }

  if(!sttWS.sendTXT(msg)){
    Serial.println("TARS: STT END SEND FAILED");
    sttError=true;
    return "";
  }

  uint32_t startedAt=millis();

  while(!sttDone && !sttError &&
        millis()-startedAt<6000){
    sttWS.loop();
    delay(2);
    yield();
  }

  String result=sttFinal;

  if(!sttDone)
    Serial.println("TARS: STT FINAL TIMEOUT");

  // PENTING: jangan disconnect.
  // WebSocket dipertahankan untuk ucapan berikutnya.
  sttDone=false;
  sttFinal="";
  sttPartial="";

  return result;
}

String recordRealtime(){
  if(!startSTT()) return "";

  oledSetListening();

  size_t prePos=0,preCount=0;
  uint32_t voiceStart=0,lastVoice=0,samples=0;
  uint32_t listenStart=millis();
  bool voice=false;

  sttDone=false;
  sttError=false;
  sttFinal="";
  sttPartial="";

  for(;;){
    sttWS.loop();

    if(sttError || !sttConnected || !sttReady){
      Serial.println("TARS: STT LOST DURING RECORD");
      return "";
    }

    size_t bytes=0;
    if(i2s_read(MIC_PORT,rawBuf,sizeof(rawBuf),
                &bytes,pdMS_TO_TICKS(30))!=ESP_OK)
      continue;

    size_t count=bytes/4;
    if(count>BUF/4) count=BUF/4;

    int32_t peak=0;
    uint64_t sum=0;

    for(size_t i=0;i<count;i++){
      int32_t v=constrain(rawBuf[i]>>16,-32768,32767);
      pcmBuf[i]=(int16_t)v;
      int32_t a=abs(v);
      if(a>peak) peak=a;
      sum+=(uint64_t)a*a;
    }

    uint32_t rms=count
      ? (uint32_t)sqrt((double)sum/count) : 0;

    if(!voice){
      if(millis()-listenStart>=10000){
        Serial.println("TARS: STT IDLE - KEEP WS CONNECTED");
        return "";
      }

      for(size_t i=0;i<count;i++){
        preBuf[prePos]=pcmBuf[i];
        prePos=(prePos+1)%PREROLL_SAMPLES;
        if(preCount<PREROLL_SAMPLES) preCount++;
      }

      if(peak>=MIC_THRESHOLD || rms>=3000){
        voice=true;
        voiceStart=lastVoice=millis();

        tarsEmotionSpeechPeak(
          (uint16_t)(peak>32767?32767:peak),true);

        size_t start=
          preCount==PREROLL_SAMPLES ? prePos : 0;
        size_t nsend=0;

        for(size_t i=0;i<preCount;i++){
          sendBuf[nsend++]=
            preBuf[(start+i)%PREROLL_SAMPLES];

          if(nsend==256){
            if(!sttWS.sendBIN(
                (uint8_t*)sendBuf,
                nsend*sizeof(int16_t))){
              sttError=true;
              break;
            }
            nsend=0;
          }
        }

        if(nsend && !sttError &&
           !sttWS.sendBIN((uint8_t*)sendBuf,
                          nsend*sizeof(int16_t)))
          sttError=true;

        samples+=preCount;

        Serial.printf(
          "TARS: ONLINE VOICE PEAK=%ld RMS=%lu\n",
          (long)peak,(unsigned long)rms);
      }
    }
    else{
      if(!sttWS.sendBIN((uint8_t*)pcmBuf,
                        count*sizeof(int16_t))){
        Serial.println("TARS: STT PCM SEND FAILED");
        sttError=true;
        return "";
      }

      samples+=count;

      if(peak>=MIC_SILENCE || rms>=1800)
        lastVoice=millis();

      if(millis()-voiceStart>=RECORD_MIN_MS &&
         millis()-lastVoice>=SILENCE_MS)
        break;
    }

    yield();
  }

  if(!voice){
    Serial.println("TARS: MIC AUDIO TOO LOW");
    return "";
  }

  if(sttError) return "";

  return stopSTT(samples);
}
