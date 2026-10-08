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

# Ambil semua folder src/include dari framework dan libdeps
for root in [
    os.path.join(framework, "libraries"),
    libdeps,
]:
    if os.path.isdir(root):
        for dirpath, dirnames, filenames in os.walk(root):
            if os.path.basename(dirpath) in ("src", "include"):
                paths.append(dirpath)

# Hilangkan duplikat
paths = list(dict.fromkeys(paths))

env.Append(CPPPATH=paths)

env.BuildSources(
    "$BUILD_DIR/include_cpp",
    "include",
    src_filter=[
        "+<*.cpp>",
    ]
)
