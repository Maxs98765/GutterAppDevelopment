# Camera + rotating-arm rig -- ESP32-P4 hub + Android app

This folder is everything needed to build this project from scratch: the
ESP32-P4 firmware, the Android app, the wiring diagram, and the exact
parts list. You should not need anything else, and you should not need to
ask whoever handed you this folder any questions to get it running.

**What it does:** an ESP32-P4 board with a camera streams live video over
WiFi to an Android app. The same app has a button that rotates an arm
120°, holds it there for a set time, then returns it to 0. There is no
separate microcontroller anywhere in this design -- one board does
everything.

```
Arducam IMX708 camera ──MIPI-CSI──> ESP32-P4 ──WiFi──> Android app
                                        │
                                        └──PWM+DIR──> Cytron MD13S ──> 12V worm-gear motor
```

## Start here

Read these in order:

1. **`BOM.xlsx`** -- the exact parts this was built for. If you're
   assembling the hardware, buy from this list, not a substitute --
   several parts (the camera, the motor driver) only work with this
   firmware because of specific choices made for these exact parts.
2. **`wiring-guide.html`** -- open it in any browser. Shows every
   connection: camera, motor driver, motor, and the solar/battery power
   system.
3. **`INSTALL.md`** -- the actual step-by-step: install the toolchains,
   wire it up, build and flash the firmware, build and install the
   Android app, connect over WiFi, verify it all works.

## What's in this folder

```
README.md            you are here
INSTALL.md            the full build/flash/install walkthrough
wiring-guide.html      wiring diagram (open in a browser)
BOM.xlsx               exact parts list
CHANGES.md             engineering notes: what this build assumes and
                        what to double-check before your first build
CMakeLists.txt         ESP-IDF project root
main/                  ESP32-P4 hub firmware (C++, ESP-IDF)
  config.h             every setting: WiFi, camera, motor pins/timing
  app_main.cpp          startup sequence
  net.cpp               WiFi + the onboard WiFi co-processor
  mipi_cam.cpp          camera capture
  motor.cpp             motor driver control + rotation timing
  http_api.cpp           the HTTP server the phone app talks to
  frame_store.cpp        thread-safe handoff of the latest camera frame
  jpeg_hw.cpp            hardware JPEG encoder (fallback path)
app/                   Android app source (Qt 6 / C++ / QML)
  config.h              app-side settings (default address, hold time)
  Main.qml               the interface
  servolink.cpp          talks to the hub's HTTP API
  mjpegclient.cpp         decodes the live video stream
partitions.csv          flash layout (includes the WiFi co-processor's
                        own firmware slot)
sdkconfig.defaults      build-time firmware settings
```

## If something doesn't work

`INSTALL.md` has a troubleshooting section covering the most likely
issues (camera not detected, WiFi co-processor not responding, app can't
reach the rig, motor running the wrong direction, and so on). `CHANGES.md`
lists a few specific things about this build that could not be confirmed
without the physical board in hand -- read it if you get stuck on
something `INSTALL.md` doesn't cover.
