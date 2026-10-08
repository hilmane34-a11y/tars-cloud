Import("env")
import os

env.Append(CPPPATH=[
    "$PROJECT_DIR/include",
    "$PROJECT_DIR/src",
    "$PROJECT_DIR/lib",
    "$PROJECT_DIR/lib/TARS-OV7670",
    "$PROJECT_PACKAGES_DIR/framework-arduinoespressif32/cores/esp32",
    "$PROJECT_PACKAGES_DIR/framework-arduinoespressif32/variants/esp32",
    "$PROJECT_PACKAGES_DIR/framework-arduinoespressif32/libraries/WiFi/src",
])

libdeps = os.path.join("$PROJECT_DIR", ".pio", "libdeps", env["PIOENV"])

env.Append(CPPPATH=[
    os.path.join(libdeps, "Adafruit GFX Library", "src"),
    os.path.join(libdeps, "Adafruit SSD1306", "src"),
    os.path.join(libdeps, "WebSockets", "src"),
])

env.BuildSources(
    "$BUILD_DIR/include_cpp",
    "include",
    src_filter=["+<*.cpp>"]
)
