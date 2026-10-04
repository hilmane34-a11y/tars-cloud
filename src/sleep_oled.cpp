#include "sleep_oled.h"

static bool active=false;
static uint8_t frame=0;
static uint32_t frameAt=0;

void sleepOLEDStart(){
  active=true;
  frame=0;
  frameAt=0;
}

void sleepOLEDStop(){
  active=false;
}

void sleepOLEDUpdate(Adafruit_SSD1306 &oled){
  if(!active)return;

  uint32_t now=millis();

  if(now-frameAt<180 && frameAt!=0)return;

  frameAt=now;
  frame=(frame+1)%8;

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  // Wajah tidur
  oled.setTextSize(3);
  oled.setCursor(29,23);
  oled.print("-_-");

  // Z bergerak naik
  int x=88-(frame*3);
  int y=34-(frame*4);

  oled.setTextSize(frame>=4?2:1);
  oled.setCursor(x,y);
  oled.print("Z");

  // Z kecil menyusul
  if(frame>=3){
    oled.setTextSize(1);
    oled.setCursor(x+12,y-10);
    oled.print("z");
  }

  oled.display();
}
