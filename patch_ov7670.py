Import("env")
import os

# ============================================================
# TARS - OV7670 CLEANUP PATCH
# ESP32-OV7670-no-FIFO
#
# Tujuan:
# - Compatibility gpio_matrix_in
# - Tetap I2S0
# - Cleanup frame + DMA + interrupt saat delete camera
# - Aman camera OFF -> ON
# - Tidak mengubah resolusi QQVGA 160x120
# ============================================================

lib_dir = os.path.join(
    env.subst("$PROJECT_LIBDEPS_DIR"),
    env.subst("$PIOENV"),
    "ESP32-OV7670-no-FIFO",
    "src"
)

cpp_file = os.path.join(lib_dir, "I2SCamera.cpp")
h_file = os.path.join(lib_dir, "I2SCamera.h")

print("")
print("========== TARS OV7670 CLEANUP PATCH ==========")
print("LIB :", lib_dir)

if not os.path.isfile(cpp_file):
    print("TARS ERROR: I2SCamera.cpp not found")
    env.Exit(1)

if not os.path.isfile(h_file):
    print("TARS ERROR: I2SCamera.h not found")
    env.Exit(1)

print("TARS: I2SCamera.cpp FOUND")
print("TARS: size =", os.path.getsize(cpp_file), "bytes")


# ============================================================
# READ FILES
# ============================================================

with open(h_file, "r", encoding="utf-8") as f:
    hdata = f.read()

with open(cpp_file, "r", encoding="utf-8") as f:
    data = f.read()


# ============================================================
# gpio_matrix_in compatibility
# ============================================================

old_gpio_decl = "void gpio_matrix_in(int gpio, int signal_index, bool inverted);"

if old_gpio_decl in hdata:
    hdata = hdata.replace(old_gpio_decl, "")

    with open(h_file, "w", encoding="utf-8") as f:
        f.write(hdata)

    print("TARS: gpio_matrix_in conflict patched")
else:
    print("TARS: gpio_matrix_in patch not needed")


# ============================================================
# Helper
# ============================================================

def replace_function(source, signature, replacement):
    start = source.find(signature)

    if start < 0:
        return source, False

    brace = source.find("{", start)

    if brace < 0:
        return source, False

    depth = 0
    end = -1

    for i in range(brace, len(source)):

        if source[i] == "{":
            depth += 1

        elif source[i] == "}":
            depth -= 1

            if depth == 0:
                end = i + 1
                break

    if end < 0:
        return source, False

    return source[:start] + replacement + source[end:], True


# ============================================================
# ADD DESTRUCTOR
# ============================================================

destructor = "  ~I2SCamera(){deinit();}\n"

if "~I2SCamera()" not in hdata:

    marker = "class I2SCamera\n{\npublic:\n"

    if marker in hdata:
        hdata = hdata.replace(
            marker,
            marker + destructor,
            1
        )

        print("TARS: I2SCamera destructor added")
    else:
        print("TARS ERROR: I2SCamera class marker not found")
        env.Exit(1)

else:
    print("TARS: I2SCamera destructor already present")


# ============================================================
# SAFE I2S STOP
# ============================================================

new_i2s_stop = r'''void I2SCamera::i2sStop()
{
    if(i2sInterruptHandle)
        esp_intr_disable(i2sInterruptHandle);

    if(vSyncInterruptHandle)
        esp_intr_disable(vSyncInterruptHandle);

    i2sConfReset();
    I2S0.conf.rx_start = 0;
}'''

data, ok = replace_function(
    data,
    "void I2SCamera::i2sStop()",
    new_i2s_stop
)

print(
    "TARS: i2sStop() patched"
    if ok
    else
    "TARS ERROR: i2sStop() not found"
)


# ============================================================
# SAFE DMA INIT
# ============================================================

new_dma_init = r'''void I2SCamera::dmaBufferInit(int bytes)
{
    dmaBufferDeinit();

    dmaBufferCount = 2;

    dmaBuffer = (DMABuffer**)malloc(
        sizeof(DMABuffer*) * dmaBufferCount
    );

    if(!dmaBuffer){
        dmaBufferCount = 0;
        return;
    }

    for(int i=0;i<dmaBufferCount;i++){
        dmaBuffer[i] = new DMABuffer(bytes);

        if(!dmaBuffer[i]){
            dmaBufferDeinit();
            return;
        }

        if(i)
            dmaBuffer[i-1]->next(dmaBuffer[i]);
    }

    dmaBuffer[dmaBufferCount-1]->next(dmaBuffer[0]);
}'''

data, ok = replace_function(
    data,
    "void I2SCamera::dmaBufferInit(int bytes)",
    new_dma_init
)

print(
    "TARS: dmaBufferInit() patched"
    if ok
    else
    "TARS ERROR: dmaBufferInit() not found"
)


# ============================================================
# SAFE DEINIT
# ============================================================

new_deinit = r'''void I2SCamera::deinit()
{
    i2sStop();

    dmaBufferDeinit();

    if(frame){
        free(frame);
        frame=nullptr;
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
}'''

data, ok = replace_function(
    data,
    "void I2SCamera::deinit()",
    new_deinit
)

print(
    "TARS: deinit() patched"
    if ok
    else
    "TARS ERROR: deinit() not found"
)


# ============================================================
# SAFE INIT
# ============================================================

new_init = r'''bool I2SCamera::init(const int XRES, const int YRES, const int VSYNC,
                     const int HREF, const int XCLK, const int PCLK,
                     const int D0, const int D1, const int D2, const int D3,
                     const int D4, const int D5, const int D6, const int D7)
{
    // Always clean stale camera resources first.
    deinit();

    xres = XRES;
    yres = YRES;
    frameBytes = XRES * YRES * 2;
    framePointer = 0;
    blocksReceived = 0;
    framesReceived = 0;
    dmaBufferActive = 0;
    stopSignal = false;

    frame = (unsigned char*)malloc(frameBytes);

    if(!frame){
        DEBUG_PRINTLN("Not enough memory for frame buffer!");
        frameBytes = 0;
        return false;
    }

    if(!i2sInit(
        VSYNC,HREF,PCLK,
        D0,D1,D2,D3,
        D4,D5,D6,D7
    )){
        DEBUG_PRINTLN("I2S initialization failed!");
        deinit();
        return false;
    }

    dmaBufferInit(xres * 2 * 2);

    if(!dmaBuffer){
        DEBUG_PRINTLN("DMA buffer initialization failed!");
        deinit();
        return false;
    }

    if(!initVSync(VSYNC)){
        DEBUG_PRINTLN("VSYNC initialization failed!");
        deinit();
        return false;
    }

    return true;
}'''

data, ok = replace_function(
    data,
    "bool I2SCamera::init(const int XRES",
    new_init
)

print(
    "TARS: init() patched"
    if ok
    else
    "TARS ERROR: init() not found"
)


# ============================================================
# WRITE FILES
# ============================================================

if ok:

    with open(h_file, "w", encoding="utf-8") as f:
        f.write(hdata)

    with open(cpp_file, "w", encoding="utf-8") as f:
        f.write(data)

    print("TARS: I2SCamera.h updated")
    print("TARS: I2SCamera.cpp updated")

else:

    print("TARS: source changes NOT written")
    env.Exit(1)


# ============================================================
# DONE
# ============================================================

print("")
print("========== TARS OV7670 CLEANUP DONE ==========")
print("gpio_matrix_in : patched/checked")
print("destructor     : enabled")
print("i2sStop()      : safe")
print("DMA cleanup    : enabled")
print("frame cleanup  : enabled")
print("ISR cleanup    : enabled")
print("init cleanup   : enabled")
print("CAMERA MODE    : QQVGA RGB565")
print("CAMERA SIZE    : 160x120")
print("================================================")
