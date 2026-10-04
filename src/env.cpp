#include "env.h"
#include <string.h>
#include <stdlib.h>

#define ENV_WIDTH 96
#define ENV_HEIGHT 32
#define GRID_W 16
#define GRID_H 8

#define MOTION_THRESHOLD 18
#define MIN_MOTION_CELLS 3
#define GLOBAL_CHANGE_CELLS 30

static EnvState state={};
static uint8_t previousGrid[GRID_W*GRID_H]={};
static bool previousValid=false;

struct ColorInfo{
  uint32_t count[11];
  uint32_t samples;
  uint32_t brightness;
};

struct SectorInfo{
  uint16_t edges;
  uint16_t texture;
  uint16_t samples;
  uint32_t contrast;
  uint32_t colorChanges;
};

static uint8_t getBrightness(uint16_t p){
  uint8_t r=((p>>11)&31)*255/31;
  uint8_t g=((p>>5)&63)*255/63;
  uint8_t b=(p&31)*255/31;
  return (77*r+150*g+29*b)>>8;
}

static EnvColor getColor(uint16_t p){
  uint8_t r=((p>>11)&31)*255/31;
  uint8_t g=((p>>5)&63)*255/63;
  uint8_t b=(p&31)*255/31;
  uint8_t mx=max(r,max(g,b));
  uint8_t mn=min(r,min(g,b));
  uint8_t d=mx-mn;

  if(mx<35)return ENV_COLOR_BLACK;
  if(mn>220&&d<35)return ENV_COLOR_WHITE;
  if(d<25)return ENV_COLOR_GRAY;

  float h;
  if(mx==r)h=60.0f*((float)g-b)/d;
  else if(mx==g)h=60.0f*((float)b-r)/d+120;
  else h=60.0f*((float)r-g)/d+240;
  if(h<0)h+=360;

  if(h<15||h>=345)return ENV_COLOR_RED;
  if(h<40)return ENV_COLOR_ORANGE;
  if(h<70)return ENV_COLOR_YELLOW;
  if(h<165)return ENV_COLOR_GREEN;
  if(h<200)return ENV_COLOR_CYAN;
  if(h<255)return ENV_COLOR_BLUE;
  return ENV_COLOR_PURPLE;
}

static void addPixel(ColorInfo &info,uint16_t p){
  EnvColor c=getColor(p);
  info.count[c]++;
  info.samples++;
  info.brightness+=getBrightness(p);
}

static EnvColor bestColor(
  const ColorInfo &info,uint8_t &confidence
){
  confidence=0;
  if(!info.samples)return ENV_COLOR_UNKNOWN;

  uint8_t best=ENV_COLOR_UNKNOWN;
  uint32_t count=0;

  for(uint8_t i=1;i<=ENV_COLOR_PURPLE;i++){
    if(info.count[i]>count){
      count=info.count[i];
      best=i;
    }
  }

  confidence=(uint32_t)count*100/info.samples;
  return (EnvColor)best;
}

void envBegin(){
  state={};
  memset(previousGrid,0,sizeof(previousGrid));
  previousValid=false;
}

void envResetMotion(){
  memset(previousGrid,0,sizeof(previousGrid));
  previousValid=false;
}

bool envAnalyze(const uint16_t *image,EnvState &result){
  return envAnalyze(image,ENV_COLOR_UNKNOWN,0,result);
}

bool envAnalyze(
  const uint16_t *image,
  uint8_t dominantColor,
  uint8_t colorConfidence,
  EnvState &result
){
  result={};
  if(!image){
    state=result;
    previousValid=false;
    return false;
  }

  ColorInfo sector[3]={};
  ColorInfo global={};
  SectorInfo detail[3]={};

  uint8_t currentGrid[GRID_W*GRID_H]={};
  uint8_t changedRegion[3]={};
  uint8_t changedCells=0;

  // Warna dan kecerahan
  for(int y=2;y<ENV_HEIGHT;y+=2){
    for(int x=0;x<ENV_WIDTH;x+=2){
      uint8_t r=x<32?0:(x<64?1:2);
      uint16_t p=image[y*ENV_WIDTH+x];
      addPixel(sector[r],p);
      addPixel(global,p);
    }
  }

  for(int i=0;i<3;i++){
    if(!sector[i].samples){
      state=result;
      previousValid=false;
      return false;
    }
  }

  result.leftBright=sector[0].brightness/sector[0].samples;
  result.centerBright=sector[1].brightness/sector[1].samples;
  result.rightBright=sector[2].brightness/sector[2].samples;

  result.leftColor=bestColor(sector[0],result.leftColorConfidence);
  result.centerColor=bestColor(sector[1],result.centerColorConfidence);
  result.rightColor=bestColor(sector[2],result.rightColorConfidence);

  // Tepi, kontras, tekstur dan perubahan warna
  for(int y=1;y<ENV_HEIGHT-2;y+=2){
    for(int x=1;x<ENV_WIDTH-2;x+=2){
      uint8_t r=x<32?0:(x<64?1:2);
      uint16_t p=image[y*ENV_WIDTH+x];

      int c=getBrightness(p);
      int right=getBrightness(image[y*ENV_WIDTH+x+2]);
      int down=getBrightness(image[(y+2)*ENV_WIDTH+x]);
      int diff=max(abs(c-right),abs(c-down));

      detail[r].samples++;
      detail[r].contrast+=diff;

      if(diff>=22)detail[r].edges++;
      if(diff>=10)detail[r].texture++;

      if(getColor(p)!=getColor(image[y*ENV_WIDTH+x+2]))
        detail[r].colorChanges++;
    }
  }

  // Motion grid
  for(int gy=0;gy<GRID_H;gy++){
    for(int gx=0;gx<GRID_W;gx++){
      int x0=gx*ENV_WIDTH/GRID_W;
      int x1=(gx+1)*ENV_WIDTH/GRID_W;
      int y0=gy*ENV_HEIGHT/GRID_H;
      int y1=(gy+1)*ENV_HEIGHT/GRID_H;

      uint32_t sum=0;
      uint16_t samples=0;

      for(int y=y0;y<y1;y+=2){
        for(int x=x0;x<x1;x+=2){
          sum+=getBrightness(image[y*ENV_WIDTH+x]);
          samples++;
        }
      }

      currentGrid[gy*GRID_W+gx]=samples?sum/samples:0;
    }
  }

  if(previousValid){
    for(int gy=0;gy<GRID_H;gy++){
      for(int gx=0;gx<GRID_W;gx++){
        int index=gy*GRID_W+gx;
        int diff=abs(
          (int)currentGrid[index]-(int)previousGrid[index]
        );

        if(diff>=MOTION_THRESHOLD){
          changedCells++;
          int cx=gx*ENV_WIDTH/GRID_W+ENV_WIDTH/GRID_W/2;
          uint8_t r=cx<32?0:(cx<64?1:2);
          changedRegion[r]++;
        }
      }
    }
  }

  memcpy(previousGrid,currentGrid,sizeof(previousGrid));
  previousValid=true;

  // Penilaian keterbukaan visual
  bool *clear[3]={
    &result.leftClear,
    &result.centerClear,
    &result.rightClear
  };

  for(int i=0;i<3;i++){
    SectorInfo &d=detail[i];
    if(!d.samples)continue;

    uint16_t edgeRate=(uint32_t)d.edges*100/d.samples;
    uint16_t textureRate=(uint32_t)d.texture*100/d.samples;
    uint16_t colorRate=(uint32_t)d.colorChanges*100/d.samples;
    uint16_t avgContrast=d.contrast/d.samples;

    uint16_t clutter=
      edgeRate*2+
      textureRate+
      colorRate+
      avgContrast/3;

    uint8_t bright=i==0?result.leftBright:
                   i==1?result.centerBright:result.rightBright;

    *clear[i]=(bright>20 && clutter<95);
  }

  result.valid=true;
  result.obstacle=!result.centerClear;

  uint8_t clearCount=
    result.leftClear+result.centerClear+result.rightClear;
  result.confidence=clearCount*100/3;

  result.motion=changedCells>=MIN_MOTION_CELLS;
  result.motionLevel=changedCells;

  if(changedCells>=GLOBAL_CHANGE_CELLS){
    result.event=ENV_SCENE_CHANGED;
  }else if(result.motion){
    if(changedRegion[0]>=changedRegion[1]&&
       changedRegion[0]>=changedRegion[2])
      result.event=ENV_MOTION_LEFT;
    else if(changedRegion[1]>=changedRegion[0]&&
            changedRegion[1]>=changedRegion[2])
      result.event=ENV_MOTION_CENTER;
    else
      result.event=ENV_MOTION_RIGHT;
  }else{
    result.event=ENV_NONE;
  }

  result.dominantColor=bestColor(global,result.colorConfidence);

  if(dominantColor>ENV_COLOR_UNKNOWN&&
     dominantColor<=ENV_COLOR_PURPLE){
    result.dominantColor=(EnvColor)dominantColor;
    result.colorConfidence=colorConfidence;
  }

  state=result;
  return true;
}

EnvState envGet(){
  return state;
}
