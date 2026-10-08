Import("env")
import os

project = env.subst("$PROJECT_DIR")
libdeps = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env["PIOENV"])
framework = env.subst("$PROJECT_PACKAGES_DIR/framework-arduinoespressif32")

paths = [
    os.path.join(project, "include"),
    os.path.join(project, "src"),
    os.path.join(project, "lib"),
    os.path.join(project, "lib", "TARS-OV7670"),

    os.path.join(framework, "cores", "esp32"),
    os.path.join(framework, "variants", "esp32"),
    os.path.join(framework, "libraries", "WiFi", "src"),
    os.path.join(framework, "libraries", "WiFiClientSecure", "src"),
    os.path.join(framework, "libraries", "HTTPClient", "src"),
    os.path.join(framework, "libraries", "LittleFS", "src"),
    os.path.join(framework, "libraries", "Wire", "src"),

    os.path.join(libdeps, "WebSockets", "src"),
    os.path.join(libdeps, "audio-tools", "src"),
    os.path.join(libdeps, "libhelix", "src"),
    os.path.join(libdeps, "JPEGENC", "src"),
    os.path.join(libdeps, "ArduinoJson", "src"),
    os.path.join(libdeps, "Adafruit SSD1306", "src"),
    os.path.join(libdeps, "Adafruit GFX Library", "src"),
    os.path.join(libdeps, "Adafruit BusIO", "src"),
]

env.Append(CPPPATH=paths)

env.BuildSources(
    "$BUILD_DIR/include_cpp",
    "include",
    src_filter=["+<*.cpp>"]
)
