#include "I2SCamera.h"
#include "Config.h"
#include "Log.h"
#include "env.h"
#include <math.h>

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

#define STREAM_LINES 16
#define STREAM_BLOCKS 4
#define CAMERA_CAPTURE_TIMEOUT 2000

static uint8_t* streamBlock[STREAM_BLOCKS]={0};
static uint8_t previewFiltered[128*64];
static volatile uint8_t streamState[STREAM_BLOCKS]={0};
static volatile int streamBlockY[STREAM_BLOCKS]={0};
static volatile int streamFill=0,streamLine=0,streamReady=0;
static volatile int readyQueue[STREAM_BLOCKS]={0};
static volatile int readyHead=0,readyTail=0;
static volatile bool streamFrameDone=false,streamError=false;

static volatile uint8_t lastDominantColor=ENV_COLOR_UNKNOWN;
static volatile uint8_t lastColorConfidence=0;

// Penantian VSYNC dibatasi agar task tidak terkunci.
static bool waitVSync(int level,uint32_t timeout)
{
  uint32_t start=millis();

  while(gpio_get_level(I2SCamera::vSyncPin)!=level){
    if(millis()-start>=timeout)return false;
    delay(1);
  }

  return true;
}

void IRAM_ATTR I2SCamera::i2sInterrupt(void* arg)
{
  I2S0.int_clr.val=I2S0.int_raw.val;
  unsigned char* buf=dmaBuffer[dmaBufferActive]->buffer;
  dmaBufferActive=(dmaBufferActive+1)%dmaBufferCount;

  int idx=streamFill;
  if(idx<0||idx>=STREAM_BLOCKS)return;

  uint8_t* dst=streamBlock[idx];
  int line=streamLine;
  if(line>=STREAM_LINES)return;

  int p=line*xres*2;

  for(int i=0;i<xres*4;i+=4){
    dst[p++]=buf[i+2];
    dst[p++]=buf[i];
  }

  streamLine++;

  if(streamLine>=STREAM_LINES){
    streamBlockY[idx]=streamLine;
    streamState[idx]=1;
    readyQueue[readyHead]=idx;
    readyHead=(readyHead+1)%STREAM_BLOCKS;
    streamReady++;
    streamFill=(streamFill+1)%STREAM_BLOCKS;
    streamLine=0;
  }

  blocksReceived++;

  if(blocksReceived>=yres){
    blocksReceived=0;
    framesReceived++;
    streamFrameDone=true;

    // Hentikan penerimaan pada akhir frame tanpa
    // memanggil reset I2S dari dalam ISR.
    if(stopSignal){
      I2S0.conf.rx_start=0;
      stopSignal=false;
    }
  }
}

void IRAM_ATTR I2SCamera::vSyncInterrupt(void* arg)
{
  gpio_intr_disable(vSyncPin);

  if(gpio_get_level(vSyncPin)){
  }

  gpio_intr_enable(vSyncPin);
}

void I2SCamera::i2sStop()
{
  if(i2sInterruptHandle)
    esp_intr_disable(i2sInterruptHandle);

  if(vSyncInterruptHandle)
    esp_intr_disable(vSyncInterruptHandle);

  I2S0.conf.rx_start=0;
  i2sConfReset();
}

bool I2SCamera::i2sRunChecked()
{
  DEBUG_PRINTLN("I2S Run");

  if(!dmaBuffer||dmaBufferCount<=0||!i2sInterruptHandle){
    DEBUG_PRINTLN("I2S DMA NOT READY");
    return false;
  }

  if(!waitVSync(1,50)||!waitVSync(0,50)){
    DEBUG_PRINTLN("I2S VSYNC TIMEOUT");
    return false;
  }

  esp_intr_disable(i2sInterruptHandle);

  if(!i2sConfReset()){
    DEBUG_PRINTLN("I2S RESET TIMEOUT");
    return false;
  }

  blocksReceived=0;
  dmaBufferActive=0;
  framePointer=0;

  DEBUG_PRINT("Sample count ");
  DEBUG_PRINTLN(dmaBuffer[0]->sampleCount());

  I2S0.rx_eof_num=dmaBuffer[0]->sampleCount();
  I2S0.in_link.addr=(uint32_t)&(dmaBuffer[0]->descriptor);
  I2S0.in_link.start=1;
  I2S0.int_clr.val=I2S0.int_raw.val;
  I2S0.int_ena.val=0;
  I2S0.int_ena.in_done=1;

  esp_intr_enable(i2sInterruptHandle);
  esp_intr_enable(vSyncInterruptHandle);
  I2S0.conf.rx_start=1;

  return true;
}

// Tetap void agar kompatibel dengan pemanggil lama.
void I2SCamera::i2sRun()
{
  (void)i2sRunChecked();
}

bool I2SCamera::initVSync(int pin)
{
  DEBUG_PRINT("Initializing VSYNC... ");

  vSyncPin=(gpio_num_t)pin;
  gpio_set_intr_type(vSyncPin,GPIO_INTR_POSEDGE);

  if(gpio_isr_register(
       &I2SCamera::vSyncInterrupt,
       (void*)"vSyncInterrupt",
       ESP_INTR_FLAG_INTRDISABLED|ESP_INTR_FLAG_IRAM,
       &vSyncInterruptHandle)!=ESP_OK)
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
  if(vSyncInterruptHandle){
    esp_intr_disable(vSyncInterruptHandle);
    esp_intr_free(vSyncInterruptHandle);
    vSyncInterruptHandle=0;
  }
}

void I2SCamera::deinit()
{
  i2sStop();

  for(int i=0;i<STREAM_BLOCKS;i++){
    if(streamBlock[i]){
      free(streamBlock[i]);
      streamBlock[i]=nullptr;
    }
  }

  dmaBufferDeinit();

  if(frame){
    free(frame);
    frame=nullptr;
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
}

void I2SCamera::dmaBufferDeinit()
{
  if(!dmaBuffer)return;

  for(int i=0;i<dmaBufferCount;i++){
    if(dmaBuffer[i]){
      delete dmaBuffer[i];
      dmaBuffer[i]=nullptr;
    }
  }

  free(dmaBuffer);
  dmaBuffer=nullptr;
  dmaBufferCount=0;
  dmaBufferActive=0;
}

bool I2SCamera::init(
  const int XRES,const int YRES,
  const int VSYNC,const int HREF,
  const int XCLK,const int PCLK,
  const int D0,const int D1,const int D2,const int D3,
  const int D4,const int D5,const int D6,const int D7)
{
  xres=XRES;
  yres=YRES;
  frame=nullptr;
  frameBytes=0;

  i2sInit(VSYNC,HREF,PCLK,D0,D1,D2,D3,D4,D5,D6,D7);
  dmaBufferInit(xres*2*2);
  initVSync(VSYNC);

  for(int i=0;i<STREAM_BLOCKS;i++){
    streamBlock[i]=(uint8_t*)malloc(XRES*STREAM_LINES*2);

    if(!streamBlock[i]){
      DEBUG_PRINTLN("STREAM BLOCK ALLOC FAIL");
      return false;
    }
  }

  return true;
}

bool I2SCamera::i2sInit(
  const int VSYNC,const int HREF,const int PCLK,
  const int D0,const int D1,const int D2,const int D3,
  const int D4,const int D5,const int D6,const int D7)
{
  int pins[]={VSYNC,HREF,PCLK,D0,D1,D2,D3,D4,D5,D6,D7};

  gpio_config_t conf={
    .pin_bit_mask=0,
    .mode=GPIO_MODE_INPUT,
    .pull_up_en=GPIO_PULLUP_DISABLE,
    .pull_down_en=GPIO_PULLDOWN_DISABLE,
    .intr_type=GPIO_INTR_DISABLE
  };

  for(int i=0;i<sizeof(pins)/sizeof(pins[0]);++i){
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
  I2S0.fifo_conf.rx_fifo_mod=1;
  I2S0.fifo_conf.rx_fifo_mod_force_en=1;
  I2S0.conf_chan.rx_chan_mod=1;

  I2S0.sample_rate_conf.rx_bits_mod=16;
  I2S0.conf.rx_right_first=0;
  I2S0.conf.rx_msb_right=0;
  I2S0.conf.rx_msb_shift=0;
  I2S0.conf.rx_mono=0;
  I2S0.conf.rx_short_sync=0;
  I2S0.timing.val=0;

  esp_err_t _res=esp_intr_alloc(
    ETS_I2S0_INTR_SOURCE,
    ESP_INTR_FLAG_INTRDISABLED|ESP_INTR_FLAG_LEVEL1|ESP_INTR_FLAG_IRAM,
    &I2SCamera::i2sInterrupt,
    NULL,
    &i2sInterruptHandle
  );

  if(_res!=ESP_OK){
    DEBUG_PRINTLN("Failed to allocate I2S interrupt");
    return false;
  }

  return true;
}

void I2SCamera::dmaBufferInit(int bytes)
{
  dmaBufferCount=2;
  dmaBuffer=(DMABuffer**)malloc(sizeof(DMABuffer*)*dmaBufferCount);

  for(int i=0;i<dmaBufferCount;i++){
    dmaBuffer[i]=new DMABuffer(bytes);

    if(i)
      dmaBuffer[i-1]->next(dmaBuffer[i]);
  }

  dmaBuffer[dmaBufferCount-1]->next(dmaBuffer[0]);
}

bool I2SCamera::encodeFrameToJPEG(
  uint8_t* outBuffer,size_t* outLen,int quality)
{
  if(!outBuffer||!outLen)return false;

  streamFill=0;
  streamLine=0;
  streamReady=0;
  readyHead=0;
  readyTail=0;
  streamFrameDone=false;
  streamError=false;

  for(int i=0;i<STREAM_BLOCKS;i++)
    streamState[i]=0;

  if(!JPEGEncoderWrapper::begin(
       outBuffer,OV7670_MAX_JPEG_SIZE,xres,yres,quality))
    return false;

  if(!i2sRunChecked()){
    JPEGEncoderWrapper::finish(outLen);
    return false;
  }

  const int blocksNeeded=(yres+STREAM_LINES-1)/STREAM_LINES;
  int blocksDone=0;
  uint32_t start=millis();

  while(blocksDone<blocksNeeded){
    if(millis()-start>CAMERA_CAPTURE_TIMEOUT){
      i2sStop();
      JPEGEncoderWrapper::finish(outLen);
      return false;
    }

    if(streamReady<=0){
      delay(1);
      continue;
    }

    int idx=readyQueue[readyTail];
    readyTail=(readyTail+1)%STREAM_BLOCKS;
    streamReady--;

    if(!JPEGEncoderWrapper::addBlock(streamBlock[idx],xres,STREAM_LINES)){
      i2sStop();
      JPEGEncoderWrapper::finish(outLen);
      return false;
    }

    streamState[idx]=0;
    blocksDone++;
    start=millis();
    vTaskDelay(1);
  }

  i2sStop();
  return JPEGEncoderWrapper::finish(outLen);
}
uint8_t I2SCamera::dominantColor(){
  return lastDominantColor;
}

uint8_t I2SCamera::dominantColorConfidence(){
  return lastColorConfidence;
}
static uint8_t classifyRGB565(uint16_t p){
  uint8_t r=((p>>11)&0x1F)*255/31;
  uint8_t g=((p>>5)&0x3F)*255/63;
  uint8_t b=(p&0x1F)*255/31;

  uint8_t mx=max(r,max(g,b));
  uint8_t mn=min(r,min(g,b));
  uint8_t delta=mx-mn;

  if(mx<35)return ENV_COLOR_BLACK;
  if(mn>220&&delta<35)return ENV_COLOR_WHITE;
  if(delta<25)return ENV_COLOR_GRAY;

  float h=0;

  if(mx==r)
    h=60.0f*((float)g-b)/delta;
  else if(mx==g)
    h=60.0f*((float)b-r)/delta+120.0f;
  else
    h=60.0f*((float)r-g)/delta+240.0f;

  if(h<0)h+=360.0f;

  if(h<15||h>=345)return ENV_COLOR_RED;
  if(h<40)return ENV_COLOR_ORANGE;
  if(h<70)return ENV_COLOR_YELLOW;
  if(h<165)return ENV_COLOR_GREEN;
  if(h<200)return ENV_COLOR_CYAN;
  if(h<255)return ENV_COLOR_BLUE;
  return ENV_COLOR_PURPLE;
}

bool I2SCamera::capturePreview(uint8_t*out)
{
  if(!out)return false;

  lastDominantColor=ENV_COLOR_UNKNOWN;
  lastColorConfidence=0;

  memset(out,0,128*64);

  uint32_t colorCount[11]={};
  uint32_t colorSamples=0;

  streamFill=0;
  streamLine=0;
  streamReady=0;
  readyHead=0;
  readyTail=0;
  streamFrameDone=false;
  streamError=false;

  for(int i=0;i<STREAM_BLOCKS;i++)
    streamState[i]=0;

  if(!i2sRunChecked())return false;

  const int blocksNeeded=(yres+STREAM_LINES-1)/STREAM_LINES;
  int blocksDone=0;
  uint32_t start=millis();

  while(blocksDone<blocksNeeded){
    if(millis()-start>CAMERA_CAPTURE_TIMEOUT){
      i2sStop();
      return false;
    }

    if(streamReady<=0){
      delay(1);
      continue;
    }

    int idx=readyQueue[readyTail];
    readyTail=(readyTail+1)%STREAM_BLOCKS;
    streamReady--;

    uint16_t* src=(uint16_t*)streamBlock[idx];
    int blockY=blocksDone*STREAM_LINES;

    for(int y=0;y<STREAM_LINES;y++){
      if((y&3)==0)vTaskDelay(1);

      int sourceY=blockY+y;
      int oy=sourceY*64/yres;

      if(oy>=64)continue;

      for(int x=0;x<xres;x++){
        uint16_t p=src[y*xres+x];

        // Sampel warna merata di seluruh frame
        if((sourceY&3)==0&&(x&3)==0){
          uint8_t color=classifyRGB565(p);

          if(color<11){
            colorCount[color]++;
            colorSamples++;
          }
        }

        // Preview OLED tetap grayscale seperti sebelumnya
        int ox=x*128/xres;
        if(ox>=128)continue;

        uint8_t r=((p>>11)&0x1F)*255/31;
        uint8_t g=((p>>5)&0x3F)*255/63;
        uint8_t b=(p&0x1F)*255/31;

        uint8_t gray=(uint8_t)(
          (77*r+150*g+29*b)>>8
        );

        if(gray<=47)
          out[oy*128+ox]=1;
      }
    }

    streamState[idx]=0;
    blocksDone++;
    start=millis();
  }

  i2sStop();

  // Cari warna dengan jumlah sampel terbanyak
  if(colorSamples){
    uint8_t bestColor=ENV_COLOR_UNKNOWN;
    uint16_t bestCount=0;

    for(uint8_t i=1;i<11;i++){
      if(colorCount[i]>bestCount){
        bestCount=colorCount[i];
        bestColor=i;
      }
    }

    lastDominantColor=bestColor;
    lastColorConfidence=
      (uint32_t)bestCount*100/colorSamples;
  }

  // MEDIAN FILTER 3x3: pertahankan detail gelap
  // dan kurangi noise preview OLED
  for(int y=0;y<64;y++){
    for(int x=0;x<128;x++){
      int count=0,total=0;

      for(int dy=-1;dy<=1;dy++){
        for(int dx=-1;dx<=1;dx++){
          int nx=x+dx,ny=y+dy;

          if(nx<0||nx>=128||ny<0||ny>=64)
            continue;

          count+=out[ny*128+nx];
          total++;
        }
      }
      previewFiltered[y*128+x]=
        (count>=total/2+1)?1:0;
    }
    if((y&3)==0)vTaskDelay(1);
  }
  memcpy(out,previewFiltered,128*64);
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
