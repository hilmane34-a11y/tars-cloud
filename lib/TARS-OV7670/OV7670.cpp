#include "OV7670.h"
#include "XClk.h"
#include "Log.h"

OV7670::OV7670(
  Mode m,
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
  const int D7)
  :i2c(SIOD,SIOC)
{
  ClockEnable(XCLK,20000000);

  DEBUG_PRINT("Waiting for VSYNC...");
  pinMode(VSYNC,INPUT);
  while(!digitalRead(VSYNC));
  while(digitalRead(VSYNC));
  DEBUG_PRINTLN(" done");

  mode=m;

  switch(mode)
  {
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

void OV7670::testImage()
{
  i2c.writeRegister(ADDR,0x71,0x35|0x80);
}

void OV7670::saturation(int s)
{
  i2c.writeRegister(ADDR,0x4f,0x80+0x20*s);
  i2c.writeRegister(ADDR,0x50,0x80+0x20*s);
  i2c.writeRegister(ADDR,0x51,0x00);
  i2c.writeRegister(ADDR,0x52,0x22+(0x11*s)/2);
  i2c.writeRegister(ADDR,0x53,(uint8_t)(0x5e+(0x2f*s)/2));
  i2c.writeRegister(ADDR,0x54,0x80+0x20*s);
  i2c.writeRegister(ADDR,0x58,0x9e);
}

void OV7670::frameControl(
  int hStart,
  int hStop,
  int vStart,
  int vStop)
{
  i2c.writeRegister(
    ADDR,
    REG_HSTART,
    hStart>>3
  );

  i2c.writeRegister(
    ADDR,
    REG_HSTOP,
    hStop>>3
  );

  i2c.writeRegister(
    ADDR,
    REG_HREF,
    ((hStop&0b111)<<3)|(hStart&0b111)
  );

  i2c.writeRegister(
    ADDR,
    REG_VSTART,
    vStart>>2
  );

  i2c.writeRegister(
    ADDR,
    REG_VSTOP,
    vStop>>2
  );

  i2c.writeRegister(
    ADDR,
    REG_VREF,
    ((vStop&0b11)<<2)|(vStart&0b11)
  );
}

void OV7670::QQQVGA()
{
  i2c.writeRegister(
    ADDR,
    REG_COM3,
    0x04
  );

  i2c.writeRegister(
    ADDR,
    REG_COM14,
    0x1b
  );

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
    0xf3
  );

  i2c.writeRegister(
    ADDR,
    REG_SCALING_PCLK_DELAY,
    0x02
  );
}

void OV7670::QQVGA()
{
  i2c.writeRegister(
    ADDR,
    REG_COM3,
    0x04
  );

  i2c.writeRegister(
    ADDR,
    REG_COM14,
    0x1a
  );

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
    0xf2
  );

  i2c.writeRegister(
    ADDR,
    REG_SCALING_PCLK_DELAY,
    0x02
  );
}

void OV7670::QVGA()
{
  /*
   * 320x240:
   * DCW downsample 2x2.
   * Konfigurasi ini sesuai register QVGA
   * pada driver OV7670 ESP32.
   */

  i2c.writeRegister(
    ADDR,
    REG_COM3,
    0x04
  );

  i2c.writeRegister(
    ADDR,
    REG_COM14,
    0x19
  );

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
  i2c.writeRegister(
    ADDR,
    REG_COM7,
    0b10000000
  );

  i2c.writeRegister(
    ADDR,
    REG_CLKRC,
    0b10000000
  );

  i2c.writeRegister(
    ADDR,
    REG_COM11,
    0b1000|0b10
  );

  i2c.writeRegister(
    ADDR,
    REG_COM7,
    0b100
  );

  i2c.writeRegister(
    ADDR,
    REG_COM15,
    0b11000000|0b010000
  );

  QQVGA();

  frameControl(
    196,
    52,
    8,
    488
  );

  i2c.writeRegister(
    ADDR,
    0xb0,
    0x84
  );

  saturation(0);

  i2c.writeRegister(
    ADDR,
    0x13,
    0xe7
  );

  i2c.writeRegister(
    ADDR,
    0x6f,
    0x9f
  );
}

void OV7670::QQQVGARGB565()
{
  i2c.writeRegister(
    ADDR,
    REG_COM7,
    0b10000000
  );

  i2c.writeRegister(
    ADDR,
    REG_CLKRC,
    0b10000000
  );

  i2c.writeRegister(
    ADDR,
    REG_COM11,
    0b1000|0b10
  );

  i2c.writeRegister(
    ADDR,
    REG_COM7,
    0b100
  );

  i2c.writeRegister(
    ADDR,
    REG_COM15,
    0b11000000|0b010000
  );

  QQQVGA();

  frameControl(
    196,
    52,
    8,
    488
  );

  i2c.writeRegister(
    ADDR,
    0xb0,
    0x84
  );

  saturation(0);

  i2c.writeRegister(
    ADDR,
    0x13,
    0xe7
  );

  i2c.writeRegister(
    ADDR,
    0x6f,
    0x9f
  );
}

void OV7670::QVGARGB565()
{
  /*
   * Reset sensor register state.
   */
  i2c.writeRegister(
    ADDR,
    REG_COM7,
    0b10000000
  );

  /*
   * Sensor clock configuration.
   * Dipertahankan sama dengan mode RGB565
   * yang sudah bekerja pada QQVGA.
   */
  i2c.writeRegister(
    ADDR,
    REG_CLKRC,
    0b10000000
  );

  i2c.writeRegister(
    ADDR,
    REG_COM11,
    0b1000|0b10
  );

  /*
   * RGB mode.
   */
  i2c.writeRegister(
    ADDR,
    REG_COM7,
    0b100
  );

  /*
   * RGB565 + full 0..255 range.
   */
  i2c.writeRegister(
    ADDR,
    REG_COM15,
    0b11000000|0b010000
  );

  /*
   * QVGA 320x240 scaling.
   */
  QVGA();

  /*
   * Pertahankan window sensor yang sama
   * dengan konfigurasi kamera TARS yang
   * sebelumnya sudah bekerja.
   */
  frameControl(
    196,
    52,
    8,
    488
  );

  /*
   * Color / white-balance settings.
   */
  i2c.writeRegister(
    ADDR,
    0xb0,
    0x84
  );

  saturation(0);

  i2c.writeRegister(
    ADDR,
    0x13,
    0xe7
  );

  i2c.writeRegister(
    ADDR,
    0x6f,
    0x9f
  );
}
