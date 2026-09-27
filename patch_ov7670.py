Import("env")

import os
import re

# ============================================================
# TARS - OV7670 PATCH
# ESP32-OV7670-no-FIFO
#
# Patch:
# 1. gpio_matrix_in compatibility
# 2. Proper I2S0/DMA/interrupt cleanup
# 3. Proper I2S0 peripheral release
# 4. Proper init() error handling
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
print("========== TARS OV7670 PATCH ==========")
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
# 1. PATCH HEADER gpio_matrix_in
# ============================================================

with open(h_file, "r", encoding="utf-8") as f:
    hdata = f.read()

old_gpio_decl = "void gpio_matrix_in(int gpio, int signal_index, bool inverted);"

if old_gpio_decl in hdata:
    hdata = hdata.replace(old_gpio_decl, "")

    with open(h_file, "w", encoding="utf-8") as f:
        f.write(hdata)

    print("TARS: gpio_matrix_in conflict patched")
else:
    print("TARS: gpio_matrix_in patch not needed")


# ============================================================
# 2. READ I2SCamera.cpp
# ============================================================

with open(cpp_file, "r", encoding="utf-8") as f:
    data = f.read()


# ============================================================
# Helper: replace complete C++ function body
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
# 3. PATCH deinit()
# ============================================================

new_deinit = r'''void I2SCamera::deinit()
{
    // Stop I2S0 RX/DMA immediately
    I2S0.conf.rx_start = 0;
    I2S0.in_link.start = 0;

    // Disable all I2S0 interrupts
    I2S0.int_ena.val = 0;
    I2S0.int_clr.val = I2S0.int_raw.val;

    // Disable camera / parallel mode
    I2S0.conf2.camera_en = 0;
    I2S0.conf2.lcd_en = 0;

    // Disable DMA descriptor mode
    I2S0.fifo_conf.dscr_en = 0;

    // Reset I2S0 configuration
    i2sConfReset();

    // Release DMA buffers
    dmaBufferDeinit();

    // Release frame buffer
    if (frame) {
        free(frame);
        frame = nullptr;
    }

    // Release I2S0 interrupt
    if (i2sInterruptHandle) {
        esp_intr_disable(i2sInterruptHandle);
        esp_intr_free(i2sInterruptHandle);
        i2sInterruptHandle = 0;
    }

    // Release VSYNC interrupt
    if (vSyncInterruptHandle) {
        esp_intr_disable(vSyncInterruptHandle);
        esp_intr_free(vSyncInterruptHandle);
        vSyncInterruptHandle = 0;
    }

    // IMPORTANT:
    // Camera directly enabled I2S0 peripheral,
    // therefore release it here before AudioTools/DAC uses I2S0.
    periph_module_disable(PERIPH_I2S0_MODULE);
}'''

data, deinit_ok = replace_function(
    data,
    "void I2SCamera::deinit()",
    new_deinit
)

if deinit_ok:
    print("TARS: deinit() patched")
else:
    print("TARS ERROR: deinit() function not found")


# ============================================================
# 4. PATCH init()
# ============================================================

new_init = r'''bool I2SCamera::init(const int XRES, const int YRES, const int VSYNC,
                     const int HREF, const int XCLK, const int PCLK,
                     const int D0, const int D1, const int D2, const int D3,
                     const int D4, const int D5, const int D6, const int D7)
{
    xres = XRES;
    yres = YRES;
    frameBytes = XRES * YRES * 2;

    frame = (unsigned char*)malloc(frameBytes);

    if (!frame) {
        DEBUG_PRINTLN("Not enough memory for frame buffer!");
        return false;
    }

    // Initialize I2S0.
    // IMPORTANT: check the result because esp_intr_alloc()
    // can fail when I2S0 resources are still occupied.
    if (!i2sInit(VSYNC, HREF, PCLK,
                 D0, D1, D2, D3, D4, D5, D6, D7)) {

        DEBUG_PRINTLN("I2S initialization failed!");

        free(frame);
        frame = nullptr;

        return false;
    }

    // Allocate DMA buffers
    dmaBufferInit(xres * 2 * 2);

    // Initialize VSYNC interrupt
    if (!initVSync(VSYNC)) {

        DEBUG_PRINTLN("VSYNC initialization failed!");

        deinit();

        return false;
    }

    return true;
}'''

data, init_ok = replace_function(
    data,
    "bool I2SCamera::init(const int XRES",
    new_init
)

if init_ok:
    print("TARS: init() patched")
else:
    print("TARS ERROR: init() function not found")


# ============================================================
# 5. WRITE ONLY IF CHANGES WERE MADE
# ============================================================

if deinit_ok or init_ok:
    with open(cpp_file, "w", encoding="utf-8") as f:
        f.write(data)

    print("TARS: I2SCamera.cpp updated")
else:
    print("TARS: no source changes written")


# ============================================================
# 6. FINAL STATUS
# ============================================================

print("")
print("========== TARS OV7670 PATCH DONE ==========")
print("gpio_matrix_in : patched/checked")
print("deinit()       :", "patched" if deinit_ok else "NOT FOUND")
print("init()         :", "patched" if init_ok else "NOT FOUND")
print("I2S0 release   :", "ENABLED")
print("DMA cleanup    :", "ENABLED")
print("IRQ cleanup    :", "ENABLED")
print("Camera mode    :", "DISABLED")
print("============================================")
