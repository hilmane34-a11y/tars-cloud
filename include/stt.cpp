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
#define PREROLL_MS 200
#define STT_CONNECT_TIMEOUT 10000

static const size_t BUF=256;
static const size_t PREROLL_SAMPLES=MIC_RATE*PREROLL_MS/1000;

static int32_t rawBuf[BUF/4];
static int16_t pcmBuf[BUF/4];
static int16_t preBuf[PREROLL_SAMPLES];
static int16_t sendBuf[256];

static WebSocketsClient sttWS;
static bool sttStarted=false;
static bool sttClosing=false;

static const char* STT_HOST_LOCAL=
  "tars-cloud-v1.hilmane34.workers.dev";

String sttFinal,sttPartial;
bool micOK=false;
bool sttConnected=false;
bool sttReady=false;
bool sttDone=false;
bool sttError=false;

extern bool wifiOK();
extern void oledSetStatus(const String&);
extern void oledSetListening();

bool sttCooling(){
  return false;
}

// WebSocket hanya dilayani saat sesi STT aktif.
void sttLoop(){
  if(sttStarted && !sttClosing)
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
    Serial.println("TARS: STT WS DISCONNECTED");
    return;
  }

  if(type==WStype_ERROR){
    sttConnected=false;
    sttReady=false;
    if(!sttClosing) sttError=true;
    Serial.println("TARS: STT WS ERROR");
    return;
  }

  if(type!=WStype_TEXT || !payload || !length)
    return;

  String msg;
  if(!msg.reserve(length+1)){
    sttError=true;
    return;
  }

  for(size_t i=0;i<length;i++)
    msg+=(char)payload[i];

  JsonDocument j;
  if(deserializeJson(j,msg)){
    Serial.println("TARS: STT JSON PARSE FAILED");
    return;
  }

  const char*t=j["type"]|"";

  if(strcmp(t,"ready")==0){
    sttReady=true;
    sttError=false;
    Serial.println("TARS: STT READY");
    oledSetStatus("STT READY");
  }
  else if(strcmp(t,"partial")==0){
    sttPartial=j["text"]|"";
    sttPartial.trim();
    if(sttPartial.length())
      Serial.printf("TARS: PARTIAL=%s\n",sttPartial.c_str());
  }
  else if(strcmp(t,"final")==0){
    sttFinal=j["text"]|"";
    sttFinal.trim();
    sttDone=true;
    Serial.printf("TARS: FINAL=%s\n",sttFinal.c_str());
  }
  else if(strcmp(t,"error")==0){
    sttError=true;
    sttDone=true;
    Serial.printf("TARS: STT ERROR=%s\n",j["error"]|"unknown");
    oledSetStatus("STT ERROR");
  }
}

// Putuskan sesi dan hentikan reconnect.
void closeSTT(uint32_t cooldown){
  (void)cooldown;

  if(!sttStarted) {
    sttConnected=false;
    sttReady=false;
    return;
  }

  sttClosing=true;
  sttWS.setReconnectInterval(0);
  sttWS.disconnect();

  sttStarted=false;
  sttConnected=false;
  sttReady=false;
  sttDone=false;
  sttError=false;
  sttFinal="";
  sttPartial="";
  sttClosing=false;

  Serial.printf("TARS: STT CLOSED heap=%u\n",ESP.getFreeHeap());
}

// Membuka WebSocket hanya ketika ada suara.
bool startSTT(){
  if(!wifiOK() || !micOK){
    Serial.println("TARS: STT BLOCKED WIFI/MIC");
    return false;
  }

  // Bersihkan sesi lama sebelum TLS baru.
  closeSTT(0);

  sttFinal="";
  sttPartial="";
  sttDone=false;
  sttError=false;
  sttConnected=false;
  sttReady=false;

  Serial.printf("TARS: STT CONNECT heap=%u maxBlock=%u\n",
    ESP.getFreeHeap(),ESP.getMaxAllocHeap());

  sttWS.onEvent(sttEvent);
  sttWS.setReconnectInterval(0);
  sttWS.enableHeartbeat(15000,5000,2);
  sttWS.beginSSL(STT_HOST_LOCAL,443,"/stt");
  sttStarted=true;

  uint32_t startedAt=millis();

  while(!sttReady &&
        !sttError &&
        millis()-startedAt<STT_CONNECT_TIMEOUT){

    sttWS.loop();

    if(!wifiOK())
      break;

    delay(2);
    yield();
  }

  if(!sttConnected || !sttReady || sttError){
    Serial.println("TARS: STT CONNECT FAILED");
    closeSTT(0);
    return false;
  }

  sttDone=false;
  sttError=false;
  sttFinal="";
  sttPartial="";

  Serial.println("TARS: STT SESSION READY");
  return true;
}

// Kirim akhir audio, tunggu final, lalu tutup WebSocket.
// Setelah fungsi ini selesai, TLS/API dan DAC boleh dimulai.
String stopSTT(uint32_t samples){
  if(!sttConnected || !sttReady){
    closeSTT(0);
    return "";
  }

  JsonDocument j;
  j["type"]="end";
  j["timestamp"]=(double)samples/MIC_RATE;

  String msg;
  if(!serializeJson(j,msg) || !sttWS.sendTXT(msg)){
    Serial.println("TARS: STT END SEND FAILED");
    closeSTT(0);
    return "";
  }

  uint32_t startedAt=millis();

  while(!sttDone && !sttError &&
        millis()-startedAt<6000){
    sttWS.loop();
    delay(2);
    yield();
  }

  String result="";
  if(sttDone && !sttError)
    result=sttFinal;

  if(!sttDone)
    Serial.println("TARS: STT FINAL TIMEOUT");

  // Wajib putus sebelum recordRealtime() kembali ke main.
  closeSTT(0);

  return result;
}

String recordRealtime(){
  if(!micOK || !wifiOK())
    return "";

  // STANDBY: mikrofon aktif, WebSocket belum dibuat.
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
    size_t bytes=0;

    if(i2s_read(MIC_PORT,rawBuf,sizeof(rawBuf),
        &bytes,pdMS_TO_TICKS(30))!=ESP_OK)
      continue;

    size_t count=bytes/4;
    if(count>BUF/4) count=BUF/4;
    if(!count) continue;

    int32_t peak=0;
    uint64_t sum=0;

    for(size_t i=0;i<count;i++){
      int32_t v=constrain(rawBuf[i]>>16,-32768,32767);
      pcmBuf[i]=(int16_t)v;

      int32_t a=abs(v);
      if(a>peak) peak=a;
      sum+=(uint64_t)a*a;
    }

    uint32_t rms=(uint32_t)sqrt((double)sum/count);

    if(!voice){
      if(millis()-listenStart>=10000){
        Serial.println("TARS: IDLE - WS NEVER OPENED");
        return "";
      }

      for(size_t i=0;i<count;i++){
        preBuf[prePos]=pcmBuf[i];
        prePos=(prePos+1)%PREROLL_SAMPLES;
        if(preCount<PREROLL_SAMPLES) preCount++;
      }

      if(peak>=MIC_THRESHOLD || rms>=3000){
        Serial.printf("TARS: VOICE DETECTED peak=%ld rms=%lu\n",
          (long)peak,(unsigned long)rms);

        // Baru sekarang buka koneksi TLS/WebSocket.
        // Catatan: audio yang masuk selama handshake tidak
        // bisa dikirim; gunakan jeda singkat sebelum bicara
        // jika koneksi internet sedang lambat.
        if(!startSTT())
          return "";

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
            samples+=nsend;
            nsend=0;
          }
        }

        if(nsend && !sttError){
          if(!sttWS.sendBIN((uint8_t*)sendBuf,
                            nsend*sizeof(int16_t)))
            sttError=true;
          else
            samples+=nsend;
        }

        if(sttError){
          closeSTT(0);
          return "";
        }
      }
    }
    else{
      sttWS.loop();

      if(sttError || !sttConnected || !sttReady){
        Serial.println("TARS: STT LOST DURING RECORD");
        closeSTT(0);
        return "";
      }

      if(!sttWS.sendBIN((uint8_t*)pcmBuf,
                        count*sizeof(int16_t))){
        Serial.println("TARS: PCM SEND FAILED");
        closeSTT(0);
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

  if(sttError){
    closeSTT(0);
    return "";
  }

  return stopSTT(samples);
}
