#include "OV7670.h"
#include "XClk.h"
#include "Log.h"

OV7670::OV7670(
  int XCLK,
  int SIOD,
  int SIOC,
  int VSYNC,
  int HREF,
  int PCLK,
  int D0,
  int D1,
  int D2,
  int D3,
  int D4,
  int D5,
  int D6,
  int D7,
  Mode m
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
  i2c.writeRegister(ADDR,REG_COM7,0b10000000);
  i2c.writeRegister(ADDR,REG_CLKRC,0b10000000);
  i2c.writeRegister(ADDR,REG_COM11,0b1000|0b10);

  i2c.writeRegister(ADDR,REG_COM7,0b100);
  i2c.writeRegister(ADDR,REG_COM15,0b11000000|0b010000);

  QVGA();

  frameControl(196,52,8,488);

  i2c.writeRegister(ADDR,0xb0,0x84);

  saturation(0);

  // AEC + AGC + AWB otomatis
  i2c.writeRegister(ADDR,0x13,0xe7);

  i2c.writeRegister(ADDR,0x6f,0x9f);
}

void OV7670::QVGA()
{
  i2c.writeRegister(ADDR,REG_COM3,0x04);
  i2c.writeRegister(ADDR,REG_COM14,0x19);

  i2c.writeRegister(
    ADDR,
    REG_SCALING_XSC,
    0x3a
  );

  i2c.writeRegister(
    ADDR,
    REG_SCALING_YSC,
    0x35
  );

  i2c.writeRegister(
    ADDR,
    REG_SCALING_DCWCTR,
    0x11
  );

  i2c.writeRegister(
    ADDR,
    REG_SCALING_PCLK_DIV,
    0xf1
  );

  i2c.writeRegister(
    ADDR,
    REG_SCALING_PCLK_DELAY,
    0x02
  );
}

void OV7670::QQVGARGB565()
{
  i2c.writeRegister(ADDR,REG_COM7,0b10000000);
  i2c.writeRegister(ADDR,REG_CLKRC,0b10000000);
  i2c.writeRegister(ADDR,REG_COM11,0b1000|0b10);

  i2c.writeRegister(ADDR,REG_COM7,0b100);
  i2c.writeRegister(ADDR,REG_COM15,0b11000000|0b010000);

  QQVGA();

  frameControl(196,52,8,488);

  i2c.writeRegister(ADDR,0xb0,0x84);

  saturation(0);

  // AEC + AGC + AWB otomatis
  i2c.writeRegister(ADDR,0x13,0xe7);

  i2c.writeRegister(ADDR,0x6f,0x9f);
}

void OV7670::QQVGA()
{
  i2c.writeRegister(ADDR,REG_COM3,0x04);
  i2c.writeRegister(ADDR,REG_COM14,0x1a);

  i2c.writeRegister(
    ADDR,
    REG_SCALING_XSC,
    0x3a
  );

  i2c.writeRegister(
    ADDR,
    REG_SCALING_YSC,
    0x35
  );

  i2c.writeRegister(
    ADDR,
    REG_SCALING_DCWCTR,
    0x22
  );

  i2c.writeRegister(
    ADDR,
    REG_SCALING_PCLK_DIV,
    0xf1
  );

  i2c.writeRegister(
    ADDR,
    REG_SCALING_PCLK_DELAY,
    0x02
  );
}

void OV7670::QQQVGA()
{
  i2c.writeRegister(ADDR,REG_COM3,0x04);
  i2c.writeRegister(ADDR,REG_COM14,0x1b);

  i2c.writeRegister(
    ADDR,
    REG_SCALING_XSC,
    0x3a
  );

  i2c.writeRegister(
    ADDR,
    REG_SCALING_YSC,
    0x35
  );

  i2c.writeRegister(
    ADDR,
    REG_SCALING_DCWCTR,
    0x33
  );

  i2c.writeRegister(
    ADDR,
    REG_SCALING_PCLK_DIV,
    0xf1
  );

  i2c.writeRegister(
    ADDR,
    REG_SCALING_PCLK_DELAY,
    0x02
  );
}

void OV7670::QQQVGA_RGB565()
{
  i2c.writeRegister(ADDR,REG_COM7,0b10000000);
  i2c.writeRegister(ADDR,REG_CLKRC,0b10000000);
  i2c.writeRegister(ADDR,REG_COM11,0b1000|0b10);

  i2c.writeRegister(ADDR,REG_COM7,0b100);
  i2c.writeRegister(ADDR,REG_COM15,0b11000000|0b010000);

  QQQVGA();

  frameControl(196,52,8,488);

  i2c.writeRegister(ADDR,0xb0,0x84);

  saturation(0);

  // AEC + AGC + AWB otomatis
  i2c.writeRegister(ADDR,0x13,0xe7);

  i2c.writeRegister(ADDR,0x6f,0x9f);
}

void OV7670::frameControl(
  uint8_t hStart,
  uint8_t hStop,
  uint8_t vStart,
  uint8_t vStop
)
{
  i2c.writeRegister(ADDR,REG_HSTART,hStart);
  i2c.writeRegister(ADDR,REG_HSTOP,hStop);
  i2c.writeRegister(ADDR,REG_HREF,
    ((hStop&0x03)<<6)|
    ((hStart&0x03)<<4)|
    ((vStop&0x03)<<2)|
    (vStart&0x03)
  );

  i2c.writeRegister(ADDR,REG_VSTART,vStart);
  i2c.writeRegister(ADDR,REG_VSTOP,vStop);
}

void OV7670::saturation(int s)
{
  uint8_t sat=(uint8_t)(0x5e+(0x2f*s)/2);

  i2c.writeRegister(ADDR,0x4f,sat);
  i2c.writeRegister(ADDR,0x50,sat);
  i2c.writeRegister(ADDR,0x51,0x00);
  i2c.writeRegister(ADDR,0x52,0x00);
  i2c.writeRegister(ADDR,0x53,sat);
  i2c.writeRegister(ADDR,0x54,sat);
}
