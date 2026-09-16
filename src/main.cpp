#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <time.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebSocketsClient.h>
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "config.h"
#include "wifi_manager.h"

#define MIC_PORT I2S_NUM_1
#define MIC_SCK 18
#define MIC_WS 19
#define MIC_SD 34
#define AUDIO_DAC_PIN 26

const uint32_t MIC_RATE=16000,RECORD_MIN_MS=500,SILENCE_MS=1000;
const uint32_t PREROLL_MS=700,OLED_TYPE_MS=39,OLED_WAVE_MS=70;
const uint32_t AUDIO_IDLE_MS=2500;
const int32_t MIC_THRESHOLD=8000,MIC_SILENCE=6000;
const size_t BUF=2048;
const size_t PREROLL_SAMPLES=MIC_RATE*PREROLL_MS/1000;
const char* STT_HOST="tars-cloud-v1.hilmane34.workers.dev";

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);
AnalogAudioStream analog;
MP3DecoderHelix codec;
EncodedAudioStream dec(&analog,&codec);
StreamCopy copier;
WebSocketsClient sttWS;

bool oledOK=false,micOK=false,dacOK=false;
bool playing=false,ntpOK=false;
bool sttConnected=false,sttReady=false,sttDone=false,sttError=false;

String sttFinal,sttPartial,oledText,oledStatus="READY";
uint32_t oledTypePos=0,oledLastType=0,oledLastWave=0;

static int32_t rawBuf[BUF/4];
static int16_t pcmBuf[BUF/4];
static int16_t preBuf[PREROLL_SAMPLES];
static int16_t sendBuf[256];

/* OLED */
void oledHeader(){
  if(!oledOK)return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(2);
  oled.setCursor(36,0);
  oled.print("TARS");
  oled.display();
}

void oledSetStatus(const String&s){
  oledStatus=s;
  oledText="";
  oledTypePos=0;
}

void oledSetListening(){
  oledStatus="LISTENING";
  oledText="";
  oledTypePos=0;
}

void oledStartSpeak(const String&s){
  oledStatus="SPEAKING";
  oledText=s;
  oledTypePos=0;
  oledLastType=millis();
}

void oledTask(void*){
  for(;;){
    if(!oledOK){
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    uint32_t now=millis();

    if(oledText.length()&&oledTypePos<oledText.length()&&now-oledLastType>=OLED_TYPE_MS){
      oledTypePos++;
      oledLastType=now;
    }

    if(now-oledLastWave>=OLED_WAVE_MS){
      oledLastWave=now;
      oled.clearDisplay();
      oled.setTextColor(SSD1306_WHITE);
      oled.setTextSize(1);
      oled.setCursor(3,0);
      oled.print("T A R S");
      oled.setCursor(3,13);
      oled.print(oledStatus);

      if(oledText.length()){
        String src=oledText.substring(0,min((size_t)oledTypePos,oledText.length()));
        String lines[20],line;
        int n=0;

        for(size_t i=0;i<src.length();i++){
          char c=src[i];

          if(c=='\n'){
            if(n<20)lines[n++]=line;
            line="";
            continue;
          }

          line+=c;

          if(line.length()>=20){
            int cut=line.lastIndexOf(' ');

            if(cut>0){
              String rest=line.substring(cut+1);
              line=line.substring(0,cut);
              if(n<20)lines[n++]=line;
              line=rest;
            }else{
              if(n<20)lines[n++]=line;
              line="";
            }
          }
        }

        if(line.length()&&n<20)lines[n++]=line;

        int first=n?(n-1)/4*4:0;

        for(int i=0;i<4&&first+i<n;i++){
          oled.setCursor(3,27+i*8);
          oled.print(lines[first+i]);
        }
      }

      if(oledStatus=="LISTENING"){
        int x=64+(int)(sin(now/120.0)*25);
        oled.drawCircle(x,52,5,SSD1306_WHITE);
      }else if(oledStatus=="SPEAKING"){
        int w=8+(now/40)%18;
        oled.fillRect(60-w/2,49,w,7,SSD1306_WHITE);
      }

      oled.display();
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

/* DAC */
bool initDAC(){
  auto cfg=analog.defaultConfig(TX_MODE);
  cfg.channels=2;

  if(!analog.begin(cfg)){
    Serial.println("TARS: DAC ERROR");
    return false;
  }

  Serial.println("TARS: PAM RIGHT GPIO26 READY");
  return true;
}

/* INMP441 */
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

  if(i2s_driver_install(MIC_PORT,&c,0,nullptr)!=ESP_OK)
    return false;

  i2s_pin_config_t p={};
  p.bck_io_num=MIC_SCK;
  p.ws_io_num=MIC_WS;
  p.data_out_num=I2S_PIN_NO_CHANGE;
  p.data_in_num=MIC_SD;

  if(i2s_set_pin(MIC_PORT,&p)!=ESP_OK)
    return false;

  i2s_zero_dma_buffer(MIC_PORT);
  Serial.println("TARS: INMP441 RIGHT READY");
  return true;
}

/* NTP */
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

        Serial.printf(
          "TARS: NTP VALID %04d-%02d-%02d %02d:%02d:%02d\n",
          t.tm_year+1900,t.tm_mon+1,t.tm_mday,
          t.tm_hour,t.tm_min,t.tm_sec
        );

        ntpOK=true;
        return true;
      }

      delay(500);
    }
  }

  Serial.println("TARS: NTP FAILED 4/4");
  return false;
}

/* WIFI */
bool wifiOK(){
  if(WiFi.status()==WL_CONNECTED)return true;
  return wifiManagerConnect(false)&&WiFi.status()==WL_CONNECTED;
}

bool bootWiFi(){
  Serial.println("TARS: WIFI CONNECTING...");
  uint32_t start=millis();

  while(WiFi.status()!=WL_CONNECTED&&millis()-start<30000){
    wifiManagerConnect(false);
    delay(100);
    yield();
  }

  if(WiFi.status()!=WL_CONNECTED){
    Serial.println("TARS: WIFI BOOT FAILED");
    return false;
  }

  Serial.print("TARS: WIFI CONNECTED IP=");
  Serial.println(WiFi.localIP());
  return true;
}

/* STT */
void sttEvent(WStype_t type,uint8_t*payload,size_t length){
  if(type==WStype_CONNECTED){
    sttConnected=true;
    Serial.println("TARS: STT WS CONNECTED");
    return;
  }

  if(type==WStype_DISCONNECTED){
    sttConnected=false;
    if(!sttDone)sttError=true;
    Serial.println("TARS: STT WS DISCONNECTED");
    return;
  }

  if(type==WStype_ERROR){
    sttError=true;
    Serial.println("TARS: STT WS ERROR");
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
    Serial.println("TARS: STT REALTIME READY");
  }else if(t=="partial"){
    sttPartial=j["text"].as<String>();
    sttPartial.trim();

    if(sttPartial.length()){
      Serial.print("TARS: STT PARTIAL = ");
      Serial.println(sttPartial);
    }
  }else if(t=="final"){
    sttFinal=j["text"].as<String>();
    sttFinal.trim();
    sttDone=true;

    Serial.print("TARS: YOU SAID = ");
    Serial.println(sttFinal);
  }else if(t=="error"){
    sttError=true;
    sttDone=true;

    Serial.print("TARS: STT ERROR = ");
    Serial.println(j["error"].as<String>());
  }
}

bool startSTT(){
  if(!wifiOK())return false;

  sttConnected=false;
  sttReady=false;
  sttDone=false;
  sttError=false;
  sttFinal="";
  sttPartial="";

  sttWS.disconnect();
  sttWS.onEvent(sttEvent);
  sttWS.setReconnectInterval(0);
  sttWS.enableHeartbeat(15000,5000,2);
  sttWS.beginSSL(STT_HOST,443,"/stt");

  uint32_t start=millis();

  while(!sttReady&&!sttError&&millis()-start<7000){
    sttWS.loop();
    delay(2);
    yield();
  }

  if(!sttReady){
    Serial.println("TARS: STT REALTIME TIMEOUT");
    sttWS.disconnect();
    return false;
  }

  return true;
}

String stopSTT(uint32_t samples){
  if(!sttConnected)return "";

  JsonDocument j;
  j["type"]="end";
  j["timestamp"]=(double)samples/MIC_RATE;

  String msg;
  serializeJson(j,msg);

  sttWS.sendTXT(msg);
  Serial.println("TARS: STT END SENT");

  uint32_t start=millis();

  while(!sttDone&&!sttError&&millis()-start<6000){
    sttWS.loop();
    delay(2);
    yield();
  }

  String result=sttFinal;
  sttWS.disconnect();

  return result;
}

String recordRealtime(){
  if(!micOK||!startSTT())return "";

  oledSetListening();

  size_t prePos=0,preCount=0;
  uint32_t voiceStart=0,lastVoice=0,samples=0;
  bool voice=false;

  Serial.println("TARS: REALTIME LISTENING");

  for(;;){
    sttWS.loop();

    if(sttError)break;

    size_t bytes=0;

    if(i2s_read(MIC_PORT,rawBuf,sizeof(rawBuf),&bytes,pdMS_TO_TICKS(30))!=ESP_OK)
      continue;

    size_t count=bytes/4;
    int32_t peak=0;

    for(size_t i=0;i<count;i++){
      int32_t v=constrain(rawBuf[i]>>16,-32768,32767);
      pcmBuf[i]=(int16_t)v;
      int32_t a=abs(v);
      if(a>peak)peak=a;
    }

    if(!voice){
      for(size_t i=0;i<count;i++){
        preBuf[prePos]=pcmBuf[i];
        prePos=(prePos+1)%PREROLL_SAMPLES;
        if(preCount<PREROLL_SAMPLES)preCount++;
      }

      if(peak>=MIC_THRESHOLD){
        voice=true;
        voiceStart=millis();
        lastVoice=voiceStart;

        size_t start=preCount==PREROLL_SAMPLES?prePos:0;
        size_t n=0;

        for(size_t i=0;i<preCount;i++){
          size_t k=(start+i)%PREROLL_SAMPLES;
          sendBuf[n++]=preBuf[k];

          if(n==256){
            if(!sttWS.sendBIN((uint8_t*)sendBuf,n*2)){
              sttError=true;
              break;
            }
            n=0;
          }
        }

        if(n&&!sttError)sttWS.sendBIN((uint8_t*)sendBuf,n*2);

        samples+=preCount;

        Serial.printf("TARS: VOICE DETECTED PEAK=%ld\n",(long)peak);
      }
    }else{
      if(!sttWS.sendBIN((uint8_t*)pcmBuf,count*2)){
        Serial.println("TARS: STT PCM SEND FAILED");
        sttError=true;
        break;
      }

      samples+=count;

      if(peak>=MIC_SILENCE)lastVoice=millis();

      if(millis()-voiceStart>=RECORD_MIN_MS&&millis()-lastVoice>=SILENCE_MS)
        break;
    }

    yield();
  }

  if(!voice){
    sttWS.disconnect();
    Serial.println("TARS: MIC AUDIO TOO LOW");
    return "";
  }

  if(sttError){
    sttWS.disconnect();
    return "";
  }

  return stopSTT(samples);
}

/* ASK */
String ask(const String&q){
  if(!wifiOK())return "";

  WiFiClientSecure c;
  c.setInsecure();

  HTTPClient h;

  if(!h.begin(c,String(TARS_CLOUD_URL)+"/ask"))
    return "";

  h.setTimeout(12000);
  h.addHeader("Content-Type","application/json");

  JsonDocument j;
  j["question"]=q;

  String body;
  serializeJson(j,body);

  uint32_t t=millis();
  int code=h.POST(body);

  Serial.printf(
    "TARS: ASK HTTP=%d TIME=%lu ms\n",
    code,(unsigned long)(millis()-t)
  );

  if(code<200||code>=300){
    h.end();
    return "";
  }

  String r=h.getString();
  h.end();

  JsonDocument x;

  if(deserializeJson(x,r))
    return "";

  String s=x["response"].as<String>();
  s.trim();

  return s;
}

/* TTS MELOTTS MP3 */
bool streamAudio(const String&text){
  if(!wifiOK())return false;

  WiFiClientSecure c;
  c.setInsecure();
  c.setTimeout(15000);

  HTTPClient h;

  uint32_t totalStart=millis();

  if(!h.begin(c,String(TARS_CLOUD_URL)+"/tts"))
    return false;

  h.setTimeout(60000);
  h.addHeader("Content-Type","application/json");

  JsonDocument j;
  j["text"]=text;

  String body;
  serializeJson(j,body);

  uint32_t t=millis();
  int code=h.POST(body);

  Serial.printf(
    "TARS: AUDIO HTTP=%d TIME=%lu ms\n",
    code,(unsigned long)(millis()-t)
  );

  if(code<200||code>=300){
    String err=h.getString();
    Serial.print("TARS: TTS HTTP ERROR=");
    Serial.println(err);
    h.end();
    return false;
  }

  WiFiClient*stream=h.getStreamPtr();

  if(!stream){
    Serial.println("TARS: AUDIO STREAM NULL");
    h.end();
    return false;
  }

  if(!dacOK)
    dacOK=initDAC();

  if(!dacOK){
    h.end();
    return false;
  }

  /*
   * LIHAT HEADER AUDIO TANPA MENGAMBIL BYTE DARI STREAM.
   * peek() hanya melihat byte pertama.
   */
  int b0=stream->peek();

  Serial.printf(
    "TARS: AUDIO FIRST BYTE=0x%02X\n",
    b0<0?0:b0
  );

  if(b0>=0){
    uint8_t head[4]={0};

    for(int i=0;i<4;i++){
      int v=stream->peek();
      if(v<0)break;
      head[i]=(uint8_t)v;

      /*
       * Ambil satu byte hanya untuk diagnostic,
       * lalu decoder akan menerima stream mulai byte berikutnya.
       * Karena itu kita gunakan buffer kecil di bawah untuk
       * mengembalikan byte header ke decoder.
       */
    }

    Serial.printf(
      "TARS: AUDIO HEADER=%02X %02X %02X %02X\n",
      head[0],head[1],head[2],head[3]
    );
  }

  /*
   * IMPORTANT:
   * Jangan konsumsi stream sebelum decoder.
   */
  Serial.println("TARS: MP3 DECODER BEGIN...");

  bool decOK=dec.begin();

  Serial.printf(
    "TARS: MP3 DECODER=%s\n",
    decOK?"OK":"FAILED"
  );

  if(!decOK){
    Serial.println("TARS: MP3 DECODER INIT FAILED");
    h.end();
    return false;
  }

  bool copyOK=copier.begin(dec,*stream);

  Serial.printf(
    "TARS: MP3 COPIER=%s\n",
    copyOK?"OK":"FAILED"
  );

  if(!copyOK){
    Serial.println("TARS: MP3 COPIER INIT FAILED");
    dec.end();
    h.end();
    return false;
  }

  playing=true;

  bool started=false;
  uint32_t start=millis();
  uint32_t lastData=start;
  uint32_t totalBytes=0;

  Serial.println("TARS: AUDIO STREAM START");

  while(true){
    size_t before=stream->available();

    bool copied=copier.copy();

    size_t after=stream->available();

    if(copied){
      lastData=millis();

      /*
       * copier.copy() berhasil berarti decoder benar-benar
       * memproses data, bukan sekadar HTTP menerima data.
       */
      if(!started){
        started=true;

        Serial.printf(
          "TARS: MP3 DECODE START=%lu ms\n",
          (unsigned long)(millis()-start)
        );

        oledStartSpeak(text);
      }
    }

    if(before>after)
      totalBytes+=before-after;

    if(
      started&&
      !stream->available()&&
      millis()-lastData>=AUDIO_IDLE_MS
    )
      break;

    if(
      !started&&
      !h.connected()&&
      !stream->available()
    )
      break;

    if(millis()-start>70000){
      Serial.println("TARS: AUDIO TIMEOUT");
      break;
    }

    yield();
  }

  Serial.printf(
    "TARS: AUDIO STREAM=%lu ms\n",
    (unsigned long)(millis()-start)
  );

  Serial.printf(
    "TARS: AUDIO CONSUMED APPROX=%lu BYTES\n",
    (unsigned long)totalBytes
  );

  dec.end();
  h.end();

  playing=false;

  Serial.printf(
    "TARS: AUDIO TOTAL=%lu ms\n",
    (unsigned long)(millis()-totalStart)
  );

  oledSetListening();

  if(!started)
    Serial.println("TARS: MP3 DATA RECEIVED BUT DECODER NEVER STARTED");

  return started;
}

/* PROCESS */
void processQuestion(const String&q){
  String answer=ask(q);

  if(!answer.length()){
    oledSetStatus("ASK ERROR");
    return;
  }

  uint32_t t=millis();

  bool ok=streamAudio(answer);

  Serial.printf(
    "TARS: AUDIO FUNCTION TIME=%lu ms\n",
    (unsigned long)(millis()-t)
  );

  oledSetStatus(ok?"LISTENING":"AUDIO ERROR");
}

/* SETUP */
void setup(){
  Serial.begin(SERIAL_BAUD);

  Wire.begin(OLED_SDA,OLED_SCL);
  Wire.setClock(400000);

  oledOK=oled.begin(
    SSD1306_SWITCHCAPVCC,
    OLED_ADDR
  );

  if(oledOK){
    oledHeader();
    oled.setCursor(3,27);
    oled.print("BOOT");
    oled.display();
  }

  dacOK=initDAC();
  micOK=initMic();

  Serial.printf(
    "TARS: DAC=%s MIC=%s\n",
    dacOK?"READY":"ERROR",
    micOK?"READY":"ERROR"
  );

  Serial.println("TARS: PAM RIGHT GPIO26");
  Serial.println("TARS: INMP441 RIGHT GPIO34");
  Serial.println("TARS: MIC THRESHOLD=8000");
  Serial.println("TARS: MIC SILENCE=6000");
  Serial.println("TARS: PREROLL=700 ms");
  Serial.println("TARS: STT REALTIME PCM");
  Serial.println("TARS: BLUETOOTH DISABLED");
  Serial.println("TARS: MP3 STREAMING ENABLED");
  Serial.println("TARS: TTS = MELOTTS");

  if(oledOK){
    xTaskCreatePinnedToCore(
      oledTask,
      "TARS_OLED",
      4096,
      nullptr,
      1,
      nullptr,
      0
    );
  }

  wifiManagerBegin();

  if(bootWiFi())
    syncTime();

  oledSetListening();
}

/* LOOP */
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

  String q=recordRealtime();

  if(q.length())
    processQuestion(q);
  else
    oledSetListening();

  delay(1);
}
