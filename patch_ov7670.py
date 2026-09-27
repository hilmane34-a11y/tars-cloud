Import("env")

import os

lib_dir = os.path.join(
    env.subst("$PROJECT_LIBDEPS_DIR"),
    env.subst("$PIOENV"),
    "ESP32-OV7670-no-FIFO",
    "src"
)

header = os.path.join(lib_dir, "I2SCamera.h")

if os.path.isfile(header):
    with open(header, "r", encoding="utf-8") as f:
        data = f.read()

    old = "void gpio_matrix_in(int gpio, int signal_index, bool inverted);"

    if old in data:
        data = data.replace(old, "")
        with open(header, "w", encoding="utf-8") as f:
            f.write(data)
        print("TARS: OV7670 gpio_matrix_in conflict patched")
    else:
        print("TARS: OV7670 patch not needed")
else:
    print("TARS: I2SCamera.h not found")
