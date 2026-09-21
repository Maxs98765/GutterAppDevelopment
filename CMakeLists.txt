# ---------------------------------------------------------------------------
#  Project-root CMakeLists.txt for the Arduino-free ESP32-P4 hub.
#
#  RECONSTRUCTED FILE, same situation as the header files earlier in this
#  project: an ESP-IDF project needs one of these at its root (plus
#  main/CMakeLists.txt), but no build files of any kind existed anywhere
#  in GC -- only source files and headers survived whatever export
#  flattened this project originally. This is the standard two-line
#  ESP-IDF project root; there is nothing project-specific to get wrong
#  here.
#
#  Build with:
#     idf.py set-target esp32p4
#     idf.py build flash monitor
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32p4_hub_no_arduino)
