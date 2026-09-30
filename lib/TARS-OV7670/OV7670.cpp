#include "OV7670.h"
#include "XClk.h"
#include "Log.h"

OV7670::OV7670(
  OV7670::Mode m,
  const int SIOD,
  const int SIOC,
  const int VSYNC,
  const int HREF,
  const int XCLK,
  const int PCLK,
  const int D0,
  const int D1,
  const int D2,
  const int D3,
  const int D4,
  const int D5,
  const int D6,
  const int D7
):I2SCamera(),i2c(SIOD,SIOC)
{
  ClockEnable(XCLK,20000000);
  DEBUG_PRINT("Waiting for VSYNC...");
  pinMode(VSYNC,INPUT);
  while(!digitalRead(VSYNC));
  while(digitalRead(VSYNC));
  DEBUG_PRINTLN(" done");

  mode=m;

  switch(mode){
    case VGA_RGB565:
      xres=640;
      yres=480;
      break;

    case QVGA_RGB565:
      xres=320;
      yres=240;
      QVGARGB565();
      break;

    case QQVGA_RGB565:
      xres=160;
      yres=120;
      QQVGARGB565();
      break;

    case QQQVGA_RGB565:
      xres=80;
      yres=60;
      QQQVGARGB565();
      break;

    default:
      xres=0;
      yres=0;
      break;
  }

  I2SCamera::init(
    xres,
    yres,
    VSYNC,
    HREF,
    XCLK,
    PCLK,
    D0,
    D1,
    D2,
    D3,
    D4,
    D5,
    D6,
    D7
  );
}

void OV7670::QVGARGB565()
{
  i2c.writeRegister(ADDR,REG_COM7,0x80);
  i2c.writeRegister(ADDR,REG_CLKRC,0x80);
  i2c.writeRegister(ADDR,REG_COM11,0x0A);
  i2c.writeRegister(ADDR,REG_COM7,0x04);
  i2c.writeRegister(ADDR,REG_COM15,0xD0);

  QVGA();
  frameControl(196,52,8,488);

  i2c.writeRegister(ADDR,0xB0,0x84);
  saturation(0);

  // AEC + AGC + AWB otomatis
  i2c.writeRegister(ADDR,REG_COM8,0xE7);

  i2c.writeRegister(ADDR,0x6F,0x9F);
}

void OV7670::QVGA()
{
  i2c.writeRegister(ADDR,REG_COM3,0x04);
  i2c.writeRegister(ADDR,REG_COM14,0x19);
  i2c.writeRegister(ADDR,REG_SCALING_XSC,0x3A);
  i2c.writeRegister(ADDR,REG_SCALING_YSC,0x35);
  i2c.writeRegister(ADDR,REG_SCALING_DCWCTR,0x11);
  i2c.writeRegister(ADDR,REG_SCALING_PCLK_DIV,0xF1);
  i2c.writeRegister(ADDR,REG_SCALING_PCLK_DELAY,0x02);
}

void OV7670::QQVGARGB565()
{
  i2c.writeRegister(ADDR,REG_COM7,0x80);
  i2c.writeRegister(ADDR,REG_CLKRC,0x80);
  i2c.writeRegister(ADDR,REG_COM11,0x0A);
  i2c.writeRegister(ADDR,REG_COM7,0x04);
  i2c.writeRegister(ADDR,REG_COM15,0xD0);

  QQVGA();
  frameControl(196,52,8,488);

  i2c.writeRegister(ADDR,0xB0,0x84);
  saturation(0);

  // AEC + AGC + AWB otomatis
  i2c.writeRegister(ADDR,REG_COM8,0xE7);

  i2c.writeRegister(ADDR,0x6F,0x9F);
}

void OV7670::QQVGA()
{
  i2c.writeRegister(ADDR,REG_COM3,0x04);
  i2c.writeRegister(ADDR,REG_COM14,0x1A);
  i2c.writeRegister(ADDR,REG_SCALING_XSC,0x3A);
  i2c.writeRegister(ADDR,REG_SCALING_YSC,0x35);
  i2c.writeRegister(ADDR,REG_SCALING_DCWCTR,0x22);
  i2c.writeRegister(ADDR,REG_SCALING_PCLK_DIV,0xF1);
  i2c.writeRegister(ADDR,REG_SCALING_PCLK_DELAY,0x02);
}

void OV7670::QQQVGARGB565()
{
  i2c.writeRegister(ADDR,REG_COM7,0x80);
  i2c.writeRegister(ADDR,REG_CLKRC,0x80);
  i2c.writeRegister(ADDR,REG_COM11,0x0A);
  i2c.writeRegister(ADDR,REG_COM7,0x04);
  i2c.writeRegister(ADDR,REG_COM15,0xD0);

  QQQVGA();
  frameControl(196,52,8,488);

  i2c.writeRegister(ADDR,0xB0,0x84);
  saturation(0);

  // AEC + AGC + AWB otomatis
  i2c.writeRegister(ADDR,REG_COM8,0xE7);

  i2c.writeRegister(ADDR,0x6F,0x9F);
}

void OV7670::QQQVGA()
{
  i2c.writeRegister(ADDR,REG_COM3,0x04);
  i2c.writeRegister(ADDR,REG_COM14,0x1B);
  i2c.writeRegister(ADDR,REG_SCALING_XSC,0x3A);
  i2c.writeRegister(ADDR,REG_SCALING_YSC,0x35);
  i2c.writeRegister(ADDR,REG_SCALING_DCWCTR,0x33);
  i2c.writeRegister(ADDR,REG_SCALING_PCLK_DIV,0xF1);
  i2c.writeRegister(ADDR,REG_SCALING_PCLK_DELAY,0x02);
}

void OV7670::frameControl(int hStart,int hStop,int vStart,int vStop)
{
  i2c.writeRegister(ADDR,REG_HSTART,(uint8_t)hStart);
  i2c.writeRegister(ADDR,REG_HSTOP,(uint8_t)hStop);
  i2c.writeRegister(
    ADDR,
    REG_HREF,
    (((uint8_t)hStop&0x03)<<6)|
    (((uint8_t)hStart&0x03)<<4)|
    (((uint8_t)vStop&0x03)<<2)|
    ((uint8_t)vStart&0x03)
  );
  i2c.writeRegister(ADDR,REG_VSTART,(uint8_t)vStart);
  i2c.writeRegister(ADDR,REG_VSTOP,(uint8_t)vStop);
}

void OV7670::saturation(int s)
{
  uint8_t sat=(uint8_t)(0x5E+(0x2F*s)/2);

  i2c.writeRegister(ADDR,0x4F,sat);
  i2c.writeRegister(ADDR,0x50,sat);
  i2c.writeRegister(ADDR,0x51,0x00);
  i2c.writeRegister(ADDR,0x52,0x00);
  i2c.writeRegister(ADDR,0x53,sat);
  i2c.writeRegister(ADDR,0x54,sat);
}
