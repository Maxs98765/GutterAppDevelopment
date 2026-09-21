# ---------------------------------------------------------------------------
#  Component registration for the hub's single "main" component.
#
#  RECONSTRUCTED FILE -- see the root CMakeLists.txt's comment. Lists
#  every .cpp in this folder; uno_link.cpp/uvc_source.cpp are gone
#  (replaced by motor.cpp/mipi_cam.cpp), jpeg_hw.cpp is carried over
#  unchanged as mipi_cam.cpp's RGB565 fallback path.
# ---------------------------------------------------------------------------
idf_component_register(
    SRCS
        "app_main.cpp"
        "net.cpp"
        "http_api.cpp"
        "frame_store.cpp"
        "mipi_cam.cpp"
        "motor.cpp"
        "jpeg_hw.cpp"
    INCLUDE_DIRS
        "."
)
