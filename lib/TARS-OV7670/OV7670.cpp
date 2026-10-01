#include "OV7670.h"
#include "XClk.h"
#include "Log.h"

OV7670::OV7670(
  Mode m,const int SIOD,const int SIOC,const int VSYNC,const int HREF,
  const int XCLK,const int PCLK,const int D0,const int D1,const int D2,
  const int D3,const int D4,const int D5,const int D6,const int D7):i2c(SIOD,SIOC)
{
  ClockEnable(XCLK,20000000);
  DEBUG_PRINT("Waiting for VSYNC...");
  pinMode(VSYNC,INPUT);
  while(!digitalRead(VSYNC));
  while(digitalRead(VSYNC));
  DEBUG_PRINTLN(" done");

  mode=m;
  switch(mode){
    case VGA_RGB565:xres=640;yres=480;break;
    case QVGA_RGB565:xres=320;yres=240;QVGARGB565();break;
    case QQVGA_RGB565:xres=160;yres=120;QQVGARGB565();break;
    case QQQVGA_RGB565:xres=80;yres=60;QQQVGA();break;
    default:xres=0;yres=0;break;
  }

  I2SCamera::init(xres,yres,VSYNC,HREF,XCLK,PCLK,D0,D1,D2,D3,D4,D5,D6,D7);
}

void OV7670::testImage(){
  i2c.writeRegister(ADDR,0x71,0xB5);
}

void OV7670::saturation(int s){
  i2c.writeRegister(ADDR,0x4f,0x80+0x20*s);
  i2c.writeRegister(ADDR,0x50,0x80+0x20*s);
  i2c.writeRegister(ADDR,0x51,0x00);
  i2c.writeRegister(ADDR,0x52,0x22+(0x11*s)/2);
  i2c.writeRegister(ADDR,0x53,0x5e);
  i2c.writeRegister(ADDR,0x54,0x80+0x20*s);
  i2c.writeRegister(ADDR,0x58,0x9e);
}

void OV7670::frameControl(int hStart,int hStop,int vStart,int vStop){
  i2c.writeRegister(ADDR,REG_HSTART,hStart>>3);
  i2c.writeRegister(ADDR,REG_HSTOP,hStop>>3);
  i2c.writeRegister(ADDR,REG_HREF,((hStop&0x07)<<3)|(hStart&0x07));
  i2c.writeRegister(ADDR,REG_VSTART,vStart>>2);
  i2c.writeRegister(ADDR,REG_VSTOP,vStop>>2);
  i2c.writeRegister(ADDR,REG_VREF,((vStop&0x03)<<2)|(vStart&0x03));
}

void OV7670::QQQVGA(){
  i2c.writeRegister(ADDR,REG_COM3,0x04);
  i2c.writeRegister(ADDR,REG_COM14,0x1b);
  i2c.writeRegister(ADDR,REG_SCALING_XSC,0x3a);
  i2c.writeRegister(ADDR,REG_SCALING_YSC,0x35);
  i2c.writeRegister(ADDR,REG_SCALING_DCWCTR,0x33);
  i2c.writeRegister(ADDR,REG_SCALING_PCLK_DIV,0xf3);
  i2c.writeRegister(ADDR,REG_SCALING_PCLK_DELAY,0x02);
}

void OV7670::QQVGA(){
  i2c.writeRegister(ADDR,REG_COM3,0x04);
  i2c.writeRegister(ADDR,REG_COM14,0x1a);
  i2c.writeRegister(ADDR,REG_SCALING_XSC,0x3a);
  i2c.writeRegister(ADDR,REG_SCALING_YSC,0x35);
  i2c.writeRegister(ADDR,REG_SCALING_DCWCTR,0x22);
  i2c.writeRegister(ADDR,REG_SCALING_PCLK_DIV,0xf2);
  i2c.writeRegister(ADDR,REG_SCALING_PCLK_DELAY,0x02);
}

void OV7670::QVGA(){
  i2c.writeRegister(ADDR,REG_COM3,0x04);
  i2c.writeRegister(ADDR,REG_COM14,0x19);
  i2c.writeRegister(ADDR,REG_SCALING_XSC,0x3a);
  i2c.writeRegister(ADDR,REG_SCALING_YSC,0x35);
  i2c.writeRegister(ADDR,REG_SCALING_DCWCTR,0x11);
  i2c.writeRegister(ADDR,REG_SCALING_PCLK_DIV,0xf1);
  i2c.writeRegister(ADDR,REG_SCALING_PCLK_DELAY,0x02);
}

void OV7670::QQVGARGB565(){
  i2c.writeRegister(ADDR,REG_COM7,0x80);
  i2c.writeRegister(ADDR,REG_CLKRC,0x80);
  i2c.writeRegister(ADDR,REG_COM11,0x0a);
  i2c.writeRegister(ADDR,REG_COM7,0x04);
  i2c.writeRegister(ADDR,REG_COM15,0xd0);

  QQVGA();
  frameControl(196,52,8,488);

  i2c.writeRegister(ADDR,0xb0,0x84);
  i2c.writeRegister(ADDR,0x4f,0xb3);
  i2c.writeRegister(ADDR,0x50,0xb3);
  i2c.writeRegister(ADDR,0x51,0x00);
  i2c.writeRegister(ADDR,0x52,0x3d);
  i2c.writeRegister(ADDR,0x53,0xa7);
  i2c.writeRegister(ADDR,0x54,0xe4);
  i2c.writeRegister(ADDR,0x58,0x9e);

  i2c.writeRegister(ADDR,0x13,0xe7);
  i2c.writeRegister(ADDR,0x41,0x08);
  i2c.writeRegister(ADDR,0x14,0x30);
  i2c.writeRegister(ADDR,0x42,0x00);
  i2c.writeRegister(ADDR,0x3f,0x08);
  i2c.writeRegister(ADDR,0x55,0x00);
  i2c.writeRegister(ADDR,0x56,0x40);
}

void OV7670::QVGARGB565(){
  i2c.writeRegister(ADDR,REG_COM7,0x80);
  i2c.writeRegister(ADDR,REG_CLKRC,0x80);
  i2c.writeRegister(ADDR,REG_COM11,0x0a);
  i2c.writeRegister(ADDR,REG_COM7,0x04);
  i2c.writeRegister(ADDR,REG_COM15,0xd0);

  QVGA();
  frameControl(168,24,12,492);

  i2c.writeRegister(ADDR,0xb0,0x84);

  i2c.writeRegister(ADDR,0x4f,0xb3);
  i2c.writeRegister(ADDR,0x50,0xb3);
  i2c.writeRegister(ADDR,0x51,0x00);
  i2c.writeRegister(ADDR,0x52,0x3d);
  i2c.writeRegister(ADDR,0x53,0xa7);
  i2c.writeRegister(ADDR,0x54,0xe4);
  i2c.writeRegister(ADDR,0x58,0x9e);

  i2c.writeRegister(ADDR,0x13,0xe7);
  i2c.writeRegister(ADDR,0x41,0x08);
  i2c.writeRegister(ADDR,0x14,0x30);
  i2c.writeRegister(ADDR,0x42,0x00);
  i2c.writeRegister(ADDR,0x3f,0x08);
  i2c.writeRegister(ADDR,0x55,0x00);
  i2c.writeRegister(ADDR,0x56,0x40);
}
