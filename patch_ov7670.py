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

# =========================================================
# EXISTING gpio_matrix_in COMPATIBILITY PATCH
# =========================================================

old = "void gpio_matrix_in(int gpio, int signal_index, bool inverted);"

if old in hpp:
    hpp = hpp.replace(old, "")
    with open(HPP, "w", encoding="utf-8") as f:
        f.write(hpp)
    print("TARS: gpio_matrix_in conflict patched")
else:
    print("TARS: gpio_matrix_in patch not needed")

# =========================================================
# RESOURCE SCAN
# =========================================================

print("\nTARS: I2S0 RESOURCE SCAN")

checks = {
    "I2S0": r"\bI2S0\b",
    "I2S0\.conf": r"I2S0\s*->|I2S0\s*\.",
    "esp_intr_alloc": r"\besp_intr_alloc\s*\(",
    "esp_intr_disable": r"\besp_intr_disable\s*\(",
    "esp_intr_enable": r"\besp_intr_enable\s*\(",
    "esp_intr_free": r"\besp_intr_free\s*\(",
    "dma": r"\bdma\b",
    "malloc": r"\bmalloc\s*\(",
    "free": r"\bfree\s*\(",
    "heap_caps_malloc": r"\bheap_caps_malloc\s*\(",
    "heap_caps_free": r"\bheap_caps_free\s*\(",
    "i2s_driver_install": r"\bi2s_driver_install\s*\(",
    "i2s_driver_uninstall": r"\bi2s_driver_uninstall\s*\(",
}

for name, pattern in checks.items():
    found = bool(re.search(pattern, cpp, re.I))
    print("  %-28s %s" % (name, "YES" if found else "NO"))

# =========================================================
# PRINT FUNCTION NAMES
# =========================================================

print("\nTARS: ALL I2SCAMERA FUNCTIONS")

funcs = list(re.finditer(
    r"(?:void|bool|int|uint32_t|uint16_t|uint8_t|"
    r"size_t|esp_err_t|static\s+void|static\s+bool|"
    r"static\s+int|static\s+uint32_t)"
    r"\s+([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{]*\)\s*\{",
    cpp
))

if not funcs:
    print("  NONE FOUND")
else:
    for m in funcs:
        print("  ", m.group(1))

# =========================================================
# PRINT FUNCTIONS RELATED TO START/STOP/CLEANUP/I2S/DMA
# =========================================================

print("\nTARS: IMPORTANT FUNCTION BODIES")

keywords = re.compile(
    r"(init|begin|start|stop|end|deinit|release|"
    r"cleanup|destroy|free|reset|i2s|dma|interrupt|"
    r"camera|capture|frame)",
    re.I
)

for i, m in enumerate(funcs):
    name = m.group(1)

    if not keywords.search(name):
        continue

    start = m.start()
    brace = cpp.find("{", start)

    if brace < 0:
        continue

    depth = 0
    end = brace

    for p in range(brace, len(cpp)):
        if cpp[p] == "{":
            depth += 1
        elif cpp[p] == "}":
            depth -= 1
            if depth == 0:
                end = p + 1
                break

    body = cpp[start:end]

    print("\n----- FUNCTION:", name, "-----")
    print(body)

# =========================================================
# PRINT RAW LINES CONTAINING CRITICAL RESOURCE OPERATIONS
# =========================================================

print("\nTARS: CRITICAL RESOURCE LINES")

lines = cpp.splitlines()

for n, line in enumerate(lines, 1):
    if re.search(
        r"I2S0|esp_intr_|dma|DMA|malloc|free|"
        r"heap_caps_|gpio_matrix|I2S_CONF|I2S_INT",
        line,
        re.I
    ):
        print("%04d: %s" % (n, line))

# =========================================================
# STATUS
# =========================================================

print("\nTARS: PATCH STATUS")
print("TARS: camera source inspected")
print("TARS: diagnostic output generated")
print("TARS: NO I2S0 CLEANUP MODIFICATION APPLIED")
print("TARS: =================================\n")
