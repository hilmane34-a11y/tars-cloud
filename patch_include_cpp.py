Import("env")
import os

project = env.subst("$PROJECT_DIR")
framework = env.subst("$PROJECT_PACKAGES_DIR/framework-arduinoespressif32")
libdeps = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env["PIOENV"])

paths = [
    # Project
    os.path.join(project, "include"),
    os.path.join(project, "src"),
    os.path.join(project, "lib"),
    os.path.join(project, "lib", "TARS-OV7670"),

    # ESP32 core
    os.path.join(framework, "cores", "esp32"),
    os.path.join(framework, "variants", "esp32"),

    # Arduino framework libraries
    os.path.join(framework, "libraries", "Wire", "src"),
    os.path.join(framework, "libraries", "SPI", "src"),
    os.path.join(framework, "libraries", "WiFi", "src"),
    os.path.join(framework, "libraries", "WiFiClientSecure", "src"),
    os.path.join(framework, "libraries", "HTTPClient", "src"),
    os.path.join(framework, "libraries", "LittleFS", "src"),
]

# PlatformIO dependencies
if os.path.isdir(libdeps):
    for name in os.listdir(libdeps):
        p = os.path.join(libdeps, name)

        if not os.path.isdir(p):
            continue

        paths.append(p)

        src = os.path.join(p, "src")
        if os.path.isdir(src):
            paths.append(src)

# Remove duplicates and invalid paths
paths = list(dict.fromkeys(
    p for p in paths if os.path.isdir(p)
))

env.Append(CPPPATH=paths)

# Compile .cpp files located in include/
env.BuildSources(
    os.path.join(env.subst("$BUILD_DIR"), "include_cpp"),
    os.path.join(project, "include"),
    src_filter=["+<*.cpp>"]
)

print("========== TARS INCLUDE CPP PATH ==========")
for p in paths:
    print("TARS INCLUDE:", p)
print("===========================================")
