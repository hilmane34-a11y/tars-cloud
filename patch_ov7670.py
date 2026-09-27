Import("env")

import os
import re

LIB = os.path.join(
    env.subst("$PROJECT_LIBDEPS_DIR"),
    env.subst("$PIOENV"),
    "ESP32-OV7670-no-FIFO",
    "src"
)

CPP = os.path.join(LIB, "I2SCamera.cpp")
HPP = os.path.join(LIB, "I2SCamera.h")

print("\n========== TARS OV7670 PATCH ==========")
print("LIB :", LIB)

if not os.path.isfile(CPP):
    print("TARS: I2SCamera.cpp NOT FOUND")
    Exit(1)

with open(CPP, "r", encoding="utf-8") as f:
    cpp = f.read()

with open(HPP, "r", encoding="utf-8") as f:
    hpp = f.read()

print("TARS: I2SCamera.cpp FOUND")
print("TARS: size =", len(cpp), "bytes")

# -------------------------------------------------
# Existing gpio_matrix_in conflict patch
# -------------------------------------------------

old = "void gpio_matrix_in(int gpio, int signal_index, bool inverted);"

if old in hpp:
    hpp = hpp.replace(old, "")
    with open(HPP, "w", encoding="utf-8") as f:
        f.write(hpp)
    print("TARS: gpio_matrix_in conflict patched")
else:
    print("TARS: gpio_matrix_in patch not needed")

# -------------------------------------------------
# Diagnostic: detect important I2S0 resources
# -------------------------------------------------

checks = {
    "I2S0": r"\bI2S0\b",
    "esp_intr_alloc": r"\besp_intr_alloc\s*\(",
    "esp_intr_disable": r"\besp_intr_disable\s*\(",
    "esp_intr_free": r"\besp_intr_free\s*\(",
    "dma": r"\bDMA\b|dma",
    "i2s_driver_install": r"\bi2s_driver_install\s*\(",
    "i2s_driver_uninstall": r"\bi2s_driver_uninstall\s*\(",
    "gpio_isr_handler_add": r"\bgpio_isr_handler_add\s*\(",
    "gpio_isr_handler_remove": r"\bgpio_isr_handler_remove\s*\(",
    "gpio_uninstall_isr_service": r"\bgpio_uninstall_isr_service\s*\(",
}

print("\nTARS: I2S0 RESOURCE SCAN")

for name, pattern in checks.items():
    found = bool(re.search(pattern, cpp, re.I))
    print("  %-28s %s" % (name, "YES" if found else "NO"))

# -------------------------------------------------
# Show functions that look like cleanup/deinit
# -------------------------------------------------

print("\nTARS: CLEANUP FUNCTIONS")

for m in re.finditer(
    r"(?:void|bool|int|esp_err_t|static\s+void)"
    r"\s+([A-Za-z_][A-Za-z0-9_]*)\s*\([^)]*\)\s*\{",
    cpp
):
    name = m.group(1)

    if re.search(
        r"(stop|deinit|deinit|release|destroy|cleanup|end|free)",
        name,
        re.I
    ):
        print("  ", name)

# -------------------------------------------------
# IMPORTANT:
# Do NOT inject guessed I2S uninstall code.
# The exact driver implementation must be known.
# -------------------------------------------------

print("\nTARS: PATCH STATUS")
print("TARS: camera source inspected")
print("TARS: no guessed I2S0 uninstall injected")
print("TARS: =================================\n")
