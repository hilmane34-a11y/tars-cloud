#include "JPEGEncoderWrapper.h"
#include "Config.h"
#include "Log.h"
#include <Arduino.h>
#include <string.h>

#if OV7670_ENABLE_JPEG

#if defined(__has_include)
  #if __has_include(<JPEGENC.h>)
    #include <JPEGENC.h>
    #define HAVE_JPEGENC 1
  #elif __has_include("JPEGENC.h")
    #include "JPEGENC.h"
    #define HAVE_JPEGENC 1
  #elif __has_include(<JPEGEncoder.h>)
    #include <JPEGEncoder.h>
    #define HAVE_JPEG_ENCODER 1
  #endif
#endif

#if defined(HAVE_JPEGENC)

static JPEGENC jpg;
static JPEGENCODE jpe;
static bool jpegActive = false;
static uint8_t* jpegOut = nullptr;
static size_t jpegCapacity = 0;

// Buffer statis: menampung 2 strip kamera berukuran 320x8.
static uint8_t jpegStrip[320 * 16 * 2];
static int jpegStripWidth = 0;
static bool jpegHalfPending = false;

static bool encodeStrip16()
{
  if (!jpegActive || jpe.cx != 16 || jpe.cy != 16)
    return false;

  const uint16_t* src = (const uint16_t*)jpegStrip;
  const int stride = jpegStripWidth * 2;

  for (int x = 0; x < jpegStripWidth; x += 16) {
    int rc = jpg.addMCU(&jpe, (uint8_t*)&src[x], stride);

    if (rc != JPEGE_SUCCESS) {
      DEBUG_PRINT("JPEGEncoderWrapper: addMCU failed rc=");
      DEBUG_PRINTLN(rc);

      jpegActive = false;
      jpegHalfPending = false;
      jpg.close();
      jpegOut = nullptr;
      jpegCapacity = 0;
      return false;
    }
  }

  return true;
}

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

  jpegHalfPending = false;
  jpegStripWidth = 0;
  jpegOut = nullptr;
  jpegCapacity = 0;

  if (!outBuffer || !outCapacity ||
      xres <= 0 || xres > 320 ||
      yres <= 0 || xres % 16 != 0 ||
      yres % 16 != 0) {
    DEBUG_PRINTLN("JPEGEncoderWrapper: invalid arguments");
    return false;
  }

  jpegOut = outBuffer;
  jpegCapacity = outCapacity;

  if (jpg.open(jpegOut, (int)jpegCapacity) != JPEGE_SUCCESS) {
    DEBUG_PRINTLN("JPEGEncoderWrapper: jpg.open failed");
    jpegOut = nullptr;
    jpegCapacity = 0;
    return false;
  }

  int q = JPEGE_Q_HIGH;
  if (quality <= 25) q = JPEGE_Q_LOW;
  else if (quality <= 50) q = JPEGE_Q_MED;
  else if (quality <= 75) q = JPEGE_Q_HIGH;
  else q = JPEGE_Q_BEST;

  int rc = jpg.encodeBegin(
    &jpe, xres, yres,
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

  jpegStripWidth = xres;
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
  if (!jpegActive || !rgb565 ||
      width != jpegStripWidth || width % 16 != 0) {
    DEBUG_PRINTLN("JPEGEncoderWrapper: invalid block");
    return false;
  }

  // Tetap mendukung strip lama setinggi 16 baris.
  if (height == 16) {
    if (jpegHalfPending) {
      DEBUG_PRINTLN("JPEGEncoderWrapper: incomplete strip");
      return false;
    }

    memcpy(jpegStrip, rgb565, (size_t)width * 16 * 2);
    return encodeStrip16();
  }

  // Mode hemat RAM: gabungkan dua strip 8 baris.
  if (height == 8) {
    const size_t halfBytes = (size_t)width * 8 * 2;

    if (!jpegHalfPending) {
      memcpy(jpegStrip, rgb565, halfBytes);
      jpegHalfPending = true;
      return true;
    }

    memcpy(jpegStrip + halfBytes, rgb565, halfBytes);
    jpegHalfPending = false;
    return encodeStrip16();
  }

  DEBUG_PRINTLN("JPEGEncoderWrapper: unsupported block height");
  return false;
}

bool JPEGEncoderWrapper::finish(size_t* outLen)
{
  if (!jpegActive || !outLen || jpegHalfPending) {
    DEBUG_PRINTLN("JPEGEncoderWrapper: incomplete or invalid finish");
    return false;
  }

  int outSize = jpg.close();
  jpegActive = false;
  jpegOut = nullptr;
  jpegCapacity = 0;

  if (outSize <= 0) {
    DEBUG_PRINTLN("JPEGEncoderWrapper: jpg.close returned 0");
    return false;
  }

  *outLen = (size_t)outSize;

  DEBUG_PRINT("JPEGEncoderWrapper: FINISH JPEG=");
  DEBUG_PRINTLN((unsigned)*outLen);

  return true;
}

bool JPEGEncoderWrapper::available()
{
  return true;
}

#elif defined(HAVE_JPEG_ENCODER)

bool JPEGEncoderWrapper::begin(uint8_t*, size_t, int, int, int) { return false; }
bool JPEGEncoderWrapper::addBlock(const uint8_t*, int, int) { return false; }
bool JPEGEncoderWrapper::finish(size_t*) { return false; }
bool JPEGEncoderWrapper::available() { return false; }

#else

bool JPEGEncoderWrapper::begin(uint8_t*, size_t, int, int, int) { return false; }
bool JPEGEncoderWrapper::addBlock(const uint8_t*, int, int) { return false; }
bool JPEGEncoderWrapper::finish(size_t*) { return false; }
bool JPEGEncoderWrapper::available() { return false; }

#endif
#else

bool JPEGEncoderWrapper::begin(uint8_t*, size_t, int, int, int) { return false; }
bool JPEGEncoderWrapper::addBlock(const uint8_t*, int, int) { return false; }
bool JPEGEncoderWrapper::finish(size_t*) { return false; }
bool JPEGEncoderWrapper::available() { return false; }

#endif
