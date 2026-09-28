#include "JPEGEncoderWrapper.h"
#include "Config.h"
#include "Log.h"
#include <Arduino.h>

#if OV7670_ENABLE_JPEG

#if defined(__has_include)
#  if __has_include(<JPEGENC.h>)
#    include <JPEGENC.h>
#    define HAVE_JPEGENC 1
#  elif __has_include("JPEGENC.h")
#    include "JPEGENC.h"
#    define HAVE_JPEGENC 1
#  elif __has_include(<JPEGEncoder.h>)
#    include <JPEGEncoder.h>
#    define HAVE_JPEG_ENCODER 1
#  elif __has_include("JPEGEncoder.h")
#    include "JPEGEncoder.h"
#    define HAVE_JPEG_ENCODER 1
#  endif
#endif

#if defined(HAVE_JPEGENC)

static JPEGENC jpg;
static JPEGENCODE jpe;
static bool jpegActive = false;
static uint8_t* jpegOut = nullptr;
static size_t jpegCapacity = 0;

bool JPEGEncoderWrapper::begin(
  uint8_t* outBuffer,
  size_t outCapacity,
  int xres,
  int yres,
  int quality)
{
  if (jpegActive) {
    jpg.close();
    jpegActive = false;
  }

  if (!outBuffer || !outCapacity || xres <= 0 || yres <= 0) {
    DEBUG_PRINTLN("JPEGEncoderWrapper: invalid begin arguments");
    return false;
  }

  jpegOut = outBuffer;
  jpegCapacity = outCapacity;

  int rc = jpg.open(
    jpegOut,
    (int)jpegCapacity
  );

  if (rc != JPEGE_SUCCESS) {
    DEBUG_PRINTLN("JPEGEncoderWrapper: jpg.open failed");
    jpegOut = nullptr;
    jpegCapacity = 0;
    return false;
  }

  int q = JPEGE_Q_HIGH;

  if (quality <= 25)
    q = JPEGE_Q_LOW;
  else if (quality <= 50)
    q = JPEGE_Q_MED;
  else if (quality <= 75)
    q = JPEGE_Q_HIGH;
  else
    q = JPEGE_Q_BEST;

  rc = jpg.encodeBegin(
    &jpe,
    xres,
    yres,
    JPEGE_PIXEL_RGB565,
    JPEGE_SUBSAMPLE_420,
    q
  );

  if (rc != JPEGE_SUCCESS) {
    DEBUG_PRINTLN("JPEGEncoderWrapper: encodeBegin failed");
    jpg.close();
    jpegOut = nullptr;
    jpegCapacity = 0;
    return false;
  }

  jpegActive = true;

  DEBUG_PRINT("JPEGEncoderWrapper: BEGIN ");
  DEBUG_PRINT(xres);
  DEBUG_PRINT("x");
  DEBUG_PRINT(yres);
  DEBUG_PRINT(" MCU=");
  DEBUG_PRINT(jpe.cx);
  DEBUG_PRINT("x");
  DEBUG_PRINTLN(jpe.cy);

  return true;
}

bool JPEGEncoderWrapper::addBlock(
  const uint8_t* rgb565,
  int width,
  int height)
{
  if (!jpegActive || !rgb565 || width <= 0 || height <= 0) {
    DEBUG_PRINTLN("JPEGEncoderWrapper: invalid addBlock");
    return false;
  }

  const int mcuX = jpe.cx;
  const int mcuY = jpe.cy;

  /*
   * JPEGENC::addMCU() menerima satu MCU.
   *
   * Block kamera boleh lebih besar dari MCU.
   * Kita pecah block menjadi MCU secara horizontal
   * dan vertikal, lalu kirim satu per satu.
   *
   * Karena kamera 320x240 nantinya menggunakan block
   * 320x16, block tersebut akan menghasilkan beberapa
   * MCU sesuai ukuran MCU JPEGENC.
   */

  for (int my = 0; my < height; my += mcuY) {
    for (int mx = 0; mx < width; mx += mcuX) {

      size_t mcuPixels = (size_t)mcuX * (size_t)mcuY;
      size_t mcuBytes = mcuPixels * 2;

      /*
       * MCU buffer kecil.
       * Ukurannya hanya sebesar satu MCU, bukan
       * seluruh frame 320x240.
       */
      uint8_t* mcuBuf = (uint8_t*)malloc(mcuBytes);

      if (!mcuBuf) {
        DEBUG_PRINTLN("JPEGEncoderWrapper: MCU malloc failed");
        jpegActive = false;
        jpg.close();
        jpegOut = nullptr;
        jpegCapacity = 0;
        return false;
      }

      size_t idx = 0;

      for (int yy = 0; yy < mcuY; ++yy) {
        int srcY = my + yy;

        for (int xx = 0; xx < mcuX; ++xx) {
          int srcX = mx + xx;

          if (srcX < width && srcY < height) {
            const uint16_t* src =
              (const uint16_t*)rgb565;

            uint16_t p =
              src[srcY * width + srcX];

            mcuBuf[idx++] = (uint8_t)(p & 0xFF);
            mcuBuf[idx++] = (uint8_t)(p >> 8);
          }
          else {
            /*
             * Padding untuk block yang tidak penuh.
             * Nilai abu-abu netral.
             */
            mcuBuf[idx++] = 0x00;
            mcuBuf[idx++] = 0x80;
          }
        }
      }

      int rc = jpg.addMCU(
        &jpe,
        mcuBuf,
        mcuX * 2
      );

      free(mcuBuf);

      if (rc != JPEGE_SUCCESS) {
        DEBUG_PRINTLN("JPEGEncoderWrapper: addMCU failed");

        jpegActive = false;
        jpg.close();
        jpegOut = nullptr;
        jpegCapacity = 0;

        return false;
      }
    }
  }

  return true;
}

bool JPEGEncoderWrapper::finish(size_t* outLen)
{
  if (!jpegActive || !outLen) {
    DEBUG_PRINTLN("JPEGEncoderWrapper: invalid finish");
    return false;
  }

  int outSize = jpg.close();

  jpegActive = false;

  if (outSize <= 0) {
    DEBUG_PRINTLN("JPEGEncoderWrapper: jpg.close returned 0");

    jpegOut = nullptr;
    jpegCapacity = 0;

    return false;
  }

  *outLen = (size_t)outSize;

  DEBUG_PRINT("JPEGEncoderWrapper: FINISH JPEG=");
  DEBUG_PRINTLN((unsigned)*outLen);

  jpegOut = nullptr;
  jpegCapacity = 0;

  return true;
}

bool JPEGEncoderWrapper::available()
{
  return true;
}

#elif defined(HAVE_JPEG_ENCODER)

bool JPEGEncoderWrapper::begin(
  uint8_t*,
  size_t,
  int,
  int,
  int)
{
  DEBUG_PRINTLN(
    "JPEGEncoderWrapper: JPEGEncoder detected but not supported"
  );
  return false;
}

bool JPEGEncoderWrapper::addBlock(
  const uint8_t*,
  int,
  int)
{
  return false;
}

bool JPEGEncoderWrapper::finish(size_t*)
{
  return false;
}

bool JPEGEncoderWrapper::available()
{
  return false;
}

#else

bool JPEGEncoderWrapper::begin(
  uint8_t*,
  size_t,
  int,
  int,
  int)
{
  DEBUG_PRINTLN(
    "JPEGEncoderWrapper: encoder library not detected"
  );
  return false;
}

bool JPEGEncoderWrapper::addBlock(
  const uint8_t*,
  int,
  int)
{
  return false;
}

bool JPEGEncoderWrapper::finish(size_t*)
{
  return false;
}

bool JPEGEncoderWrapper::available()
{
  return false;
}

#endif

#else

bool JPEGEncoderWrapper::begin(
  uint8_t*,
  size_t,
  int,
  int,
  int)
{
  DEBUG_PRINTLN(
    "JPEGEncoderWrapper: JPEG support compiled out"
  );
  return false;
}

bool JPEGEncoderWrapper::addBlock(
  const uint8_t*,
  int,
  int)
{
  return false;
}

bool JPEGEncoderWrapper::finish(size_t*)
{
  return false;
}

bool JPEGEncoderWrapper::available()
{
  return false;
}

#endif
