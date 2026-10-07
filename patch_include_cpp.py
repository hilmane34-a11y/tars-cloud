Import("env")

for f in [
    "include/tars_emotion.cpp",
    "include/camera_wifi_live.cpp"
]:
    env.BuildSources(
        "$BUILD_DIR/include_cpp",
        f
    )
