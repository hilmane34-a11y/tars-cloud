Import("env")

env.BuildSources(
    "$BUILD_DIR/include_cpp",
    "include",
    src_filter=[
        "+<tars_emotion.cpp>",
        "+<camera_wifi_live.cpp>"
    ]
)
