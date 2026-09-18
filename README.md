# Servo rig

Android app, ESP32-S3 hub, and Arduino Uno actuator. One button rotates a
servo to 180°, holds it, and returns it to 0. Live camera feed alongside.

```
USB camera ──USB──> ESP32-S3 ──WiFi──> Android app
                       │
                       └──UART──> Arduino Uno ──PWM──> servo
```

The S3 is the only thing on WiFi. It hosts the camera, re-streams it as
MJPEG, and relays servo commands down the serial link to the Uno.

## The tweakable hold duration

The number lives in **`firmware/uno_servo/uno_servo.ino`**:

```cpp
unsigned long HOLD_MS = 10000UL;   // 10 seconds
```

Three other places can override it, in increasing order of convenience:

| Where | What it does |
|---|---|
| `uno_servo.ino`, `HOLD_MS` | the real default; needs a reflash |
| `esp32s3_hub/main/config.h`, `kHoldOverrideMs` | non-zero overrides every request |
| `app/config.h`, `kDefaultHoldMs` | what the app sends; needs a rebuild |
| the app's settings slider | 1–60 s, no rebuild, and "Save to the Arduino" makes it the new default |

Set `kSendHoldWithRotate = false` in `app/config.h` if you would rather the
sketch be the single source of truth and have the app not send a duration.

Related knobs in `uno_servo.ino`: `ANGLE_HOME`, `ANGLE_TARGET`,
`SWEEP_STEP_DEG` and `SWEEP_STEP_MS` (sweep speed — set the step to 0 to snap
instantly), `SERVO_MIN_US` / `SERVO_MAX_US` (pulse widths for true 0° and
180° on your particular servo), and `DETACH_AFTER_MS`.

## Wiring

**Serial link.** The Uno runs at 5 V and the S3 at 3.3 V, so one direction
needs a divider.

| Uno | ESP32-S3 | Note |
|---|---|---|
| D10 (SoftwareSerial RX) | GPIO17 | direct; 3.3 V clears the Uno's 3.0 V threshold |
| D11 (SoftwareSerial TX) | GPIO18 | **through a divider**: 10 kΩ in series, 15 kΩ to GND → about 3.0 V |
| GND | GND | required, and easy to forget |

Do not feed D11 straight into GPIO18. 5 V on an S3 pin damages it.

**Servo.** Signal to Uno **D9**. Power from its own 5 V supply, not the Uno's
regulator — a servo stalling at the end stop pulls far more than the Uno can
give and will brown out the board mid-command. Tie all grounds together.

**Camera.** The S3's USB PHY is hardwired: D− is GPIO19, D+ is GPIO20. Easiest
path is the board's native USB-OTG jack with an OTG adapter. If you are
breaking out to a header instead, D−→GPIO19, D+→GPIO20, GND→GND, and feed
**VBUS from a 5 V supply** — the S3 cannot source bus power itself.

On a dev board with two USB ports, the native USB port is now the camera, so
flash and monitor through the UART bridge port.

## Building

**Uno.** Open `firmware/uno_servo/uno_servo.ino` in the Arduino IDE, board
"Arduino Uno", upload. No libraries to install; `Servo` and `SoftwareSerial`
ship with the IDE.

**ESP32-S3.** ESP-IDF 5.0 or newer. The UVC host driver comes down
automatically from the component registry.

```sh
cd firmware/esp32s3_hub
idf.py set-target esp32s3
# put your WiFi credentials in main/config.h first
idf.py build flash monitor
```

Your S3 module **must have PSRAM** (N8R2, N16R8, and similar). JPEG frames
and USB transfer buffers do not fit in internal RAM, and the firmware will
tell you so on the console if PSRAM is missing or disabled.

**Android app.** Qt 6.5+ with the Android kit, plus the Android SDK/NDK that
Qt's installer sets up.

```sh
cd app
# Qt Creator: open CMakeLists.txt, pick the Android Qt 6 kit, Run.
# Or from a shell:
~/Qt/6.5.3/android_arm64_v8a/bin/qt-cmake -S . -B build-android
cmake --build build-android --target apk
```

Set the rig address in `app/config.h` (`kDefaultHost`) or just type it into
the app's settings panel. `192.168.4.1` is right when the phone is joined to
the hub's own access point.

## Bring-up order

Getting all three working at once is hard to debug. Go in stages.

1. **Servo alone.** Flash the Uno, open the serial monitor at 115200, and
   watch for `uno_servo ready`. The Uno replies on the software link, not
   USB, so to test by hand short D10/D11 to a USB-serial adapter — or just
   move on and test through the S3.
2. **Servo over WiFi, no camera.** Set `kCameraEnabled = false` in
   `esp32s3_hub/main/config.h`, flash, and open `http://<rig>/` in a phone
   browser. The Rotate button there exercises the whole chain. If the console
   logs `no reply to "P"`, the problem is the serial link: check the divider,
   the common ground, and that both ends agree on 9600 baud.
3. **Camera.** Set `kCameraEnabled = true`, plug the camera in, reflash, and
   watch the log for `first frame`.
4. **App.** Point it at the rig and run it.

## When it misbehaves

**`uvc_streaming_config failed` or no frames.** The camera has to advertise
MJPEG at exactly the resolution and frame rate in `config.h`. Set
`kResolutionAny = true`, reflash, and read the log to see what the camera
actually offers, then pin those numbers. Very cheap cameras sometimes only
offer uncompressed YUY2, which is far too much bandwidth for this path —
that one needs a different camera.

**`frame too large`.** Raise `kMaxFrameBytes`.

**Video stutters.** Drop `kFrameRate` before dropping resolution. Or turn on
the settings switch for still-image polling, which trades smoothness for
resilience.

**Servo twitches or the Uno resets.** Almost always power. Give the servo its
own supply, and add a 470 µF capacitor across it close to the servo.

**Servo buzzes at the end of travel.** It is being commanded past its
mechanical stop. Lower `SERVO_MAX_US` in steps of 50.

**App shows "Rig not responding".** Confirm the address in settings, confirm
the phone is on the same network, and load `http://<rig>/status` in the
phone's browser to isolate whether it is the app or the network.

## What is where

```
firmware/uno_servo/uno_servo.ino     servo state machine, hold timing
firmware/esp32s3_hub/                ESP-IDF project (C++)
  main/config.h                      all hub settings
  main/app_main.cpp                  WiFi, mDNS, startup
  main/uvc_source.cpp                USB host, camera negotiation
  main/frame_store.cpp               thread-safe latest-frame handoff
  main/http_api.cpp                  MJPEG stream + servo routes
  main/uno_link.cpp                  UART master
app/                                 Qt 6 Android app (C++/QML)
  config.h                           all app settings
  servolink.cpp                      HTTP control client, telemetry polling
  mjpegclient.cpp                    multipart MJPEG parsing and decoding
  videosurface.cpp                   paints frames into the scene graph
  Main.qml                           the interface
```

## HTTP API

Useful for testing from a browser or `curl`, and for anything else you want
to bolt on later.

| Route | Effect |
|---|---|
| `GET /stream` | multipart MJPEG video |
| `GET /snapshot.jpg` | one JPEG frame |
| `GET /rotate` | rotate, hold, return |
| `GET /rotate?hold=4000` | same, with a one-shot 4 s hold |
| `GET /home` | return to 0 immediately |
| `GET /hold?ms=10000` | change the Uno's default hold |
| `GET /status` | JSON state, angle, ms remaining, camera health |
| `GET /` | plain control page |
