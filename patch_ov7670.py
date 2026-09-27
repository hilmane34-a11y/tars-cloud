Import("env")

import os

# ============================================================
# TARS - OV7670 PATCH
# ESP32-OV7670-no-FIFO
#
# Tujuan:
# - Compatibility gpio_matrix_in
# - OV7670 tetap menggunakan I2S0
# - Kamera tidak lagi diputus saat TARS speaking
# - Tidak ada lifecycle CAMERA -> DAC -> CAMERA
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
# gpio_matrix_in compatibility
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
# READ SOURCE
# ============================================================

with open(cpp_file, "r", encoding="utf-8") as f:
    data = f.read()


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
# SAFE INIT
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

    if (!i2sInit(
        VSYNC, HREF, PCLK,
        D0, D1, D2, D3,
        D4, D5, D6, D7
    )) {

        DEBUG_PRINTLN("I2S initialization failed!");

        free(frame);
        frame = nullptr;

        return false;
    }

    dmaBufferInit(xres * 2 * 2);

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

print(
    "TARS: init() patched"
    if init_ok
    else
    "TARS ERROR: init() function not found"
)


# ============================================================
# WRITE
# ============================================================

if init_ok:

    with open(cpp_file, "w", encoding="utf-8") as f:
        f.write(data)

    print("TARS: I2SCamera.cpp updated")
else:
    print("TARS: no source changes written")


print("")
print("========== TARS OV7670 PATCH DONE ==========")
print("gpio_matrix_in :", "patched/checked")
print("init()         :", "patched" if init_ok else "NOT FOUND")
print("CAMERA MODE    : PERMANENT LIVE")
print("DAC SWITCH     : DISABLED")
print("I2S0 CAMERA    : PERSISTENT")
print("============================================")
