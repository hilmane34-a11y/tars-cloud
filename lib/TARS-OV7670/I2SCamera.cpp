#include "I2SCamera.h"
#include "Config.h"
#include "Log.h"
#include <Arduino.h>
#include <stdlib.h>

#define STREAM_LINES 16
#define STREAM_BLOCKS 2
#define CAMERA_CAPTURE_TIMEOUT 3000

int I2SCamera::blocksReceived=0;
int I2SCamera::framesReceived=0;
int I2SCamera::xres=640;
int I2SCamera::yres=480;
I2SCamera::ImageFormat I2SCamera::imageFormat=I2SCamera::FORMAT_BMP;
gpio_num_t I2SCamera::vSyncPin=(gpio_num_t)0;
intr_handle_t I2SCamera::i2sInterruptHandle=0;
intr_handle_t I2SCamera::vSyncInterruptHandle=0;
int I2SCamera::dmaBufferCount=0;
int I2SCamera::dmaBufferActive=0;
DMABuffer **I2SCamera::dmaBuffer=0;
unsigned char* I2SCamera::frame=0;
int I2SCamera::framePointer=0;
int I2SCamera::frameBytes=0;
volatile bool I2SCamera::stopSignal=false;

static uint8_t* streamBlock[STREAM_BLOCKS]={0};
static volatile uint8_t streamState[STREAM_BLOCKS]={0};
static volatile int streamFill=0;
static volatile int streamLine=0;
static volatile int streamReady=0;
static volatile bool streamFrameDone=false;
static volatile bool streamError=false;

void IRAM_ATTR I2SCamera::i2sInterrupt(void* arg)
{
  I2S0.int_clr.val=I2S0.int_raw.val;
  if(streamError||streamFrameDone)return;
  if(!dmaBuffer||dmaBufferCount<=0)return;

  unsigned char* buf=dmaBuffer[dmaBufferActive]->buffer;
  dmaBufferActive=(dmaBufferActive+1)%dmaBufferCount;

  int block=streamFill;
  if(block<0||block>=STREAM_BLOCKS){
    streamError=true;
    return;
  }

  if(streamState[block]!=1){
    streamError=true;
    return;
  }

  uint8_t* dst=streamBlock[block];
  if(!dst){
    streamError=true;
    return;
  }

  int p=streamLine*xres*2;

  for(int i=0;i<xres*4;i+=4){
    dst[p++]=buf[i+2];
    dst[p++]=buf[i];
  }

  streamLine++;
  blocksReceived++;

  if(streamLine>=STREAM_LINES){
    streamLine=0;
    streamState[block]=2;
    streamReady++;

    if(blocksReceived>=yres){
      framesReceived++;
      streamFrameDone=true;
      return;
    }

    int next=(block+1)%STREAM_BLOCKS;

    /*
     * Buffer berikutnya harus sudah bebas.
     * Jika JPEG belum sempat mengambil buffer sebelumnya,
     * jangan pernah menimpanya.
     */
    if(streamState[next]!=0){
      streamError=true;
      return;
    }

    streamFill=next;
    streamState[next]=1;
  }
}

void IRAM_ATTR I2SCamera::vSyncInterrupt(void* arg)
{
  gpio_intr_disable(vSyncPin);
  gpio_intr_enable(vSyncPin);
}

void I2SCamera::i2sStop()
{
  if(i2sInterruptHandle)
    esp_intr_disable(i2sInterruptHandle);

  if(vSyncInterruptHandle)
    esp_intr_disable(vSyncInterruptHandle);

  i2sConfReset();
  I2S0.conf.rx_start=0;
}

void I2SCamera::i2sRun()
{
  i2sStop();

  blocksReceived=0;
  dmaBufferActive=0;
  framePointer=0;

  streamFill=0;
  streamLine=0;
  streamReady=0;
  streamFrameDone=false;
  streamError=false;

  for(int i=0;i<STREAM_BLOCKS;i++)
    streamState[i]=0;

  streamState[0]=1;

  while(gpio_get_level(vSyncPin)==0);
  while(gpio_get_level(vSyncPin)!=0);

  I2S0.rx_eof_num=dmaBuffer[0]->sampleCount();
  I2S0.in_link.addr=(uint32_t)&dmaBuffer[0]->descriptor;
  I2S0.in_link.start=1;
  I2S0.int_clr.val=I2S0.int_raw.val;
  I2S0.int_ena.val=0;
  I2S0.int_ena.in_done=1;

  if(i2sInterruptHandle)
    esp_intr_enable(i2sInterruptHandle);

  if(vSyncInterruptHandle)
    esp_intr_enable(vSyncInterruptHandle);

  I2S0.conf.rx_start=1;
}

bool I2SCamera::initVSync(int pin)
{
  vSyncPin=(gpio_num_t)pin;
  gpio_set_intr_type(vSyncPin,GPIO_INTR_POSEDGE);

  if(gpio_isr_register(
    &I2SCamera::vSyncInterrupt,
    (void*)"vSyncInterrupt",
    ESP_INTR_FLAG_INTRDISABLED|ESP_INTR_FLAG_IRAM,
    &vSyncInterruptHandle
  )!=ESP_OK)
    return false;

  gpio_intr_enable(vSyncPin);
  return true;
}

void I2SCamera::deinitVSync()
{
  if(vSyncInterruptHandle){
    esp_intr_disable(vSyncInterruptHandle);
    esp_intr_free(vSyncInterruptHandle);
    vSyncInterruptHandle=0;
  }
}

void I2SCamera::deinit()
{
  i2sStop();
  dmaBufferDeinit();

  for(int i=0;i<STREAM_BLOCKS;i++){
    if(streamBlock[i]){
      free(streamBlock[i]);
      streamBlock[i]=0;
    }
    streamState[i]=0;
  }

  if(frame){
    free(frame);
    frame=0;
  }

  if(i2sInterruptHandle){
    esp_intr_disable(i2sInterruptHandle);
    esp_intr_free(i2sInterruptHandle);
    i2sInterruptHandle=0;
  }

  if(vSyncInterruptHandle){
    esp_intr_disable(vSyncInterruptHandle);
    esp_intr_free(vSyncInterruptHandle);
    vSyncInterruptHandle=0;
  }

  framePointer=0;
  frameBytes=0;
  streamFill=0;
  streamLine=0;
  streamReady=0;
  streamFrameDone=false;
  streamError=false;
}

bool I2SCamera::init(
  const int XRES,const int YRES,
  const int VSYNC,const int HREF,const int XCLK,const int PCLK,
  const int D0,const int D1,const int D2,const int D3,
  const int D4,const int D5,const int D6,const int D7)
{
  deinit();

  xres=XRES;
  yres=YRES;
  frameBytes=0;
  frame=0;

  if(xres!=320||yres!=240){
    DEBUG_PRINTLN("TARS: streaming camera requires 320x240");
    return false;
  }

  const size_t blockBytes=(size_t)xres*STREAM_LINES*2;

  DEBUG_PRINT("TARS: STREAM BLOCK BYTES=");
  DEBUG_PRINTLN((unsigned)blockBytes);

  for(int i=0;i<STREAM_BLOCKS;i++){
    streamBlock[i]=(uint8_t*)malloc(blockBytes);

    if(!streamBlock[i]){
      DEBUG_PRINT("TARS: STREAM BLOCK ");
      DEBUG_PRINT(i);
      DEBUG_PRINTLN(" ALLOC FAILED");
      deinit();
      return false;
    }
  }

  if(!i2sInit(
    VSYNC,HREF,PCLK,
    D0,D1,D2,D3,D4,D5,D6,D7
  )){
    deinit();
    return false;
  }

  /*
   * 320 pixel × 2 byte × 2 =
   * 1280 byte per DMA buffer.
   */
  dmaBufferInit(xres*2*2);

  if(!dmaBuffer){
    deinit();
    return false;
  }

  if(!initVSync(VSYNC)){
    deinit();
    return false;
  }

  DEBUG_PRINT("TARS: STREAM CAMERA ");
  DEBUG_PRINT(xres);
  DEBUG_PRINT("x");
  DEBUG_PRINT(yres);
  DEBUG_PRINT(" BLOCK=");
  DEBUG_PRINT(STREAM_LINES);
  DEBUG_PRINT("x");
  DEBUG_PRINTLN(STREAM_BLOCKS);

  return true;
}

bool I2SCamera::i2sInit(
  const int VSYNC,const int HREF,const int PCLK,
  const int D0,const int D1,const int D2,const int D3,
  const int D4,const int D5,const int D6,const int D7)
{
  int pins[]={
    VSYNC,HREF,PCLK,
    D0,D1,D2,D3,D4,D5,D6,D7
  };

  gpio_config_t conf={
    .pin_bit_mask=0,
    .mode=GPIO_MODE_INPUT,
    .pull_up_en=GPIO_PULLUP_DISABLE,
    .pull_down_en=GPIO_PULLDOWN_DISABLE,
    .intr_type=GPIO_INTR_DISABLE
  };

  for(int i=0;i<(int)(sizeof(pins)/sizeof(pins[0]));i++){
    conf.pin_bit_mask=1ULL<<pins[i];
    gpio_config(&conf);
  }

  gpio_matrix_in(D0,I2S0I_DATA_IN0_IDX,false);
  gpio_matrix_in(D1,I2S0I_DATA_IN1_IDX,false);
  gpio_matrix_in(D2,I2S0I_DATA_IN2_IDX,false);
  gpio_matrix_in(D3,I2S0I_DATA_IN3_IDX,false);
  gpio_matrix_in(D4,I2S0I_DATA_IN4_IDX,false);
  gpio_matrix_in(D5,I2S0I_DATA_IN5_IDX,false);
  gpio_matrix_in(D6,I2S0I_DATA_IN6_IDX,false);
  gpio_matrix_in(D7,I2S0I_DATA_IN7_IDX,false);

  gpio_matrix_in(0x30,I2S0I_DATA_IN8_IDX,false);
  gpio_matrix_in(0x30,I2S0I_DATA_IN9_IDX,false);
  gpio_matrix_in(0x30,I2S0I_DATA_IN10_IDX,false);
  gpio_matrix_in(0x30,I2S0I_DATA_IN11_IDX,false);
  gpio_matrix_in(0x30,I2S0I_DATA_IN12_IDX,false);
  gpio_matrix_in(0x30,I2S0I_DATA_IN13_IDX,false);
  gpio_matrix_in(0x30,I2S0I_DATA_IN14_IDX,false);
  gpio_matrix_in(0x30,I2S0I_DATA_IN15_IDX,false);

  gpio_matrix_in(VSYNC,I2S0I_V_SYNC_IDX,true);
  gpio_matrix_in(0x38,I2S0I_H_SYNC_IDX,false);
  gpio_matrix_in(HREF,I2S0I_H_ENABLE_IDX,false);
  gpio_matrix_in(PCLK,I2S0I_WS_IN_IDX,false);

  periph_module_enable(PERIPH_I2S0_MODULE);
  i2sConfReset();

  I2S0.conf.rx_slave_mod=1;
  I2S0.conf2.lcd_en=1;
  I2S0.conf2.camera_en=1;

  I2S0.clkm_conf.clkm_div_a=1;
  I2S0.clkm_conf.clkm_div_b=0;
  I2S0.clkm_conf.clkm_div_num=2;

  I2S0.fifo_conf.dscr_en=1;
  I2S0.fifo_conf.rx_fifo_mod=0;
  I2S0.fifo_conf.rx_fifo_mod_force_en=1;
  I2S0.conf_chan.rx_chan_mod=1;

  I2S0.sample_rate_conf.rx_bits_mod=0;

  I2S0.conf.rx_right_first=0;
  I2S0.conf.rx_msb_right=0;
  I2S0.conf.rx_msb_shift=0;
  I2S0.conf.rx_mono=0;
  I2S0.conf.rx_short_sync=0;
  I2S0.timing.val=0;

  esp_err_t r=esp_intr_alloc(
    ETS_I2S0_INTR_SOURCE,
    ESP_INTR_FLAG_INTRDISABLED|
    ESP_INTR_FLAG_LEVEL1|
    ESP_INTR_FLAG_IRAM,
    &I2SCamera::i2sInterrupt,
    NULL,
    &i2sInterruptHandle
  );

  if(r!=ESP_OK){
    DEBUG_PRINTLN("TARS: I2S interrupt allocation failed");
    return false;
  }

  return true;
}

void I2SCamera::dmaBufferInit(int bytes)
{
  dmaBufferDeinit();

  dmaBufferCount=2;

  dmaBuffer=(DMABuffer**)malloc(sizeof(DMABuffer*)*dmaBufferCount);

  if(!dmaBuffer){
    dmaBufferCount=0;
    return;
  }

  for(int i=0;i<dmaBufferCount;i++){
    dmaBuffer[i]=new DMABuffer(bytes);

    if(!dmaBuffer[i]){
      dmaBufferDeinit();
      return;
    }

    if(i)
      dmaBuffer[i-1]->next(dmaBuffer[i]);
  }

  dmaBuffer[dmaBufferCount-1]->next(dmaBuffer[0]);
}

void I2SCamera::dmaBufferDeinit()
{
  if(!dmaBuffer)
    return;

  for(int i=0;i<dmaBufferCount;i++){
    if(dmaBuffer[i]){
      delete dmaBuffer[i];
      dmaBuffer[i]=0;
    }
  }

  free(dmaBuffer);
  dmaBuffer=0;
  dmaBufferCount=0;
  dmaBufferActive=0;
}

bool I2SCamera::encodeFrameToJPEG(
  uint8_t* outBuffer,
  size_t* outLen,
  int quality)
{
  if(!OV7670_ENABLE_JPEG||!outBuffer||!outLen)
    return false;

  if(!JPEGEncoderWrapper::available())
    return false;

  streamFill=0;
  streamLine=0;
  streamReady=0;
  streamFrameDone=false;
  streamError=false;

  for(int i=0;i<STREAM_BLOCKS;i++)
    streamState[i]=0;

  if(!JPEGEncoderWrapper::begin(
    outBuffer,
    OV7670_MAX_JPEG_SIZE,
    xres,
    yres,
    quality
  ))
    return false;

  i2sRun();

  int encodedBlocks=0;
  uint32_t start=millis();

  while(!streamFrameDone&&!streamError){
    int b=-1;

    for(int i=0;i<STREAM_BLOCKS;i++){
      if(streamState[i]==2){
        b=i;
        break;
      }
    }

    if(b<0){
      if(millis()-start>CAMERA_CAPTURE_TIMEOUT){
        streamError=true;
        break;
      }
      delay(1);
      continue;
    }

    streamState[b]=3;

    if(!JPEGEncoderWrapper::addBlock(
      streamBlock[b],
      xres,
      STREAM_LINES
    )){
      streamError=true;
      break;
    }

    /*
     * Baru sekarang ISR boleh memakai kembali buffer.
     */
    streamState[b]=0;

    if(streamReady>0)
      streamReady--;

    encodedBlocks++;
    start=millis();
  }

  /*
   * Frame bisa selesai tepat ketika block terakhir
   * belum sempat dikonsumsi oleh loop utama.
   */
  for(int pass=0;pass<STREAM_BLOCKS;pass++){
    int b=-1;

    for(int i=0;i<STREAM_BLOCKS;i++){
      if(streamState[i]==2){
        b=i;
        break;
      }
    }

    if(b<0)
      break;

    streamState[b]=3;

    if(!JPEGEncoderWrapper::addBlock(
      streamBlock[b],
      xres,
      STREAM_LINES
    )){
      streamError=true;
      break;
    }

    streamState[b]=0;

    if(streamReady>0)
      streamReady--;

    encodedBlocks++;
  }

  i2sStop();

  if(streamError){
    JPEGEncoderWrapper::finish(outLen);
    DEBUG_PRINTLN("TARS: STREAM JPEG ERROR");
    return false;
  }

  if(!streamFrameDone){
    JPEGEncoderWrapper::finish(outLen);
    DEBUG_PRINTLN("TARS: STREAM FRAME TIMEOUT");
    return false;
  }

  if(encodedBlocks!=(yres/STREAM_LINES)){
    JPEGEncoderWrapper::finish(outLen);
    DEBUG_PRINT("TARS: STREAM BLOCK COUNT ERROR=");
    DEBUG_PRINTLN(encodedBlocks);
    return false;
  }

  if(!JPEGEncoderWrapper::finish(outLen)){
    DEBUG_PRINTLN("TARS: JPEG FINISH ERROR");
    return false;
  }

  DEBUG_PRINT("TARS: STREAM JPEG BLOCKS=");
  DEBUG_PRINT(encodedBlocks);
  DEBUG_PRINT(" SIZE=");
  DEBUG_PRINTLN((unsigned)*outLen);

  return true;
}

bool I2SCamera::capturePreview(uint8_t* out)
{
  if(!out)
    return false;

  memset(out,0,128*64);

  streamFill=0;
  streamLine=0;
  streamReady=0;
  streamFrameDone=false;
  streamError=false;

  for(int i=0;i<STREAM_BLOCKS;i++)
    streamState[i]=0;

  i2sRun();

  int blocksDone=0;
  const int blocksNeeded=(yres+STREAM_LINES-1)/STREAM_LINES;
  uint32_t start=millis();

  while(blocksDone<blocksNeeded&&!streamError){
    int b=-1;

    for(int i=0;i<STREAM_BLOCKS;i++){
      if(streamState[i]==2){
        b=i;
        break;
      }
    }

    if(b<0){
      if(millis()-start>CAMERA_CAPTURE_TIMEOUT){
        streamError=true;
        break;
      }
      delay(1);
      continue;
    }

    streamState[b]=3;

    uint16_t* src=(uint16_t*)streamBlock[b];
    int blockY=blocksDone*STREAM_LINES;

    for(int y=0;y<STREAM_LINES;y++){
      int oy=(blockY+y)*64/yres;
      if(oy>=64)
        continue;

      for(int x=0;x<xres;x++){
        int ox=x*128/xres;
        if(ox>=128)
          continue;

        uint16_t p=src[y*xres+x];

        uint8_t r=((p>>11)&0x1F)*255/31;
        uint8_t g=((p>>5)&0x3F)*255/63;
        uint8_t bl=(p&0x1F)*255/31;

        uint8_t gray=(uint8_t)((77*r+150*g+29*bl)>>8);

        if(gray>=45)
          out[oy*128+ox]=1;
      }
    }

    streamState[b]=0;

    if(streamReady>0)
      streamReady--;

    blocksDone++;
    start=millis();

    if((blocksDone&1)==0)
      vTaskDelay(1);
  }

  i2sStop();

  if(streamError||blocksDone<blocksNeeded){
    DEBUG_PRINTLN("TARS: PREVIEW FRAME ERROR");
    return false;
  }

  return true;
}

void I2SCamera::dmaDiagnostic()
{
  DEBUG_PRINT("TARS: DMA blocks=");
  DEBUG_PRINT(dmaBufferCount);
  DEBUG_PRINT(" active=");
  DEBUG_PRINT(dmaBufferActive);
  DEBUG_PRINT(" received=");
  DEBUG_PRINT(blocksReceived);
  DEBUG_PRINT(" frames=");
  DEBUG_PRINTLN(framesReceived);

  DEBUG_PRINT("TARS: STREAM ready=");
  DEBUG_PRINT(streamReady);
  DEBUG_PRINT(" fill=");
  DEBUG_PRINT(streamFill);
  DEBUG_PRINT(" line=");
  DEBUG_PRINTLN(streamLine);
}
