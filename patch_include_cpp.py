Import("env")

framework = "$PROJECT_PACKAGES_DIR/framework-arduinoespressif32"
libdeps = "$PROJECT_LIBDEPS_DIR/$PIOENV"

env.Append(CPPPATH=[
    "$PROJECT_DIR/include",
    "$PROJECT_DIR/src",
    "$PROJECT_DIR/lib",
    "$PROJECT_DIR/lib/TARS-OV7670",

    # ESP32 Arduino framework
    framework + "/cores/esp32",
    framework + "/variants/esp32",
    framework + "/libraries/WiFi/src",
    framework + "/libraries/WiFiClientSecure/src",
    framework + "/libraries/HTTPClient/src",
    framework + "/libraries/LittleFS/src",
    framework + "/libraries/Wire/src",

    # PlatformIO libraries
    libdeps + "/WebSockets/src",
    libdeps + "/audio-tools/src",
    libdeps + "/libhelix/src",
    libdeps + "/JPEGENC/src",
    libdeps + "/ArduinoJson/src",
    libdeps + "/Adafruit SSD1306/src",
    libdeps + "/Adafruit GFX Library/src",
    libdeps + "/Adafruit BusIO/src",
])

env.BuildSources(
    "$BUILD_DIR/include_cpp",
    "include",
    src_filter=["+<*.cpp>"]
)
