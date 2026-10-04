#include "env.h"
#include <string.h>
#include <stdlib.h>

#define ENV_WIDTH 128
#define ENV_HEIGHT 64
#define GRID_W 16
#define GRID_H 8

#define DARK_PIXEL 1
#define MOTION_THRESHOLD 25
#define MIN_MOTION_CELLS 3
#define GLOBAL_CHANGE_CELLS 30

static EnvState state={};
static uint8_t previousGrid[GRID_W*GRID_H]={};
static bool previousValid=false;

void envBegin(){
  state={};
  memset(previousGrid,0,sizeof(previousGrid));
  previousValid=false;
}

void envResetMotion(){
  memset(previousGrid,0,sizeof(previousGrid));
  previousValid=false;
}

bool envAnalyze(
  const uint8_t*image,
  EnvState&result
){
  return envAnalyze(
    image,
    ENV_COLOR_UNKNOWN,
    0,
    result
  );
}

bool envAnalyze(
  const uint8_t*image,
  uint8_t dominantColor,
  uint8_t colorConfidence,
  EnvState&result
){
  result={};

  if(!image){
    state=result;
    previousValid=false;
    return false;
  }

  uint16_t dark[3]={},total[3]={};
  uint8_t currentGrid[GRID_W*GRID_H]={};
  uint8_t changedCells=0;
  uint8_t changedRegion[3]={};

  for(int y=8;y<60;y+=2){
    for(int x=0;x<ENV_WIDTH;x+=2){
      uint8_t region=x<42?0:(x<86?1:2);

      total[region]++;

      if(image[y*ENV_WIDTH+x]==DARK_PIXEL)
        dark[region]++;
    }
  }

  uint8_t darkRate[3]={};
  uint8_t clear[3]={};

  for(int i=0;i<3;i++){
    if(!total[i]){
      state=result;
      previousValid=false;
      return false;
    }

    darkRate[i]=(uint32_t)dark[i]*100/total[i];
    clear[i]=darkRate[i]<45;
  }

  // Grid 16x8 untuk mendeteksi perubahan gambar
  for(int gy=0;gy<GRID_H;gy++){
    for(int gx=0;gx<GRID_W;gx++){
      int x0=gx*ENV_WIDTH/GRID_W;
      int x1=(gx+1)*ENV_WIDTH/GRID_W;
      int y0=gy*ENV_HEIGHT/GRID_H;
      int y1=(gy+1)*ENV_HEIGHT/GRID_H;

      uint16_t darkCount=0;
      uint16_t pixelCount=0;

      for(int y=y0;y<y1;y+=2){
        for(int x=x0;x<x1;x+=2){
          if(image[y*ENV_WIDTH+x]==DARK_PIXEL)
            darkCount++;

          pixelCount++;
        }
      }

      currentGrid[gy*GRID_W+gx]=pixelCount
        ?(uint32_t)darkCount*100/pixelCount:0;
    }
  }

  // Deteksi perubahan antar-frame
  if(previousValid){
    for(int gy=0;gy<GRID_H;gy++){
      for(int gx=0;gx<GRID_W;gx++){
        int index=gy*GRID_W+gx;

        int difference=abs(
          (int)currentGrid[index]-
          (int)previousGrid[index]
        );

        if(difference>=MOTION_THRESHOLD){
          changedCells++;

          int centerX=gx*ENV_WIDTH/GRID_W+
                      ENV_WIDTH/GRID_W/2;

          uint8_t region=centerX<42?0:
                         (centerX<86?1:2);

          changedRegion[region]++;
        }
      }
    }
  }

  memcpy(previousGrid,currentGrid,sizeof(previousGrid));
  previousValid=true;

  result.valid=true;

  result.leftClear=clear[0];
  result.centerClear=clear[1];
  result.rightClear=clear[2];

  result.leftBright=100-darkRate[0];
  result.centerBright=100-darkRate[1];
  result.rightBright=100-darkRate[2];

  result.obstacle=!result.centerClear;

  uint8_t clearCount=
    clear[0]+clear[1]+clear[2];

  result.confidence=clearCount*100/3;

  result.motion=changedCells>=MIN_MOTION_CELLS;
  result.motionLevel=changedCells;

  if(changedCells>=GLOBAL_CHANGE_CELLS){
    result.event=ENV_SCENE_CHANGED;
  }else if(result.motion){
    if(changedRegion[0]>=changedRegion[1]&&
       changedRegion[0]>=changedRegion[2]){
      result.event=ENV_MOTION_LEFT;
    }else if(changedRegion[1]>=changedRegion[0]&&
             changedRegion[1]>=changedRegion[2]){
      result.event=ENV_MOTION_CENTER;
    }else{
      result.event=ENV_MOTION_RIGHT;
    }
  }else{
    result.event=ENV_NONE;
  }

  // Informasi warna dominan dari RGB565 kamera
  if(dominantColor<=ENV_COLOR_PURPLE){
    result.dominantColor=(EnvColor)dominantColor;
    result.colorConfidence=colorConfidence;
  }else{
    result.dominantColor=ENV_COLOR_UNKNOWN;
    result.colorConfidence=0;
  }

  state=result;
  return true;
}

EnvState envGet(){
  return state;
}
