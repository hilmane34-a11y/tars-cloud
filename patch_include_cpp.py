Import("env")
import os

project = env.subst("$PROJECT_DIR")
framework = env.subst("$PROJECT_PACKAGES_DIR/framework-arduinoespressif32")
libdeps = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env["PIOENV"])

paths = [
    os.path.join(project, "include"),
    os.path.join(project, "src"),
    os.path.join(project, "lib"),
    os.path.join(project, "lib", "TARS-OV7670"),

    os.path.join(framework, "cores", "esp32"),
    os.path.join(framework, "variants", "esp32"),
]

# Cari semua folder yang berisi header library
wanted = {
    "Adafruit_GFX.h",
    "Adafruit_SSD1306.h",
    "WebSocketsClient.h",
    "AudioTools.h",
    "ArduinoJson.h",
    "JPEGDEC.h",
    "WiFiClientSecure.h",
    "HTTPClient.h",
    "LittleFS.h",
}

for root in [framework, libdeps]:
    if not os.path.isdir(root):
        continue

    for dirpath, dirnames, filenames in os.walk(root):
        if wanted.intersection(filenames):
            paths.append(dirpath)

paths = list(dict.fromkeys(paths))

# Paksa include path masuk sebagai compiler -I
for p in paths:
    if os.path.isdir(p):
        env.Append(CPPPATH=[p])

# Build semua .cpp di include/
env.BuildSources(
    os.path.join(env.subst("$BUILD_DIR"), "include_cpp"),
    os.path.join(project, "include"),
    src_filter=["+<*.cpp>"]
)

print("========== TARS INCLUDE CPP PATH ==========")
for p in paths:
    print("TARS INCLUDE:", p)
print("===========================================")
