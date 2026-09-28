#pragma once

#include "soc/soc.h"
#include "soc/gpio_sig_map.h"
#include "soc/i2s_reg.h"
#include "soc/i2s_struct.h"
#include "soc/io_mux_reg.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_reg.h"

#include "driver/gpio.h"
#include "driver/periph_ctrl.h"
#include "rom/lldesc.h"
#include "XClk.h"
#include "DMABuffer.h"
#include "esp_attr.h"
#include "JPEGEncoderWrapper.h"

class I2SCamera
{
public:
  enum ImageFormat {
    FORMAT_BMP,
    FORMAT_JPEG
  };

  static ImageFormat imageFormat;

  static bool encodeFrameToJPEG(
    uint8_t* outBuffer,
    size_t* outLen,
    int quality = 80
  );

  static gpio_num_t vSyncPin;
  static int blocksReceived;
  static int framesReceived;
  static int xres;
  static int yres;

  static intr_handle_t i2sInterruptHandle;
  static intr_handle_t vSyncInterruptHandle;

  static int dmaBufferCount;
  static int dmaBufferActive;
  static DMABuffer **dmaBuffer;

  /*
   * Tidak digunakan lagi sebagai framebuffer kamera.
   * Tetap dipertahankan agar kompatibel dengan struktur
   * library lama.
   */
  static unsigned char* frame;
  static int framePointer;
  static int frameBytes;

  static volatile bool stopSignal;

  /*
   * Mulai capture secara NON-BLOCKING.
   *
   * Capture akan berjalan di ISR.
   * encodeFrameToJPEG() kemudian mengambil block
   * hasil capture sambil frame masih berlangsung.
   */
  static void start()
  {
    i2sRun();
  }

  /*
   * Hentikan capture.
   */
  static void stop()
  {
    i2sStop();
    stopSignal=false;
  }

  /*
   * Mulai satu frame.
   *
   * Tidak menunggu sampai frame selesai.
   * encodeFrameToJPEG() yang mengonsumsi block.
   */
  static void oneFrame()
  {
    i2sRun();
  }

  static void i2sStop();
  static void i2sRun();

  static void dmaBufferInit(int bytes);
  static void dmaBufferDeinit();

  static bool initVSync(int pin);
  static void deinitVSync();
  static void deinit();

  static void IRAM_ATTR i2sInterrupt(void* arg);
  static void IRAM_ATTR vSyncInterrupt(void* arg);

  static bool i2sInit(
    const int VSYNC,
    const int HREF,
    const int PCLK,
    const int D0,
    const int D1,
    const int D2,
    const int D3,
    const int D4,
    const int D5,
    const int D6,
    const int D7
  );

  static bool init(
    const int XRES,
    const int YRES,
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
  );

  static inline void i2sConfReset()
  {
    const uint32_t lc_conf_reset_flags =
      I2S_IN_RST_M |
      I2S_AHBM_RST_M |
      I2S_AHBM_FIFO_RST_M;

    I2S0.lc_conf.val |= lc_conf_reset_flags;
    I2S0.lc_conf.val &= ~lc_conf_reset_flags;

    const uint32_t conf_reset_flags =
      I2S_RX_RESET_M |
      I2S_RX_FIFO_RESET_M |
      I2S_TX_RESET_M |
      I2S_TX_FIFO_RESET_M;

    I2S0.conf.val |= conf_reset_flags;
    I2S0.conf.val &= ~conf_reset_flags;

    while(I2S0.state.rx_fifo_reset_back);
  }
};
