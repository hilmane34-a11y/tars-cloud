#include "I2SCamera.h"
#include "Config.h"
#include "Log.h"

int I2SCamera::blocksReceived = 0;
int I2SCamera::framesReceived = 0;
int I2SCamera::xres = 640;
int I2SCamera::yres = 480;
I2SCamera::ImageFormat I2SCamera::imageFormat = I2SCamera::FORMAT_BMP;
gpio_num_t I2SCamera::vSyncPin = (gpio_num_t)0;
intr_handle_t I2SCamera::i2sInterruptHandle = 0;
intr_handle_t I2SCamera::vSyncInterruptHandle = 0;
int I2SCamera::dmaBufferCount = 0;
int I2SCamera::dmaBufferActive = 0;
DMABuffer **I2SCamera::dmaBuffer = 0;
unsigned char* I2SCamera::frame = 0;
int I2SCamera::framePointer = 0;
int I2SCamera::frameBytes = 0;
volatile bool I2SCamera::stopSignal = false;

void IRAM_ATTR I2SCamera::i2sInterrupt(void* arg)
{
  I2S0.int_clr.val = I2S0.int_raw.val;
  blocksReceived++;
  unsigned char* buf = dmaBuffer[dmaBufferActive]->buffer;
  dmaBufferActive = (dmaBufferActive + 1) % dmaBufferCount;

  if(framePointer < frameBytes)
    for(int i = 0; i < xres * 4; i += 4)
    {
      frame[framePointer++] = buf[i + 2];
      frame[framePointer++] = buf[i];
    }

  if(blocksReceived == yres)
  {
    framePointer = 0;
    blocksReceived = 0;
    framesReceived++;

    if(stopSignal)
    {
      i2sStop();
      stopSignal = false;
    }
  }
}

void IRAM_ATTR I2SCamera::vSyncInterrupt(void* arg)
{
  gpio_intr_disable(vSyncPin);

  if (gpio_get_level(vSyncPin)) {
  }

  gpio_intr_enable(vSyncPin);
}

void I2SCamera::i2sStop()
{
  esp_intr_disable(i2sInterruptHandle);
  esp_intr_disable(vSyncInterruptHandle);
  i2sConfReset();
  I2S0.conf.rx_start = 0;
}

void I2SCamera::i2sRun()
{
  DEBUG_PRINTLN("I2S Run");

  while (gpio_get_level(vSyncPin) == 0);
  while (gpio_get_level(vSyncPin) != 0);

  esp_intr_disable(i2sInterruptHandle);
  i2sConfReset();
  blocksReceived = 0;
  dmaBufferActive = 0;
  framePointer = 0;

  DEBUG_PRINT("Sample count ");
  DEBUG_PRINTLN(dmaBuffer[0]->sampleCount());

  I2S0.rx_eof_num = dmaBuffer[0]->sampleCount();
  I2S0.in_link.addr = (uint32_t)&(dmaBuffer[0]->descriptor);
  I2S0.in_link.start = 1;
  I2S0.int_clr.val = I2S0.int_raw.val;
  I2S0.int_ena.val = 0;
  I2S0.int_ena.in_done = 1;

  esp_intr_enable(i2sInterruptHandle);
  esp_intr_enable(vSyncInterruptHandle);
  I2S0.conf.rx_start = 1;
}

bool I2SCamera::initVSync(int pin)
{
  DEBUG_PRINT("Initializing VSYNC... ");

  vSyncPin = (gpio_num_t)pin;
  gpio_set_intr_type(vSyncPin, GPIO_INTR_POSEDGE);

  if (gpio_isr_register(
        &I2SCamera::vSyncInterrupt,
        (void*)"vSyncInterrupt",
        ESP_INTR_FLAG_INTRDISABLED | ESP_INTR_FLAG_IRAM,
        &vSyncInterruptHandle) != ESP_OK)
  {
    DEBUG_PRINTLN("vSync ISR registration failed!");
    return false;
  }

  gpio_intr_enable(vSyncPin);
  DEBUG_PRINTLN("done.");
  return true;
}

void I2SCamera::deinitVSync()
{
  if (vSyncInterruptHandle) {
    esp_intr_disable(vSyncInterruptHandle);
    esp_intr_free(vSyncInterruptHandle);
    vSyncInterruptHandle = 0;
  }
}

void I2SCamera::deinit()
{
  i2sStop();
  dmaBufferDeinit();

  if (frame) {
    free(frame);
    frame = nullptr;
  }

  if (i2sInterruptHandle) {
    esp_intr_disable(i2sInterruptHandle);
    esp_intr_free(i2sInterruptHandle);
    i2sInterruptHandle = 0;
  }

  if (vSyncInterruptHandle) {
    esp_intr_disable(vSyncInterruptHandle);
    esp_intr_free(vSyncInterruptHandle);
    vSyncInterruptHandle = 0;
  }
}

bool I2SCamera::init(
  const int XRES, const int YRES,
  const int VSYNC, const int HREF,
  const int XCLK, const int PCLK,
  const int D0, const int D1, const int D2, const int D3,
  const int D4, const int D5, const int D6, const int D7)
{
  xres = XRES;
  yres = YRES;
  frameBytes = XRES * YRES * 2;

  frame = (unsigned char*)malloc(frameBytes);

  if(!frame)
  {
    DEBUG_PRINTLN("Not enough memory for frame buffer!");
    return false;
  }

  i2sInit(VSYNC, HREF, PCLK, D0, D1, D2, D3, D4, D5, D6, D7);
  dmaBufferInit(xres * 2 * 2);
  initVSync(VSYNC);

  return true;
}

bool I2SCamera::i2sInit(
  const int VSYNC, const int HREF, const int PCLK,
  const int D0, const int D1, const int D2, const int D3,
  const int D4, const int D5, const int D6, const int D7)
{
  int pins[] = {VSYNC, HREF, PCLK, D0, D1, D2, D3, D4, D5, D6, D7};

  gpio_config_t conf = {
    .pin_bit_mask = 0,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE
  };

  for (int i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
    conf.pin_bit_mask = 1ULL << pins[i];
    gpio_config(&conf);
  }

  gpio_matrix_in(D0, I2S0I_DATA_IN0_IDX, false);
  gpio_matrix_in(D1, I2S0I_DATA_IN1_IDX, false);
  gpio_matrix_in(D2, I2S0I_DATA_IN2_IDX, false);
  gpio_matrix_in(D3, I2S0I_DATA_IN3_IDX, false);
  gpio_matrix_in(D4, I2S0I_DATA_IN4_IDX, false);
  gpio_matrix_in(D5, I2S0I_DATA_IN5_IDX, false);
  gpio_matrix_in(D6, I2S0I_DATA_IN6_IDX, false);
  gpio_matrix_in(D7, I2S0I_DATA_IN7_IDX, false);

  gpio_matrix_in(0x30, I2S0I_DATA_IN8_IDX, false);
  gpio_matrix_in(0x30, I2S0I_DATA_IN9_IDX, false);
  gpio_matrix_in(0x30, I2S0I_DATA_IN10_IDX, false);
  gpio_matrix_in(0x30, I2S0I_DATA_IN11_IDX, false);
  gpio_matrix_in(0x30, I2S0I_DATA_IN12_IDX, false);
  gpio_matrix_in(0x30, I2S0I_DATA_IN13_IDX, false);
  gpio_matrix_in(0x30, I2S0I_DATA_IN14_IDX, false);
  gpio_matrix_in(0x30, I2S0I_DATA_IN15_IDX, false);

  gpio_matrix_in(VSYNC, I2S0I_V_SYNC_IDX, true);
  gpio_matrix_in(0x38, I2S0I_H_SYNC_IDX, false);
  gpio_matrix_in(HREF, I2S0I_H_ENABLE_IDX, false);
  gpio_matrix_in(PCLK, I2S0I_WS_IN_IDX, false);

  periph_module_enable(PERIPH_I2S0_MODULE);

  i2sConfReset();

  I2S0.conf.rx_slave_mod = 1;
  I2S0.conf2.lcd_en = 1;
  I2S0.conf2.camera_en = 1;

  I2S0.clkm_conf.clkm_div_a = 1;
  I2S0.clkm_conf.clkm_div_b = 0;
  I2S0.clkm_conf.clkm_div_num = 2;

  I2S0.fifo_conf.dscr_en = 1;
  I2S0.fifo_conf.rx_fifo_mod =0;
  I2S0.fifo_conf.rx_fifo_mod_force_en = 1;
  I2S0.conf_chan.rx_chan_mod = 1;

  I2S0.sample_rate_conf.rx_bits_mod = 0;
  I2S0.conf.rx_right_first = 0;
  I2S0.conf.rx_msb_right = 0;
  I2S0.conf.rx_msb_shift = 0;
  I2S0.conf.rx_mono = 0;
  I2S0.conf.rx_short_sync = 0;
  I2S0.timing.val = 0;

  esp_err_t _res = esp_intr_alloc(
    ETS_I2S0_INTR_SOURCE,
    ESP_INTR_FLAG_INTRDISABLED | ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_IRAM,
    &I2SCamera::i2sInterrupt,
    NULL,
    &i2sInterruptHandle
  );

  if (_res != ESP_OK) {
    DEBUG_PRINTLN("Failed to allocate I2S interrupt");
    return false;
  }

  return true;
}

void I2SCamera::dmaBufferInit(int bytes)
{
  dmaBufferCount = 2;
  dmaBuffer = (DMABuffer**) malloc(sizeof(DMABuffer*) * dmaBufferCount);

  for(int i = 0; i < dmaBufferCount; i++)
  {
    dmaBuffer[i] = new DMABuffer(bytes);

    if(i)
      dmaBuffer[i-1]->next(dmaBuffer[i]);
  }

  dmaBuffer[dmaBufferCount - 1]->next(dmaBuffer[0]);
}

void I2SCamera::dmaBufferDeinit()
{
  if (!dmaBuffer) return;

  for(int i = 0; i < dmaBufferCount; i++)
    delete(dmaBuffer[i]);

  delete(dmaBuffer);

  dmaBuffer = 0;
  dmaBufferCount = 0;
}

bool I2SCamera::encodeFrameToJPEG(uint8_t* outBuffer, size_t* outLen, int quality)
{
  if (!OV7670_ENABLE_JPEG) {
    DEBUG_PRINTLN("I2SCamera::encodeFrameToJPEG: JPEG support compiled out");
    return false;
  }

  if(!JPEGEncoderWrapper::available()) {
    DEBUG_PRINTLN("I2SCamera::encodeFrameToJPEG: no JPEG encoder available");
    return false;
  }

  if (!outBuffer || !outLen) {
    DEBUG_PRINTLN("I2SCamera::encodeFrameToJPEG: invalid output buffer");
    return false;
  }

  return JPEGEncoderWrapper::encode(frame, xres, yres, quality, outBuffer, outLen);
}
