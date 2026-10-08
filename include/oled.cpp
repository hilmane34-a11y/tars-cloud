#include "oled.h"
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "config.h"
#include <sleep_oled.h>

extern bool cameraLive,playing,sleepPreparing;
extern bool visionLiveEnabled();
extern SemaphoreHandle_t previewMux;
extern uint8_t cameraPreview[128*64];
extern bool previewReady;

Adafruit_SSD1306 oled(OLED_WIDTH,OLED_HEIGHT,&Wire,-1);
bool oledOK=false;
volatile uint8_t oledSpecial=0;
volatile uint32_t oledDeadUntil=0,oledDoorStart=0;
String oledText,oledStatus="READY";
uint32_t oledTypePos=0,oledLastType=0,oledLastWave=0,oledPage=0,oledLastPage=0;

void oledSetStatus(const String&s){
oledStatus=s;oledText="";oledTypePos=0;oledPage=0;oledLastPage=millis();
}
void oledSetListening(){oledSetStatus("LISTENING");}
void oledStartSpeak(const String&s){
oledStatus="SPEAKING";oledText=s;oledTypePos=0;oledPage=0;
oledLastType=millis();oledLastPage=millis();
}
void oledShowText(const String&s,const String&status){
oledStatus=status;oledText=s;oledTypePos=s.length();oledPage=0;
oledLastType=millis();oledLastPage=millis();
}

void drawSpecialOLED(uint8_t m){
oled.clearDisplay();
oled.setTextColor(SSD1306_WHITE);
oled.drawLine(15,55,8,37,1);oled.drawLine(8,37,8,22,1);oled.drawLine(8,22,4,17,1);oled.drawLine(8,22,8,14,1);oled.drawLine(8,22,12,15,1);
oled.drawLine(113,55,120,37,1);oled.drawLine(120,37,120,22,1);oled.drawLine(120,22,124,17,1);oled.drawLine(120,22,120,14,1);oled.drawLine(120,22,116,15,1);

if(m==1){
oled.fillCircle(42,25,8,1);oled.fillCircle(86,25,8,1);oled.drawLine(45,44,83,44,1);
}else{
oled.drawLine(34,18,49,32,1);oled.drawLine(49,18,34,32,1);
oled.drawLine(79,18,94,32,1);oled.drawLine(94,18,79,32,1);
oled.drawCircle(64,45,7,1);oled.fillRect(61,49,6,4,0);
oled.drawLine(64,52,64,57,1);oled.drawLine(64,57,69,57,1);
uint32_t e=millis()-oledDoorStart;
int bx=5+(int)((e/35U>48U)?48U:e/35U);
oled.drawLine(bx-10,27,bx-2,27,1);oled.drawLine(bx-8,30,bx-2,30,1);oled.fillCircle(bx,27,3,1);
if(e>1700){oled.drawLine(53,23,58,28,1);oled.drawLine(58,23,53,28,1);}
}
oled.display();
}

void drawCameraOLED(){
  if(!oledOK||!previewMux)return;
  if(xSemaphoreTake(previewMux,pdMS_TO_TICKS(100))!=pdTRUE)
    return;
  if(!previewReady){
    xSemaphoreGive(previewMux);
    return;
  }
  oled.clearDisplay();
  for(int y=0;y<64;y++){
    for(int x=0;x<128;x++){
      if(cameraPreview[y*128+x])
        oled.drawPixel(x,y,SSD1306_WHITE);
    }
  }
  oled.display();
  xSemaphoreGive(previewMux);
}

void oledTask(void*){
for(;;){
if(!oledOK){vTaskDelay(pdMS_TO_TICKS(50));continue;}
uint32_t now=millis();

if(oledSpecial){
if(oledSpecial==2&&now>=oledDeadUntil){
oledSpecial=0;
oledSetStatus(visionLiveEnabled()?"LISTENING":"READY");
}else{
drawSpecialOLED(oledSpecial);
vTaskDelay(pdMS_TO_TICKS(20));
continue;
}
}

if(sleepPreparing){
sleepOLEDUpdate(oled);
vTaskDelay(pdMS_TO_TICKS(20));
continue;
}

if(cameraLive&&!playing){
drawCameraOLED();
vTaskDelay(pdMS_TO_TICKS(200));
continue;
}

if(oledText.length()&&oledTypePos<oledText.length()&&now-oledLastType>=OLED_TYPE_MS){
oledTypePos++;oledLastType=now;
}

if(now-oledLastWave>=OLED_WAVE_MS){
oledLastWave=now;
oled.clearDisplay();
oled.setTextColor(1);
oled.setTextSize(2);
oled.setCursor(36,0);
oled.print("TARS");
oled.setTextSize(1);
oled.setCursor(3,17);
oled.print(oledStatus);

if(oledText.length()){
String s=oledText.substring(0,min(oledTypePos,(uint32_t)oledText.length()));
uint32_t lineNo=0,target=oledPage*4;
uint8_t shown=0;
String line;
bool next=false;

for(size_t i=0;i<=s.length();i++){
char c=i<s.length()?s[i]:'\0';
if(c=='\n'||c=='\0'){
if(lineNo>=target&&shown<4){
oled.setCursor(3,29+shown*8);
oled.print(line);
shown++;
}
line="";
lineNo++;
if(shown>=4){next=i<s.length();break;}
continue;
}
line+=c;
if(line.length()>=20){
int cut=line.lastIndexOf(' ');
if(cut>0){
String rest=line.substring(cut+1);
line=line.substring(0,cut);
if(lineNo>=target&&shown<4){
oled.setCursor(3,29+shown*8);
oled.print(line);
shown++;
}
line=rest;
lineNo++;
if(shown>=4){next=i+1<s.length();break;}
}
}
if((i&63)==63)vTaskDelay(1);
}

if(oledStatus=="SPEAKING"&&now-oledLastPage>=OLED_PAGE_MS){
if(next)oledPage++;
oledLastPage=now;
}
}

if(oledStatus=="LISTENING"){
int x=64+(int)(sin(now/120.0)*25);
oled.drawCircle(x,56,4,1);
}else if(oledStatus=="SPEAKING"){
int w=8+(now/40)%18;
oled.fillRect(64-w/2,51,w,6,1);
}
oled.display();
}
vTaskDelay(pdMS_TO_TICKS(10));
}
}
