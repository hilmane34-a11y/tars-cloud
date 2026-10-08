Import("env")
import os

project = env.subst("$PROJECT_DIR")
framework = env.subst("$PROJECT_PACKAGES_DIR/framework-arduinoespressif32")
libdeps = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env["PIOENV"])

roots = [
    project,
    framework,
    libdeps,
]

headers = {
    "Adafruit_GFX.h",
    "Adafruit_SSD1306.h",
    "Adafruit_I2CDevice.h",
    "Adafruit_BusIO_Register.h",
    "WebSocketsClient.h",
    "AudioTools.h",
    "ArduinoJson.h",
    "WiFi.h",
    "WiFiClientSecure.h",
    "HTTPClient.h",
    "LittleFS.h",
}

paths = []

for root in roots:
    if not os.path.isdir(root):
        continue

    for dirpath, dirnames, filenames in os.walk(root):
        found = headers.intersection(filenames)

        if found:
            paths.append(dirpath)

paths += [
    os.path.join(project, "include"),
    os.path.join(project, "src"),
    os.path.join(project, "lib"),
    os.path.join(project, "lib", "TARS-OV7670"),
    os.path.join(framework, "cores", "esp32"),
    os.path.join(framework, "variants", "esp32"),
]

paths = list(dict.fromkeys(
    p for p in paths if os.path.isdir(p)
))

for p in paths:
    env.Append(CPPPATH=[p])

env.BuildSources(
    os.path.join(env.subst("$BUILD_DIR"), "include_cpp"),
    os.path.join(project, "include"),
    src_filter=["+<*.cpp>"]
)

print("========== TARS INCLUDE CPP PATH ==========")
for p in paths:
    print("TARS INCLUDE:", p)
print("===========================================")
