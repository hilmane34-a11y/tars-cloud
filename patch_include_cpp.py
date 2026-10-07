Import("env")

env.Append(
    CPPPATH=[
        "include",
        "$PROJECTSRC_DIR",
        "$PROJECT_DIR/include",
        "$PROJECT_DIR/src",
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
