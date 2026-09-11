// TARS Cloud + OLED + WiFi/NTP + TTS/SING + ESP32 DAC + Helix + WiFi Manager
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
#include <driver/i2s.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "config.h"
#include "wifi_manager.h"

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_ADDR 0x3C

#define DAC_I2S_PORT I2S_NUM_0
#define DAC_LEFT_GPIO 25
#define DAC_RIGHT_GPIO 26

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);

bool oledReadyFlag=false;

static String oledAnswer="";
static size_t oledTypedChars=0;
static uint32_t oledLastType=0;
static const uint32_t OLED_TYPE_INTERVAL=44;
static bool oledTyping=false;

volatile bool dacFirstAudioWrite=false;
volatile uint32_t dacFirstAudioMillis=0;

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
static const uint32_t PLAY_TIMEOUT_MS=120000;

static const uint32_t INPUT_SAMPLE_RATE=22050;
static const uint8_t INPUT_CHANNELS=1;
static const uint8_t OUTPUT_CHANNELS=2;
static const uint8_t BITS_PER_SAMPLE=16;

static const size_t PCM_OUTPUT_CHUNK=1024;
static const float PCM_GAIN=3.5f;

static bool dacReady=false;

bool ntpSynced=false;
bool singMode=false;
volatile uint8_t singLevel=0;
volatile bool playbackRunning=false;

void printHeap(const char *l){
  Serial.printf(
    "HEAP[%s]: free=%u largest=%u internal=%u\n",
    l,
    (unsigned)ESP.getFreeHeap(),
    (unsigned)ESP.getMaxAllocHeap(),
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
  );
}

void oledHeader(const char *s){
  if(!oledReadyFlag)return;

  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(45,0);
  oled.print("T A R S");

  oled.drawLine(0,9,27,9,SSD1306_WHITE);
  oled.drawLine(34,9,61,9,SSD1306_WHITE);
  oled.drawLine(67,9,94,9,SSD1306_WHITE);
  oled.drawLine(101,9,127,9,SSD1306_WHITE);

  oled.setCursor(3,13);
  oled.print("> ");
  oled.print(s);
}

void oledDrawMechanical(bool speaking){
  if(!oledReadyFlag)return;

  const int b=62;
  oled.drawLine(2,b,125,b,SSD1306_WHITE);

  if(!speaking){
    static const uint8_t p[24]={
      2,2,5,5,5,2,2,4,4,2,2,5,
      5,5,2,2,4,4,2,2,5,5,2,2
    };

    for(int i=0;i<24;i++){
      int x=3+i*5;
      if(x>123)break;

      uint8_t n=(i+oledMechanicalFrame)%24;
      int h=p[n];

      oled.drawLine(x,b-h,x+3,b-h,SSD1306_WHITE);

      if(i<23){
        int nx=x+5;
        if(nx<=125){
          oled.drawLine(
            x+3,b-h,
            nx,b-p[(n+1)%24],
            SSD1306_WHITE
          );
        }
      }
    }

    int x=45+(oledMechanicalFrame%17);
    oled.drawLine(x,57,x+3,57,SSD1306_WHITE);
    return;
  }

  for(int i=0;i<6;i++){
    int x=8+i*22;
    int h=3;

    if(i==(oledMechanicalFrame%6))
      h=8;
    else if(
      i==((oledMechanicalFrame+5)%6) ||
      i==((oledMechanicalFrame+1)%6)
    )
      h=5;

    oled.drawLine(x,b-h,x,b,SSD1306_WHITE);
    oled.drawLine(x-3,b-h,x,b,SSD1306_WHITE);
    oled.drawLine(x,b,x+3,b-h,SSD1306_WHITE);
  }

  int x=61+(((oledMechanicalFrame%3)-1)*3);
  oled.drawLine(x,56,x,59,SSD1306_WHITE);
}

void oledShowReady(){
  if(!oledReadyFlag)return;

  oled.clearDisplay();
  oledHeader("READY");

  oled.setCursor(3,27);
  oled.print("WAITING FOR");

  oled.setCursor(3,36);
  oled.print("COMMAND...");

  oledDrawMechanical(false);
  oled.display();

  oledLastAnim=millis();
}

void oledShowListening(){
  if(!oledReadyFlag)return;

  oled.clearDisplay();
  oledHeader("LISTENING");

  oled.setCursor(3,27);
  oled.print("INPUT RECEIVED");

  oled.setCursor(3,36);
  oled.print("AWAITING QUERY");

  oledDrawMechanical(false);
  oled.display();
}

void oledShowProcessing(){
  if(!oledReadyFlag)return;

  oled.clearDisplay();
  oledHeader("PROCESSING");

  oled.setCursor(3,27);
  oled.print("ANALYZING...");

  oled.setCursor(3,36);
  oled.print("GENERATING RESPONSE");

  oledDrawMechanical(false);
  oled.display();
}

void oledShowOnline(){
  if(!oledReadyFlag)return;

  oled.clearDisplay();
  oledHeader("ONLINE");

  oled.setCursor(3,27);
  oled.print("SYSTEM INITIALIZED");

  oled.setCursor(3,36);
  oled.print("DAC : READY");

  oled.setCursor(3,45);
  oled.print("VOICE: READY");

  oledDrawMechanical(false);
  oled.display();
}

void oledUpdateReadyAnimation(){
  if(!oledReadyFlag)return;

  uint32_t n=millis();

  if(n-oledLastAnim<OLED_ANIM_INTERVAL)return;

  oledLastAnim=n;
  oledMechanicalFrame++;

  oled.clearDisplay();
  oledHeader("READY");

  oled.setCursor(3,27);
  oled.print("WAITING FOR");

  oled.setCursor(3,36);
  oled.print("COMMAND...");

  oledDrawMechanical(false);
  oled.display();
}

void oledPrepareTyping(const String &t){
  if(!oledReadyFlag)return;

  oledAnswer=t;
  oledTypedChars=0;
  oledLastType=millis();
  oledMechanicalFrame=0;
  oledTyping=false;

  oledAudioSyncPending=true;
}

void oledStartTypingNow(){
  if(!oledReadyFlag)return;

  oledTypedChars=0;
  oledLastType=millis();
  oledMechanicalFrame=0;
  oledTyping=true;
  oledAudioSyncPending=false;

  Serial.println("TARS: OLED TYPING START - AUDIO SYNC");
}

void oledDrawTypedText(bool speaking){
  if(!oledReadyFlag)return;

  oled.clearDisplay();
  oledHeader(speaking?"SPEAKING":"READY");

  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);

  String v=oledAnswer.substring(0,oledTypedChars);

  const int sx=3;
  const int sy=23;
  const int mc=20;
  const int ml=4;

  int line=0;
  int col=0;

  oled.setCursor(sx,sy);
  oled.print("> ");
  col=2;

  for(size_t i=0;i<v.length();i++){
    char c=v[i];

    if(c=='\r')continue;

    if(c=='\n'){
      line++;
      col=0;

      if(line>=ml)break;

      oled.setCursor(sx,sy+line*9);
      continue;
    }

    if(col>=mc){
      line++;
      col=0;

      if(line>=ml)break;

      oled.setCursor(sx,sy+line*9);
    }

    oled.write(c);
    col++;
  }

  oledDrawMechanical(speaking);
  oled.display();
}

void oledDrawSing(){
  if(!oledReadyFlag)return;

  oled.clearDisplay();

  uint8_t level=singLevel;

  for(int i=0;i<6;i++){
    int variation=((int)i*13+(millis()/90)%17)%18-9;

    int h=4+(level*(70+variation)/255);

    if(h<4)h=4;
    if(h>55)h=55;

    int x=16+i*19;

    oled.fillRect(
      x,
      63-h,
      8,
      h,
      SSD1306_WHITE
    );
  }

  oled.display();
}

void oledUpdateTyping(bool speaking){
  if(!oledReadyFlag)return;

  if(singMode){
    oledDrawSing();
    return;
  }

  uint32_t n=millis();

  if(
    oledAudioSyncPending &&
    dacFirstAudioWrite &&
    n-dacFirstAudioMillis>=OLED_AUDIO_SYNC_DELAY_MS
  ){
    oledStartTypingNow();
    oledDrawTypedText(true);
  }

  bool redraw=false;

  if(
    oledTyping &&
    n-oledLastType>=OLED_TYPE_INTERVAL
  ){
    oledLastType=n;

    if(oledTypedChars<oledAnswer.length())
      oledTypedChars++;
    else
      oledTyping=false;

    redraw=true;
  }

  if(
    speaking &&
    n-oledLastAnim>=OLED_ANIM_INTERVAL
  ){
    oledLastAnim=n;
    oledMechanicalFrame++;
    redraw=true;
  }

  if(redraw)
    oledDrawTypedText(speaking);
}

bool initDAC(){
  Serial.println("TARS: DAC INIT");

  i2s_config_t cfg;

  memset(&cfg,0,sizeof(cfg));

  cfg.mode=
    (i2s_mode_t)(
      I2S_MODE_MASTER |
      I2S_MODE_TX |
      I2S_MODE_DAC_BUILT_IN
    );

  cfg.sample_rate=INPUT_SAMPLE_RATE;
  cfg.bits_per_sample=I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format=I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format=I2S_COMM_FORMAT_I2S_MSB;

  cfg.intr_alloc_flags=0;
  cfg.dma_buf_count=4;
  cfg.dma_buf_len=256;
  cfg.use_apll=false;
  cfg.tx_desc_auto_clear=true;
  cfg.fixed_mclk=0;

  esp_err_t err=i2s_driver_install(
    DAC_I2S_PORT,
    &cfg,
    0,
    nullptr
  );

  if(err!=ESP_OK){
    Serial.printf(
      "TARS: DAC DRIVER FAILED: %d\n",
      (int)err
    );
    return false;
  }

  err=i2s_set_dac_mode(
    I2S_DAC_CHANNEL_BOTH_EN
  );

  if(err!=ESP_OK){
    Serial.printf(
      "TARS: DAC MODE FAILED: %d\n",
      (int)err
    );

    i2s_driver_uninstall(DAC_I2S_PORT);
    return false;
  }

  i2s_zero_dma_buffer(DAC_I2S_PORT);

  dacReady=true;

  Serial.println("TARS: DAC GPIO25 = LEFT");
  Serial.println("TARS: DAC GPIO26 = RIGHT");
  Serial.println("TARS: DAC READY");

  return true;
}

void stopDAC(){
  if(!dacReady)return;

  i2s_zero_dma_buffer(DAC_I2S_PORT);
  i2s_driver_uninstall(DAC_I2S_PORT);

  dacReady=false;

  Serial.println("TARS: DAC STOP");
}

class PCMOutputStream:public AudioStream{
  AudioInfo currentInfo;

  uint16_t outputBuffer[PCM_OUTPUT_CHUNK/2];

  static uint8_t pcmToDAC(int16_t sample){
    int32_t v=(int32_t)((float)sample*PCM_GAIN);

    if(v>32767)v=32767;
    if(v<-32768)v=-32768;

    int32_t u=(v>>8)+128;

    if(u<0)u=0;
    if(u>255)u=255;

    return(uint8_t)u;
  }

public:

  void setAudioInfo(AudioInfo i)override{
    currentInfo=i;

    AudioStream::setAudioInfo(i);

    Serial.printf(
      "PCM format: %d Hz, %d ch, %d bit\n",
      i.sample_rate,
      i.channels,
      i.bits_per_sample
    );

    if(dacReady){
      i2s_set_clk(
        DAC_I2S_PORT,
        i.sample_rate,
        I2S_BITS_PER_SAMPLE_16BIT,
        I2S_CHANNEL_STEREO
      );
    }
  }

  int availableForWrite()override{
    return PCM_OUTPUT_CHUNK;
  }

  size_t write(
    const uint8_t *data,
    size_t size
  )override{

    if(
      !data ||
      !size ||
      !dacReady
    )
      return 0;

    if(
      currentInfo.sample_rate!=INPUT_SAMPLE_RATE ||
      currentInfo.bits_per_sample!=BITS_PER_SAMPLE ||
      (
        currentInfo.channels!=INPUT_CHANNELS &&
        currentInfo.channels!=OUTPUT_CHANNELS
      )
    ){
      Serial.printf(
        "PCM ERROR: unsupported %d Hz %d ch %d bit\n",
        currentInfo.sample_rate,
        currentInfo.channels,
        currentInfo.bits_per_sample
      );

      return 0;
    }

    size_t bytesPerSample=
      currentInfo.channels*2;

    size_t frames=
      size/bytesPerSample;

    size_t maxFrames=
      (PCM_OUTPUT_CHUNK/2)/2;

    if(frames>maxFrames)
      frames=maxFrames;

    if(!frames)
      return 0;

    int peak=0;

    for(size_t i=0;i<frames;i++){

      int16_t sample=0;

      if(currentInfo.channels==1){

        sample=(int16_t)(
          data[i*2] |
          ((uint16_t)data[i*2+1]<<8)
        );

      }else{

        int16_t left=(int16_t)(
          data[i*4] |
          ((uint16_t)data[i*4+1]<<8)
        );

        int16_t right=(int16_t)(
          data[i*4+2] |
          ((uint16_t)data[i*4+3]<<8)
        );

        sample=(int16_t)(
          ((int32_t)left+(int32_t)right)/2
        );
      }

      int a=abs((int)sample);

      if(a>peak)
        peak=a;

      uint8_t dac=pcmToDAC(sample);

      outputBuffer[i*2]=(uint16_t)dac<<8;
      outputBuffer[i*2+1]=(uint16_t)dac<<8;
    }

    singLevel=(uint8_t)min(255,peak>>7);

    size_t bytesToWrite=frames*4;
    size_t written=0;

    esp_err_t err=i2s_write(
      DAC_I2S_PORT,
      outputBuffer,
      bytesToWrite,
      &written,
      portMAX_DELAY
    );

    if(err!=ESP_OK){
      Serial.printf(
        "DAC ERROR: %d\n",
        (int)err
      );
      return 0;
    }

    if(
      written>0 &&
      !dacFirstAudioWrite
    ){
      dacFirstAudioWrite=true;
      dacFirstAudioMillis=millis();

      Serial.println(
        "TARS: DAC FIRST REAL AUDIO"
      );
    }

    return frames*bytesPerSample;
  }
};

PCMOutputStream pcmOutput;
MP3DecoderHelix mp3Decoder;
EncodedAudioStream mp3Stream(
  &pcmOutput,
  &mp3Decoder
);

bool isTimeValid(){
  return time(nullptr)>1577836800;
}

bool syncNTP(){
  Serial.println("TARS: NTP START");

  configTime(
    GMT_OFFSET_SEC,
    DAYLIGHT_OFFSET_SEC,
    NTP_SERVER_1,
    NTP_SERVER_2
  );

  uint32_t s=millis();
  int a=0;

  while(
    !isTimeValid() &&
    millis()-s<NTP_TIMEOUT_MS
  ){
    Serial.printf(
      "TARS: NTP attempt %d\n",
      ++a
    );

    delay(1000);
  }

  if(!isTimeValid()){
    Serial.println("TARS: NTP FAILED");
    ntpSynced=false;
    return false;
  }

  time_t now=time(nullptr);
  struct tm t;

  localtime_r(&now,&t);

  Serial.printf(
    "TARS: NTP OK %04d-%02d-%02d %02d:%02d:%02d\n",
    t.tm_year+1900,
    t.tm_mon+1,
    t.tm_mday,
    t.tm_hour,
    t.tm_min,
    t.tm_sec
  );

  ntpSynced=true;

  return true;
}

bool ensureTimeValid(){
  if(
    ntpSynced &&
    isTimeValid()
  )
    return true;

  if(isTimeValid()){
    ntpSynced=true;
    return true;
  }

  return syncNTP();
}

bool connectWiFi(bool requireTime=false){

  if(WiFi.status()==WL_CONNECTED){

    if(!requireTime || ntpSynced)
      return true;

    return ensureTimeValid();
  }

  Serial.println("TARS: WiFi ON");

  if(!wifiManagerConnect(false)){
    Serial.println("TARS: WIFI FAILED");
    return false;
  }

  if(
    requireTime &&
    !ensureTimeValid()
  ){
    Serial.println("TARS: NTP FAILED");
    return false;
  }

  Serial.print("TARS: IP = ");
  Serial.println(WiFi.localIP());

  return true;
}

void disconnectWiFi(){
  wifiManagerDisconnect();
}

String askAI(const String &question){

  if(
    !connectWiFi(true) ||
    !ensureTimeValid()
  )
    return "";

  oledShowProcessing();

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  if(!http.begin(client,ASK_URL))
    return "";

  http.addHeader(
    "Content-Type",
    "application/json"
  );

  StaticJsonDocument<512> req;

  req["text"]=question;

  String body;

  serializeJson(req,body);

  int code=http.POST(body);

  Serial.printf(
    "ASK HTTP: %d\n",
    code
  );

  if(code<200 || code>=300){
    http.end();
    return "";
  }

  String response=http.getString();

  http.end();

  StaticJsonDocument<1024> json;

  if(deserializeJson(json,response)){
    Serial.println("TARS: JSON ERROR");
    return "";
  }

  String answer=json["response"]|"";

  Serial.println("TARS RESPONSE:");
  Serial.println(answer);

  return answer;
}

bool downloadAudio(
  const char *url,
  const String &text
){

  if(
    !connectWiFi(true) ||
    !ensureTimeValid()
  )
    return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  if(!http.begin(client,url))
    return false;

  http.addHeader(
    "Content-Type",
    "application/json"
  );

  StaticJsonDocument<512> req;

  req["text"]=text;

  String body;

  serializeJson(req,body);

  int code=http.POST(body);

  Serial.printf(
    "AUDIO HTTP: %d\n",
    code
  );

  if(code<200 || code>=300){
    http.end();
    return false;
  }

  int len=http.getSize();

  if(LittleFS.exists(MP3_PATH))
    LittleFS.remove(MP3_PATH);

  File file=LittleFS.open(
    MP3_PATH,
    FILE_WRITE
  );

  if(!file){
    http.end();
    return false;
  }

  WiFiClient *stream=http.getStreamPtr();

  uint8_t buffer[1024];

  size_t total=0;

  uint32_t last=millis();

  while(
    http.connected() &&
    (len>0 || len==-1)
  ){

    size_t av=stream->available();

    if(av){

      size_t n=min(
        av,
        sizeof(buffer)
      );

      int r=stream->readBytes(
        buffer,
        n
      );

      if(r>0){

        file.write(
          buffer,
          r
        );

        total+=r;
        last=millis();

        if(len>0)
          len-=r;
      }

    }else{

      if(millis()-last>5000)
        break;

      delay(1);
    }
  }

  file.flush();
  file.close();

  http.end();

  Serial.printf(
    "TARS: MP3 bytes = %u\n",
    (unsigned)total
  );

  return total>0;
}

bool playMP3(){

  if(!LittleFS.exists(MP3_PATH)){
    Serial.println("TARS: MP3 NOT FOUND");
    return false;
  }

  File mp3File=LittleFS.open(
    MP3_PATH,
    FILE_READ
  );

  if(!mp3File)
    return false;

  size_t mp3Size=mp3File.size();

  if(!mp3Size){
    mp3File.close();
    LittleFS.remove(MP3_PATH);
    return false;
  }

  Serial.printf(
    "TARS: PLAY START%s\n",
    singMode?" [SING]":""
  );

  playbackRunning=true;
  singLevel=0;

  dacFirstAudioWrite=false;
  dacFirstAudioMillis=0;

  if(!dacReady){
    mp3File.close();
    playbackRunning=false;
    return false;
  }

  printHeap("BEFORE_HELIX");

  mp3Decoder.setMaxPCMSize(2048);
  mp3Decoder.setMaxFrameSize(1024);

  if(!mp3Stream.begin()){
    mp3File.close();
    playbackRunning=false;

    Serial.println(
      "TARS: MP3 DECODER FAILED"
    );

    return false;
  }

  StreamCopy copier(
    mp3Stream,
    mp3File,
    1024
  );

  copier.setCheckAvailableForWrite(false);
  copier.setCheckAvailable(true);

  uint32_t ps=millis();
  uint32_t lp=millis();

  bool finished=false;

  while(
    millis()-ps<PLAY_TIMEOUT_MS
  ){

    oledUpdateTyping(true);

    size_t pos=mp3File.position();

    if(millis()-lp>1000){

      Serial.printf(
        "AUDIO: MP3=%u/%u DAC=%d\n",
        (unsigned)pos,
        (unsigned)mp3Size,
        dacReady
      );

      lp=millis();
    }

    if(pos>=mp3Size){
      finished=true;
      Serial.println("TARS: MP3 EOF");
      break;
    }

    size_t copied=copier.copy();

    if(!copied)
      delay(2);
    else
      yield();
  }

  mp3Stream.end();

  mp3File.close();

  Serial.println(
    "TARS: DAC DRAIN"
  );

  uint32_t ts=millis();

  while(
    millis()-ts<500
  ){
    oledUpdateTyping(true);
    delay(10);
  }

  if(!finished){
    playbackRunning=false;

    if(LittleFS.exists(MP3_PATH))
      LittleFS.remove(MP3_PATH);

    Serial.println(
      "TARS: PLAYBACK TIMEOUT"
    );

    oledShowReady();

    return false;
  }

  singLevel=0;

  playbackRunning=false;

  if(LittleFS.exists(MP3_PATH)){
    LittleFS.remove(MP3_PATH);
    Serial.println(
      "TARS: MP3 FILE REMOVED"
    );
  }

  oledTyping=false;
  oledTypedChars=oledAnswer.length();
  singMode=false;

  while(oledTyping){
    oledUpdateTyping(false);
    delay(5);
  }

  oledShowReady();

  Serial.println(
    "TARS: PLAY DONE"
  );

  printHeap("PLAY_DONE");

  return true;
}

bool isSingRequest(String q){
  q.toLowerCase();

  return
    q.indexOf("nyanyi")>=0 ||
    q.indexOf("bernyanyi")>=0 ||
    q.indexOf("nyanyikan")>=0 ||
    q.indexOf("nyanyiin")>=0;
}

void handleQuestion(
  const String &question
){

  if(!question.length())
    return;

  Serial.println(
    "\nTARS: COMMAND RECEIVED"
  );

  Serial.println(question);

  oledShowListening();

  if(!connectWiFi(true)){
    oledShowReady();
    return;
  }

  String answer=askAI(question);

  if(!answer.length()){
    oledShowReady();
    return;
  }

  bool sing=isSingRequest(question);

  oledPrepareTyping(answer);

  if(!downloadAudio(
    sing?SING_URL:TTS_URL,
    answer
  )){
    Serial.println(
      "TARS: AUDIO DOWNLOAD FAILED"
    );

    oledShowReady();
    return;
  }

  singMode=sing;

  disconnectWiFi();

  playMP3();

  connectWiFi(false);
}

void setup(){

  Serial.begin(SERIAL_BAUD);

  delay(1000);

  Serial.println(
    "\n================================"
  );

  Serial.println(
    "        TARS ESP32 START"
  );

  Serial.println(
    "================================"
  );

  printHeap("BOOT");

  Wire.begin(21,22);

  if(
    oled.begin(
      SSD1306_SWITCHCAPVCC,
      OLED_ADDR
    )
  ){
    oledReadyFlag=true;
    oledShowOnline();
  }

  if(!LittleFS.begin(true)){
    Serial.println(
      "LittleFS FAILED"
    );
    return;
  }

  Serial.println(
    "LittleFS OK"
  );

  if(!initDAC()){
    Serial.println(
      "TARS: DAC INIT FAILED"
    );
  }

  Serial.println(
    "PCM  : HELIX MP3"
  );

  Serial.println(
    "DAC  : GPIO25 + GPIO26"
  );

  Serial.println(
    "OLED : AUDIO SYNC 50 MS"
  );

  Serial.println(
    "BT   : DISABLED / REMOVED"
  );

  printHeap("BEFORE_WIFI");

  Serial.println(
    "TARS: WIFI MANAGER START"
  );

  wifiManagerBegin();

  if(WiFi.status()!=WL_CONNECTED){

    if(!connectWiFi(true))
      Serial.println(
        "TARS: INITIAL WIFI FAILED"
      );

  }else if(!ntpSynced){

    ensureTimeValid();
  }

  printHeap("READY");

  oledShowReady();

  Serial.println(
    "================================"
  );

  Serial.println(
    "TARS: READY FOR QUESTION"
  );

  Serial.println(
    "================================"
  );
}

void loop(){

  oledUpdateReadyAnimation();

  if(Serial.available()){

    String q=
      Serial.readStringUntil('\n');

    q.trim();

    if(q.length())
      handleQuestion(q);
  }

  delay(10);
}
