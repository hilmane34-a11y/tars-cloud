Import("env")

env.Append(
    CPPPATH=[
        "$PROJECT_DIR/include",
        "$PROJECT_DIR/src",
        "$PROJECT_DIR/lib",
        "$PROJECT_DIR/lib/TARS-OV7670",

        "$PROJECT_PACKAGES_DIR/framework-arduinoespressif32/cores/esp32",
        "$PROJECT_PACKAGES_DIR/framework-arduinoespressif32/variants/esp32",
        "$PROJECT_PACKAGES_DIR/framework-arduinoespressif32/libraries/WiFi/src",
    ]
)

env.BuildSources(
    "$BUILD_DIR/include_cpp",
    "include",
    src_filter=[
        "+<tars_emotion.cpp>",
        "+<camera_wifi_live.cpp>"
    ]
)
