#include "stt.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <math.h>
#include "config.h"
#include "tars_emotion.h"

#define MIC_PORT I2S_NUM_1
#define MIC_SCK 18
#define MIC_WS 2
#define MIC_SD 15
#define MIC_RATE 16000
#define MIC_THRESHOLD 14000
#define MIC_SILENCE 8000
#define RECORD_MIN_MS 500
#define SILENCE_MS 800
#define PREROLL_MS 250
#define STT_NORMAL_COOLDOWN 1000
#define STT_ERROR_COOLDOWN 30000
#define STT_QUOTA_COOLDOWN 60000
#define STT_RECONNECT_GUARD 60000
#define STT_IDLE_TIMEOUT_MS 10000

static const size_t BUF=256,PREROLL_SAMPLES=MIC_RATE*PREROLL_MS/1000;
static int32_t rawBuf[BUF/4];
static int16_t pcmBuf[BUF/4],preBuf[PREROLL_SAMPLES],sendBuf[256];
static WebSocketsClient sttWS;
static uint32_t sttRetryAt=0;
static bool sttRetryShown=false;

String sttFinal,sttPartial;
bool micOK=false;
bool sttConnected=false,sttReady=false,sttDone=false,sttError=false,sttClosing=false;

static const char* STT_HOST_LOCAL="tars-cloud-v1.hilmane34.workers.dev";

extern bool wifiOK();
extern void oledSetStatus(const String&);
extern void oledSetListening();

bool sttCooling(){return millis()<sttRetryAt;}

static void sttEvent(WStype_t type,uint8_t*payload,size_t length){
if(type==WStype_CONNECTED){
sttConnected=true;
sttError=false;
Serial.println("TARS: STT WS CONNECTED");
oledSetStatus("STT CONNECTED");
return;
}

if(type==WStype_DISCONNECTED){
sttConnected=false;
if(!sttClosing&&!sttDone)sttError=true;
Serial.println(sttClosing?"TARS: STT WS DISCONNECTED (NORMAL)":"TARS: STT WS DISCONNECTED");
return;
}

if(type==WStype_ERROR){
if(!sttClosing)sttError=true;
Serial.println("TARS: STT WS ERROR");
oledSetStatus("STT ERROR");
return;
}

if(type!=WStype_TEXT)return;

String msg;
msg.reserve(length+1);
for(size_t i=0;i<length;i++)msg+=(char)payload[i];

JsonDocument j;
if(deserializeJson(j,msg))return;

String t=j["type"].as<String>();

if(t=="ready"){
sttReady=true;
sttError=false;
sttRetryShown=false;
Serial.println("TARS: STT REALTIME READY");
oledSetStatus("STT READY");
}else if(t=="partial"){
sttPartial=j["text"].as<String>();
sttPartial.trim();
if(sttPartial.length())
Serial.println("TARS: STT PARTIAL = "+sttPartial);
}else if(t=="final"){
sttFinal=j["text"].as<String>();
sttFinal.trim();
sttDone=true;
Serial.println("TARS: YOU SAID = "+sttFinal);
}else if(t=="error"){
sttError=true;
sttDone=true;
String e=j["error"].as<String>();
Serial.println("TARS: STT ERROR = "+e);
oledSetStatus("STT ERROR");
}
}

void closeSTT(uint32_t cooldown){
sttClosing=true;
sttWS.disconnect();
sttConnected=false;
sttReady=false;
sttRetryAt=millis()+cooldown;
sttRetryShown=false;
sttClosing=false;
}

bool startSTT(bool offline){
Serial.printf("TARS: STT START ENTER WIFI=%d MIC=%d RAM=%u MAX=%u\n",WiFi.status(),micOK?1:0,(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getMaxAllocHeap());

if(!wifiOK()){
Serial.printf("TARS: STT BLOCK WIFI status=%d\n",WiFi.status());
return false;
}

if(!micOK){
Serial.println("TARS: STT BLOCK MIC=false");
return false;
}

if((int32_t)(millis()-sttRetryAt)<0){
if(!sttRetryShown){
Serial.printf("TARS: STT RETRY COOLDOWN %lu ms\n",(unsigned long)(sttRetryAt-millis()));
sttRetryShown=true;
}
return false;
}

Serial.printf("TARS: STT PREPARE WSS RAM=%u MAX=%u\n",(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getMaxAllocHeap());

sttClosing=true;
sttWS.disconnect();
sttConnected=false;
sttReady=false;
sttClosing=false;
sttDone=false;
sttError=false;
sttFinal="";
sttPartial="";
sttRetryShown=false;

sttWS.onEvent(sttEvent);
sttWS.setReconnectInterval(60000);
sttWS.enableHeartbeat(15000,5000,2);

Serial.printf("TARS: STT BEGIN SSL RAM=%u MAX=%u\n",(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getMaxAllocHeap());

sttWS.beginSSL(STT_HOST_LOCAL,443,"/stt");

Serial.printf("TARS: STT SSL STARTED RAM=%u MAX=%u\n",(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getMaxAllocHeap());

uint32_t st=millis();

while(!sttReady&&!sttError&&millis()-st<20000){
sttWS.loop();

if(((millis()-st)%1000)<5){
Serial.printf("TARS: STT WAIT %lus CONNECTED=%d READY=%d ERROR=%d RAM=%u MAX=%u\n",
(unsigned long)((millis()-st)/1000),
sttConnected?1:0,
sttReady?1:0,
sttError?1:0,
(unsigned)ESP.getFreeHeap(),
(unsigned)ESP.getMaxAllocHeap());
}

delay(2);
yield();
}

if(!sttReady){
Serial.printf("TARS: STT CONNECT TIMEOUT connected=%d error=%d RAM=%u MAX=%u\n",
sttConnected?1:0,
sttError?1:0,
(unsigned)ESP.getFreeHeap(),
(unsigned)ESP.getMaxAllocHeap());

closeSTT(STT_ERROR_COOLDOWN);
return false;
}

sttRetryAt=millis();
sttRetryShown=false;

Serial.println(offline?"TARS: OFFLINE STT READY":"TARS: ONLINE STT READY");
return true;
}
String stopSTT(uint32_t samples,bool offline){
if(!sttConnected&&!sttDone)return "";

JsonDocument j;
j["type"]="end";
j["timestamp"]=(double)samples/MIC_RATE;

String msg;
serializeJson(j,msg);

if(!sttWS.sendTXT(msg)){
Serial.println("TARS: STT END SEND FAILED");
closeSTT(STT_ERROR_COOLDOWN);
return "";
}

uint32_t st=millis();

while(!sttDone&&!sttError&&millis()-st<6000){
sttWS.loop();
delay(2);
yield();
}

String r=sttFinal;

closeSTT(STT_NORMAL_COOLDOWN);

return r;
}

String recordSTT(bool offline){
Serial.printf("TARS: RECORD STT START mode=%s\n",offline?"OFFLINE":"ONLINE");

if(!startSTT(offline)){
Serial.println("TARS: RECORD STT START FAILED");
return "";
}

if(offline)oledSetStatus("READY");
else oledSetListening();

size_t prePos=0,preCount=0;
uint32_t voiceStart=0,lastVoice=0,samples=0,listenStart=millis();
bool voice=false;

for(;;){
sttWS.loop();

if(sttError)break;

size_t bytes=0;

if(i2s_read(MIC_PORT,rawBuf,sizeof(rawBuf),&bytes,pdMS_TO_TICKS(30))!=ESP_OK)
continue;

size_t count=bytes/4;
int32_t peak=0;
uint64_t sum=0;

for(size_t i=0;i<count;i++){
int32_t v=constrain(rawBuf[i]>>16,-32768,32767);
pcmBuf[i]=(int16_t)v;

int32_t a=abs(v);

if(a>peak)peak=a;

sum+=(uint64_t)a*a;
}

uint32_t rms=count?(uint32_t)sqrt((double)sum/count):0;

if(!voice){
if(!offline&&millis()-listenStart>=STT_IDLE_TIMEOUT_MS){
Serial.println("TARS: STT IDLE TIMEOUT - CLOSE NORMAL");
closeSTT(STT_NORMAL_COOLDOWN);
return "";
}

for(size_t i=0;i<count;i++){
preBuf[prePos]=pcmBuf[i];
prePos=(prePos+1)%PREROLL_SAMPLES;

if(preCount<PREROLL_SAMPLES)
preCount++;
}

if(peak>=MIC_THRESHOLD||rms>=3000){
voice=true;
voiceStart=lastVoice=millis();

tarsEmotionSpeechPeak((uint16_t)min(peak,32767),true);

size_t start=preCount==PREROLL_SAMPLES?prePos:0;
size_t nsend=0;

for(size_t i=0;i<preCount;i++){
sendBuf[nsend++]=preBuf[(start+i)%PREROLL_SAMPLES];

if(nsend==256){
if(!sttWS.sendBIN((uint8_t*)sendBuf,nsend*2)){
sttError=true;
break;
}
nsend=0;
}
}

if(nsend&&!sttError&&!sttWS.sendBIN((uint8_t*)sendBuf,nsend*2))
sttError=true;

samples+=preCount;

Serial.printf(
"TARS: %s VOICE PEAK=%ld RMS=%lu\n",
offline?"OFFLINE":"ONLINE",
(long)peak,
(unsigned long)rms
);
}
}else{
if(!sttWS.sendBIN((uint8_t*)pcmBuf,count*2)){
Serial.println(
offline?
"TARS: OFFLINE STT PCM SEND FAILED":
"TARS: STT PCM SEND FAILED"
);

sttError=true;
break;
}

samples+=count;

if(peak>=MIC_SILENCE||rms>=1800)
lastVoice=millis();

if(
millis()-voiceStart>=RECORD_MIN_MS&&
millis()-lastVoice>=SILENCE_MS
)
break;
}

yield();
}

if(!voice||sttError){
closeSTT(
sttError?
STT_ERROR_COOLDOWN:
STT_NORMAL_COOLDOWN
);

if(!voice)
Serial.println(
offline?
"TARS: OFFLINE MIC AUDIO TOO LOW":
"TARS: MIC AUDIO TOO LOW"
);

return "";
}

return stopSTT(samples,offline);
}

String recordRealtime(){
return recordSTT(false);
}

String recordOffline(){
return recordSTT(true);
}
