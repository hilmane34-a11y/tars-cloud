#include "stt.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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
#define MIC_SCK 18
#define MIC_WS 2
#define MIC_SD 15
#define MIC_RATE 16000
#define MIC_THRESHOLD 6000
#define MIC_SILENCE 4000
#define RECORD_MIN_MS 500
#define SILENCE_MS 800
#define PREROLL_MS 150
#define STT_CONNECT_TIMEOUT_MS 10000
#define STT_READY_TIMEOUT_MS 8000
#define STT_STOP_TIMEOUT_MS 6000

static const size_t BUF=256;
static const size_t RAW_SAMPLES=BUF/4;
static const size_t PREROLL_SAMPLES=MIC_RATE*PREROLL_MS/1000;
static int32_t rawBuf[RAW_SAMPLES];
static int16_t pcmBuf[RAW_SAMPLES];
static int16_t preBuf[PREROLL_SAMPLES];
static int16_t sendBuf[256];
static WebSocketsClient sttWS;
static bool wsStarted=false;
static uint32_t sttRetryAt=0;
static bool sttRetryShown=false;
static bool sttStopped=false;

String sttFinal,sttPartial;
bool micOK=false;
bool sttConnected=false,sttReady=false,sttDone=false,sttError=false,sttClosing=false;
volatile bool sttSessionActive=false;

static const char *STT_HOST_LOCAL="tars-cloud-v1.hilmane34.workers.dev";

extern bool wifiOK();
extern void oledSetStatus(const String &);
extern void oledSetListening();

static bool wsSendTXT(const String &message){
    if(!sttConnected)return false;
    String payload=message;
    return sttWS.sendTXT(payload);
}
static bool wsSendBIN(const uint8_t *data,size_t length){
    if(!sttConnected||!data||!length)return false;
    return sttWS.sendBIN(const_cast<uint8_t *>(data),length);
}
bool sttCooling(){return (int32_t)(millis()-sttRetryAt)<0;}

static void sttEvent(WStype_t type,uint8_t *payload,size_t length){
    if(type==WStype_CONNECTED){
        sttConnected=true;
        Serial.println("TARS: PERSISTENT WSS CONNECTED");
        oledSetStatus("WSS CONNECTED");
        return;
    }
    if(type==WStype_DISCONNECTED){
        bool wasActive=sttSessionActive;
        sttConnected=false;
        sttReady=false;
        sttSessionActive=false;
        sttStopped=true;
        if(!sttClosing&&wasActive)sttError=true;
        Serial.println(sttClosing?"TARS: WSS DISCONNECTED (NORMAL)":"TARS: WSS DISCONNECTED");
        return;
    }
    if(type==WStype_ERROR){
        if(sttSessionActive&&!sttClosing)sttError=true;
        Serial.println("TARS: WSS ERROR");
        return;
    }
    if(type!=WStype_TEXT||!payload||!length)return;

    String msg;
    if(!msg.reserve(length+1)){
        Serial.println("TARS: STT JSON ALLOC FAILED");
        if(sttSessionActive)sttError=true;
        return;
    }
    for(size_t i=0;i<length;i++)msg+=(char)payload[i];

    JsonDocument j;
    if(deserializeJson(j,msg)){
        Serial.println("TARS: STT JSON PARSE FAILED");
        return;
    }
    const char *t=j["type"]|"";
    if(strcmp(t,"pong")==0)return;

    if(strcmp(t,"ready")==0){
        sttReady=true;
        sttError=false;
        sttRetryShown=false;
        Serial.println("TARS: STT SESSION READY");
        oledSetStatus("STT READY");
        return;
    }
    if(strcmp(t,"partial")==0){
        sttPartial=j["text"]|"";
        sttPartial.trim();
        if(sttPartial.length())Serial.printf("TARS: STT PARTIAL = %s\n",sttPartial.c_str());
        return;
    }
    if(strcmp(t,"final")==0){
        sttFinal=j["text"]|"";
        sttFinal.trim();
        sttDone=true;
        Serial.printf("TARS: YOU SAID = %s\n",sttFinal.c_str());
        return;
    }
    if(strcmp(t,"stt_stopped")==0){
        sttStopped=true;
        sttReady=false;
        sttSessionActive=false;
        Serial.println("TARS: STT SESSION STOPPED");
        return;
    }
    if(strcmp(t,"error")==0){
        const char *e=j["error"]|"unknown";
        sttError=true;
        sttDone=true;
        Serial.printf("TARS: STT ERROR = %s\n",e);
        oledSetStatus("STT ERROR");
        return;
    }
}

void sttService(){
    if(!wsStarted&&WiFi.status()==WL_CONNECTED){
        sttWS.onEvent(sttEvent);
        sttWS.setReconnectInterval(5000);
        sttWS.enableHeartbeat(15000,5000,2);
        Serial.println("TARS: START PERSISTENT WSS");
        sttWS.beginSSL(STT_HOST_LOCAL,443,"/stt");
        wsStarted=true;
    }
    if(wsStarted)sttWS.loop();
}

void closeSTT(uint32_t cooldown){
    sttClosing=true;
    if(sttConnected&&sttSessionActive){
        JsonDocument j;
        j["type"]="end";
        String message;
        serializeJson(j,message);
        wsSendTXT(message);
    }
    sttSessionActive=false;
    sttReady=false;
    sttStopped=true;
    sttRetryAt=millis()+cooldown;
    sttRetryShown=false;
    sttClosing=false;
}

bool startSTT(){
    if(!wifiOK()){
        Serial.printf("TARS: STT BLOCK WIFI status=%d\n",WiFi.status());
        return false;
    }
    if(!micOK){
        Serial.println("TARS: STT BLOCK MIC=false");
        return false;
    }
    if(sttCooling()){
        if(!sttRetryShown){
            Serial.println("TARS: STT RETRY COOLDOWN");
            sttRetryShown=true;
        }
        return false;
    }

    uint32_t started=millis();
    while(!sttConnected&&millis()-started<STT_CONNECT_TIMEOUT_MS){
        sttService();
        delay(2);
        yield();
        if(WiFi.status()!=WL_CONNECTED)break;
    }
    if(!sttConnected){
        Serial.println("TARS: WSS CONNECT TIMEOUT");
        sttRetryAt=millis()+STT_ERROR_COOLDOWN;
        sttRetryShown=false;
        return false;
    }

    sttFinal="";
    sttPartial="";
    sttDone=false;
    sttError=false;
    sttReady=false;
    sttStopped=false;
    sttSessionActive=false;
    sttRetryShown=false;
    Serial.println("TARS: WSS READY FOR STT");
    return true;
}

String stopSTT(uint32_t samples){
    if(!sttConnected||!sttSessionActive)return sttFinal;

    JsonDocument j;
    j["type"]="end";
    j["timestamp"]=(double)samples/MIC_RATE;
    String msg;
    if(serializeJson(j,msg)==0){
        Serial.println("TARS: STT END JSON FAILED");
        closeSTT(STT_ERROR_COOLDOWN);
        return "";
    }
    if(!wsSendTXT(msg)){
        Serial.println("TARS: STT END SEND FAILED");
        closeSTT(STT_ERROR_COOLDOWN);
        return "";
    }

    uint32_t started=millis();
    while(!sttStopped&&!sttError&&millis()-started<STT_STOP_TIMEOUT_MS){
        sttService();
        delay(2);
        yield();
    }

    String result=sttFinal;
    sttSessionActive=false;
    sttReady=false;
    sttRetryAt=millis()+(sttError?STT_ERROR_COOLDOWN:STT_NORMAL_COOLDOWN);
    sttRetryShown=false;
    return result;
}

static bool sendPCM(const int16_t *data,size_t count){
    if(!data||!count)return true;
    return wsSendBIN(reinterpret_cast<const uint8_t *>(data),count*sizeof(int16_t));
}
static bool sendSamples(const int16_t *data,size_t count){
    size_t offset=0;
    while(offset<count){
        size_t n=count-offset;
        if(n>256)n=256;
        if(!sendPCM(data+offset,n))return false;
        offset+=n;
    }
    return true;
}
static bool readMic(size_t &count,int32_t &peak,uint32_t &rms){
    size_t bytes=0;
    if(i2s_read(MIC_PORT,rawBuf,sizeof(rawBuf),&bytes,pdMS_TO_TICKS(30))!=ESP_OK){
        count=0;
        return false;
    }
    count=bytes/sizeof(int32_t);
    peak=0;
    uint64_t sum=0;
    for(size_t i=0;i<count;i++){
        int32_t v=constrain(rawBuf[i]>>16,-32768,32767);
        pcmBuf[i]=(int16_t)v;
        int32_t a=abs(v);
        if(a>peak)peak=a;
        sum+=(uint64_t)a*a;
    }
    rms=count?(uint32_t)sqrt((double)sum/count):0;
    return true;
}

String recordSTT(){
    if(!startSTT())return "";
    oledSetListening();

    size_t prePos=0,preCount=0;
    uint32_t voiceStart=0,lastVoice=0,samples=0,listenStart=millis();
    bool voice=false;

    for(;;){
        sttService();
        if(sttError)break;

        size_t count=0;
        int32_t peak=0;
        uint32_t rms=0;
        if(!readMic(count,peak,rms)||!count)continue;

        if(!voice){
            if(millis()-listenStart>=STT_IDLE_TIMEOUT_MS){
                Serial.println("TARS: STT IDLE TIMEOUT");
                sttRetryAt=millis()+STT_NORMAL_COOLDOWN;
                return "";
            }

            for(size_t i=0;i<count;i++){
                preBuf[prePos]=pcmBuf[i];
                prePos++;
                if(prePos>=PREROLL_SAMPLES)prePos=0;
                if(preCount<PREROLL_SAMPLES)preCount++;
            }

            if(peak>=MIC_THRESHOLD||rms>=3000){
                voice=true;
                voiceStart=millis();
                lastVoice=voiceStart;
                sttSessionActive=true;
                sttReady=false;
                sttDone=false;
                sttError=false;
                sttStopped=false;

                JsonDocument startMessage;
                startMessage["type"]="stt_start";
                String startJSON;
                serializeJson(startMessage,startJSON);
                if(!wsSendTXT(startJSON)){
                    Serial.println("TARS: STT START SEND FAILED");
                    closeSTT(STT_ERROR_COOLDOWN);
                    return "";
                }

                tarsEmotionSpeechPeak((uint16_t)(peak>32767?32767:peak),true);
                Serial.printf("TARS: ONLINE VOICE PEAK=%ld RMS=%lu\n",(long)peak,(unsigned long)rms);

                // Tunggu READY sambil membuang audio; jangan menimbun PCM.
                uint32_t waitStart=millis();
                while(!sttReady&&!sttError&&millis()-waitStart<STT_READY_TIMEOUT_MS){
                    sttService();
                    size_t discardCount=0;
                    int32_t discardPeak=0;
                    uint32_t discardRms=0;
                    readMic(discardCount,discardPeak,discardRms);
                    if(discardPeak>=MIC_SILENCE||discardRms>=1800)lastVoice=millis();
                    delay(1);
                    yield();
                }

                if(!sttReady||sttError){
                    Serial.println("TARS: STT SESSION READY TIMEOUT");
                    closeSTT(STT_ERROR_COOLDOWN);
                    return "";
                }

                Serial.println("TARS: STREAMING PCM REALTIME");
                size_t preStart=preCount==PREROLL_SAMPLES?prePos:0;
                for(size_t i=0;i<preCount;){
                    size_t n=preCount-i;
                    if(n>256)n=256;
                    for(size_t k=0;k<n;k++)
                        sendBuf[k]=preBuf[(preStart+i+k)%PREROLL_SAMPLES];
                    if(!sendPCM(sendBuf,n)){
                        sttError=true;
                        break;
                    }
                    i+=n;
                }
                if(sttError)break;
                samples+=preCount;

                while(!sttError){
                    sttService();
                    if(sttError)break;

                    size_t liveCount=0;
                    int32_t livePeak=0;
                    uint32_t liveRms=0;
                    if(!readMic(liveCount,livePeak,liveRms)||!liveCount)continue;
                    if(!sendPCM(pcmBuf,liveCount)){
                        Serial.println("TARS: STT PCM SEND FAILED");
                        sttError=true;
                        break;
                    }
                    samples+=liveCount;
                    if(livePeak>=MIC_SILENCE||liveRms>=1800)lastVoice=millis();
                    if(millis()-voiceStart>=RECORD_MIN_MS&&millis()-lastVoice>=SILENCE_MS)break;
                    yield();
                }
                break;
            }
        }
        yield();
    }

    if(!voice||sttError){
        if(sttError)closeSTT(STT_ERROR_COOLDOWN);
        else{
            sttRetryAt=millis()+STT_NORMAL_COOLDOWN;
            Serial.println("TARS: MIC AUDIO TOO LOW");
        }
        return "";
    }
    return stopSTT(samples);
}

String recordRealtime(){return recordSTT();}
