#include "I2SCamera.h"
#include "Config.h"
#include "Log.h"
#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#define STREAM_LINES 16
#define STREAM_BLOCKS 4
#define CAMERA_CAPTURE_TIMEOUT 2000

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
DMABuffer**I2SCamera::dmaBuffer=0;
unsigned char*I2SCamera::frame=0;
int I2SCamera::framePointer=0;
int I2SCamera::frameBytes=0;
volatile bool I2SCamera::stopSignal=false;

static uint8_t*streamBlock[STREAM_BLOCKS]={0};
static volatile uint8_t streamState[STREAM_BLOCKS]={0};
static volatile int streamBlockY[STREAM_BLOCKS]={0};
static volatile int streamFill=0,streamLine=0,streamReady=0;
static volatile int readyQueue[STREAM_BLOCKS]={0};
static volatile int readyHead=0,readyTail=0;
static volatile bool streamFrameDone=false,streamError=false;

static volatile uint32_t diagBytes=0;
static volatile uint32_t diagNonZero=0;
static volatile uint8_t diagMin=255;
static volatile uint8_t diagMax=0;
static volatile uint8_t diagRaw[16]={0};
static volatile uint16_t diagRGB[8]={0};
static volatile bool diagCaptured=false;

static void cameraI2SConfig(){
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
}

void IRAM_ATTR I2SCamera::i2sInterrupt(void*arg){
    I2S0.int_clr.val=I2S0.int_raw.val;
    if(streamError||streamFrameDone)return;
    if(!dmaBuffer||dmaBufferCount<=0){streamError=true;return;}

    DMABuffer*d=dmaBuffer[dmaBufferActive];
    if(!d||!d->buffer){streamError=true;return;}

    unsigned char*buf=d->buffer;
    dmaBufferActive=(dmaBufferActive+1)%dmaBufferCount;

    if(!diagCaptured){
        for(int i=0;i<16;i++)diagRaw[i]=buf[i];
        for(int i=0;i<8;i++)diagRGB[i]=(uint16_t)(((uint16_t)buf[i*4+2]<<8)|buf[i*4]);
        diagCaptured=true;
    }

    int block=streamFill;
    uint8_t*dst=streamBlock[block];
    if(!dst){streamError=true;return;}

    int p=streamLine*xres*2;

for(int i=0;i<xres*4;i+=4){
    uint8_t b0=buf[i];
    uint8_t b1=buf[i+1];

    dst[p++]=b0;
    dst[p++]=b1;

    diagBytes+=2;
    if(b0||b1)diagNonZero++;
    if(b0<diagMin)diagMin=b0;
    if(b1<diagMin)diagMin=b1;
    if(b0>diagMax)diagMax=b0;
    if(b1>diagMax)diagMax=b1;
}

    streamLine++;
    blocksReceived++;

    if(streamLine>=STREAM_LINES){
        streamBlockY[block]=blocksReceived-STREAM_LINES;
        streamState[block]=2;

        if(streamReady<STREAM_BLOCKS){
            readyQueue[readyTail]=block;
            readyTail=(readyTail+1)%STREAM_BLOCKS;
            streamReady++;
        }else{
            streamError=true;
            return;
        }

        streamLine=0;

        if(blocksReceived>=yres){
            framesReceived++;
            streamFrameDone=true;
            return;
        }

        int next=(block+1)%STREAM_BLOCKS;
        if(streamState[next]!=0){
            streamError=true;
            return;
        }

        streamFill=next;
        streamState[next]=1;
    }
}

void IRAM_ATTR I2SCamera::vSyncInterrupt(void*arg){
    GPIO.status1_w1tc.val=GPIO.status1.val;
    GPIO.status_w1tc=GPIO.status;
}

void I2SCamera::i2sStop(){
    if(i2sInterruptHandle)esp_intr_disable(i2sInterruptHandle);
    if(vSyncInterruptHandle)esp_intr_disable(vSyncInterruptHandle);
    I2S0.conf.rx_start=0;
    i2sConfReset();
}

void I2SCamera::i2sRun(){
    i2sStop();
    cameraI2SConfig();

    blocksReceived=0;
    dmaBufferActive=0;
    framePointer=0;
    streamFill=0;
    streamLine=0;
    streamReady=0;
    readyHead=0;
    readyTail=0;
    streamFrameDone=false;
    streamError=false;

    diagBytes=0;
    diagNonZero=0;
    diagMin=255;
    diagMax=0;
    diagCaptured=false;
    memset((void*)diagRaw,0,sizeof(diagRaw));
    memset((void*)diagRGB,0,sizeof(diagRGB));

    for(int i=0;i<STREAM_BLOCKS;i++){
        streamState[i]=0;
        streamBlockY[i]=0;
    }

    streamState[0]=1;

    if(!dmaBuffer||dmaBufferCount<=0||!dmaBuffer[0]||!dmaBuffer[0]->valid()){
        streamError=true;
        return;
    }

    uint32_t t=millis();

    while(gpio_get_level(vSyncPin)==0){
        if(millis()-t>500){
            streamError=true;
            return;
        }
        delay(1);
    }

    t=millis();

    while(gpio_get_level(vSyncPin)!=0){
        if(millis()-t>500){
            streamError=true;
            return;
        }
        delay(1);
    }

    I2S0.rx_eof_num=dmaBuffer[0]->sampleCount();
    I2S0.in_link.addr=(uint32_t)&dmaBuffer[0]->descriptor;
    I2S0.in_link.start=1;
    I2S0.int_clr.val=I2S0.int_raw.val;
    I2S0.int_ena.val=0;
    I2S0.int_ena.in_done=1;

    if(!i2sInterruptHandle||!vSyncInterruptHandle){
        streamError=true;
        return;
    }

    esp_intr_enable(i2sInterruptHandle);
    esp_intr_enable(vSyncInterruptHandle);
    I2S0.conf.rx_start=1;
}

bool I2SCamera::initVSync(int pin){
    vSyncPin=(gpio_num_t)pin;
    gpio_set_intr_type(vSyncPin,GPIO_INTR_POSEDGE);

    if(gpio_isr_register(
        &I2SCamera::vSyncInterrupt,
        (void*)"vSyncInterrupt",
        ESP_INTR_FLAG_INTRDISABLED|ESP_INTR_FLAG_IRAM,
        &vSyncInterruptHandle)!=ESP_OK)return false;

    gpio_intr_enable(vSyncPin);
    return true;
}

void I2SCamera::deinitVSync(){
    if(vSyncInterruptHandle){
        esp_intr_disable(vSyncInterruptHandle);
        esp_intr_free(vSyncInterruptHandle);
        vSyncInterruptHandle=0;
    }
}

void I2SCamera::deinit(){
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
        esp_intr_free(i2sInterruptHandle);
        i2sInterruptHandle=0;
    }

    if(vSyncInterruptHandle){
        esp_intr_free(vSyncInterruptHandle);
        vSyncInterruptHandle=0;
    }

    framePointer=0;
    frameBytes=0;
    dmaBufferActive=0;
    blocksReceived=0;
    streamFill=0;
    streamLine=0;
    streamReady=0;
    readyHead=0;
    readyTail=0;
    streamFrameDone=false;
    streamError=false;
}

bool I2SCamera::init(
    const int XRES,const int YRES,
    const int VSYNC,const int HREF,const int XCLK,const int PCLK,
    const int D0,const int D1,const int D2,const int D3,
    const int D4,const int D5,const int D6,const int D7){

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

    for(int i=0;i<STREAM_BLOCKS;i++){
        streamBlock[i]=(uint8_t*)malloc(blockBytes);

        if(!streamBlock[i]){
            DEBUG_PRINTLN("TARS: stream buffer allocation failed");
            deinit();
            return false;
        }
    }

    if(!i2sInit(VSYNC,HREF,PCLK,D0,D1,D2,D3,D4,D5,D6,D7)){
        deinit();
        return false;
    }

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
    const int D4,const int D5,const int D6,const int D7){

    int pins[]={VSYNC,HREF,PCLK,D0,D1,D2,D3,D4,D5,D6,D7};

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
    cameraI2SConfig();

    esp_err_t r=esp_intr_alloc(
        ETS_I2S0_INTR_SOURCE,
        ESP_INTR_FLAG_INTRDISABLED|ESP_INTR_FLAG_LEVEL1|ESP_INTR_FLAG_IRAM,
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

void I2SCamera::dmaBufferInit(int bytes){
    dmaBufferDeinit();

    dmaBufferCount=2;
    dmaBuffer=(DMABuffer**)malloc(sizeof(DMABuffer*)*dmaBufferCount);

    if(!dmaBuffer){
        dmaBufferCount=0;
        return;
    }

    for(int i=0;i<dmaBufferCount;i++){
        dmaBuffer[i]=new DMABuffer(bytes);

        if(!dmaBuffer[i]||!dmaBuffer[i]->valid()){
            dmaBufferDeinit();
            return;
        }

        if(i)dmaBuffer[i-1]->next(dmaBuffer[i]);
    }

    dmaBuffer[dmaBufferCount-1]->next(dmaBuffer[0]);
}

void I2SCamera::dmaBufferDeinit(){
    if(!dmaBuffer)return;

    for(int i=0;i<dmaBufferCount;i++){
        if(dmaBuffer[i])delete dmaBuffer[i];
    }

    free(dmaBuffer);
    dmaBuffer=0;
    dmaBufferCount=0;
}
void I2SCamera::dmaDiagnostic(){
    if(!dmaBuffer||dmaBufferCount<=0||!dmaBuffer[0]||!dmaBuffer[0]->buffer){
        return;
    }

    Serial.print("TARS: DMA RAW=");
    for(int i=0;i<32;i++){
        Serial.printf("%02X",dmaBuffer[0]->buffer[i]);
        if(i<31)Serial.print(" ");
    }
    Serial.println();
}
bool I2SCamera::encodeFrameToJPEG(uint8_t*outBuffer,size_t*outLen,int quality){
    if(!OV7670_ENABLE_JPEG||!outBuffer||!outLen)return false;
    *outLen=0;

    if(!JPEGEncoderWrapper::available())return false;

    if(!JPEGEncoderWrapper::begin(
        outBuffer,
        OV7670_MAX_JPEG_SIZE,
        xres,
        yres,
        quality)){
        i2sStop();
        return false;
    }

    i2sRun();

    if(streamError){
        i2sStop();
        DEBUG_PRINTLN("TARS: STREAM START ERROR");
        return false;
    }

    uint32_t started=millis();
    int encodedBlocks=0;

    while(!streamFrameDone&&!streamError){
        if(millis()-started>CAMERA_CAPTURE_TIMEOUT){
            streamError=true;
            break;
        }

        if(streamReady<=0){
            delay(1);
            continue;
        }

        int b=readyQueue[readyHead];
        readyHead=(readyHead+1)%STREAM_BLOCKS;

        if(streamState[b]!=2){
            streamError=true;
            break;
        }

        streamState[b]=3;

        if(!JPEGEncoderWrapper::addBlock(
            streamBlock[b],
            xres,
            STREAM_LINES)){
            streamError=true;
            break;
        }

        streamState[b]=0;

        if(streamReady>0)streamReady--;
        encodedBlocks++;
    }

    if(!streamError){
        while(streamReady>0){
            int b=readyQueue[readyHead];
            readyHead=(readyHead+1)%STREAM_BLOCKS;

            if(streamState[b]!=2){
                streamError=true;
                break;
            }

            streamState[b]=3;

            if(!JPEGEncoderWrapper::addBlock(
                streamBlock[b],
                xres,
                STREAM_LINES)){
                streamError=true;
                break;
            }

            streamState[b]=0;
            streamReady--;
            encodedBlocks++;
        }
    }

    i2sStop();

    if(streamError){
        DEBUG_PRINTLN("TARS: STREAM JPEG ERROR");
        return false;
    }

    if(!streamFrameDone){
        DEBUG_PRINTLN("TARS: STREAM FRAME TIMEOUT");
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

bool I2SCamera::capturePreview(uint8_t*out){
    if(!out||xres!=320||yres!=240){
        DEBUG_PRINTLN("TARS: PREVIEW INVALID ARG");
        return false;
    }

    memset(out,0,128*64);

    i2sRun();

    if(streamError){
        i2sStop();
        DEBUG_PRINTLN("TARS: PREVIEW STREAM START ERROR");
        return false;
    }

    uint32_t started=millis();
    int processedBlocks=0;

    while((!streamFrameDone||streamReady>0)&&!streamError){
        if(millis()-started>CAMERA_CAPTURE_TIMEOUT){
            streamError=true;
            break;
        }

        if(streamReady<=0){
            delay(1);
            continue;
        }

        int b=readyQueue[readyHead];
        readyHead=(readyHead+1)%STREAM_BLOCKS;

        if(streamState[b]!=2){
            streamError=true;
            break;
        }

        streamState[b]=3;

        int y0=streamBlockY[b];
        uint16_t*f=(uint16_t*)streamBlock[b];

        if(!f||y0<0||y0>=yres){
            streamError=true;
            break;
        }

        for(int oy=0;oy<64;oy++){
            int sy=oy*240/64;

            if(sy<y0||sy>=y0+STREAM_LINES)continue;

            int ly=sy-y0;

            for(int ox=0;ox<128;ox++){
                int sx=ox*320/128;
                uint16_t p=f[ly*320+sx];

                int r=(p>>11)&31;
                int g=(p>>5)&63;
                int bl=p&31;
                int gray=(r*255/31+g*255/63+bl*255/31)/3;

                if(gray>20)out[oy*128+ox]=1;
            }
        }

        streamState[b]=0;

        if(streamReady>0)streamReady--;
        processedBlocks++;
    }

    i2sStop();

    DEBUG_PRINT("TARS: CAM RAW ");

    for(int i=0;i<16;i++){
        char hex[3];
        sprintf(hex,"%02X",(unsigned)diagRaw[i]);
        DEBUG_PRINT(hex);
        if(i<15)DEBUG_PRINT(" ");
    }

    DEBUG_PRINTLN("");

    DEBUG_PRINT("TARS: CAM RGB565 ");

    for(int i=0;i<8;i++){
        char hex[5];
        sprintf(hex,"%04X",(unsigned)diagRGB[i]);
        DEBUG_PRINT(hex);
        if(i<7)DEBUG_PRINT(" ");
    }

    DEBUG_PRINTLN("");

    DEBUG_PRINT("TARS: CAM DATA bytes=");
    DEBUG_PRINT((unsigned long)diagBytes);
    DEBUG_PRINT(" nonzero=");
    DEBUG_PRINT((unsigned long)diagNonZero);
    DEBUG_PRINT(" min=");
    DEBUG_PRINT((int)diagMin);
    DEBUG_PRINT(" max=");
    DEBUG_PRINT((int)diagMax);
    DEBUG_PRINTLN("");

    bool ok=streamFrameDone&&!streamError&&processedBlocks==(yres/STREAM_LINES);

    DEBUG_PRINT("TARS: PREVIEW ");
    DEBUG_PRINT(ok?"OK":"FAIL");
    DEBUG_PRINT(" BLOCKS=");
    DEBUG_PRINT(processedBlocks);
    DEBUG_PRINT("/");
    DEBUG_PRINTLN(yres/STREAM_LINES);

    return ok;
}
