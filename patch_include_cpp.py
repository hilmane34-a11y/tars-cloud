Import("env")
import os

project = env.subst("$PROJECT_DIR")
framework = env.subst("$PROJECT_PACKAGES_DIR/framework-arduinoespressif32")
libdeps = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env["PIOENV"])

paths = []

def add_header_dirs(root):
    if not os.path.isdir(root):
        return

    for dirpath, dirnames, filenames in os.walk(root):
        if any(f.endswith(".h") for f in filenames):
            paths.append(dirpath)

# Semua header project
add_header_dirs(os.path.join(project, "include"))
add_header_dirs(os.path.join(project, "src"))
add_header_dirs(os.path.join(project, "lib"))

# Semua header framework ESP32
add_header_dirs(framework)

# Semua header library PlatformIO
add_header_dirs(libdeps)

# Path utama project/framework
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
