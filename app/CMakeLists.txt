cmake_minimum_required(VERSION 3.21)
project(ServoRig VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Multimedia is deliberately not used: Qt cannot decode a multipart MJPEG
# stream, so MjpegClient does the framing and QImage does the JPEG decode.
find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2 Network Gui)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(appServoRig
    main.cpp
)

qt_add_qml_module(appServoRig
    URI ServoRig
    VERSION 1.0
    QML_FILES
        Main.qml
    SOURCES
        config.h
        servolink.h  servolink.cpp
        mjpegclient.h mjpegclient.cpp
        videosurface.h videosurface.cpp
)

target_link_libraries(appServoRig PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::Gui
)

set_target_properties(appServoRig PROPERTIES
    QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android"
    QT_ANDROID_APP_NAME "Servo rig"
    QT_ANDROID_VERSION_NAME "1.0"
    QT_ANDROID_VERSION_CODE 1
    QT_ANDROID_TARGET_SDK_VERSION 34
    QT_ANDROID_MIN_SDK_VERSION 28
)

install(TARGETS appServoRig
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
