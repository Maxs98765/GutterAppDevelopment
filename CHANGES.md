# What changed in this rewrite

This is the engineering changelog for this folder -- what was rewritten,
why, and what to double-check. If you're setting the rig up and haven't
read it yet, start with **README.md** instead; come back here if you want
the reasoning behind a specific file.

This folder is a complete, self-contained project: everything needed to
build the ESP32-P4 hub firmware and the Android app, using **only** parts
that are actually in `BOM.xlsx` -- no Arduino Uno, no L298N, no USB
webcam. It supersedes the older, partial build that used to live loose in
the parent folder.

## What changed, and why

The BOM has three parts that don't match what the earlier code assumed:

| BOM part | Old code assumed | This folder assumes |
|---|---|---|
| Arducam Raspberry Pi Camera Module 3 (Sony IMX708, CSI ribbon) | A USB UVC webcam | A MIPI-CSI camera, driven by Espressif's `esp_video`/`esp_cam_sensor` + a third-party IMX708 driver |
| Cytron MD13S motor driver (2 control pins: PWM + DIR) | An L298N (3 pins: ENA/IN1/IN2) | Direct PWM+DIR control from this board's own GPIOs |
| *(nothing -- no Arduino Uno anywhere in the BOM)* | An Arduino Uno running the motor's timing state machine, reached over UART | The same timing state machine, running on the ESP32-P4 hub itself |

## File-by-file

**New files** (didn't exist before): `mipi_cam.cpp/.h` (camera capture),
`motor.cpp/.h` (motor control).

**Changed files**: `config.h` (camera pins/format + motor pins/timing
replace the old USB-camera and Uno-serial settings), `app_main.cpp`
(starts `motor::` and `cam::` instead of `uno::` and `uvc::`),
`http_api.cpp` (the `/rotate`, `/home`, `/hold`, `/status` handlers call
`motor::` directly instead of going over serial to a Uno -- the JSON
responses are unchanged, so the phone app needed no changes at all),
`idf_component.yml` (drops `usb_host_uvc`, adds `esp_video` +
`esp_cam_sensor` + a third-party IMX708 driver), `sdkconfig.defaults`
(drops the UVC option, adds the MIPI-CSI/ISP options, and fixes a
pre-existing bug -- see below), plus the two build files below.

**Unchanged, carried over as-is**: `net.cpp/.h` (WiFi + C6 co-processor
bring-up), `frame_store.cpp/.h` (the JPEG ring buffer), `http_api.h`,
`jpeg_hw.cpp/.h` (kept as `mipi_cam.cpp`'s fallback encoder -- see below),
`partitions.csv`.

**New build files, not carried over from anywhere -- they never existed**:
`CMakeLists.txt` (project root) and `main/CMakeLists.txt`. No build files
of any kind were present in `GC` for this firmware; the same export that
lost every header file earlier in this project apparently lost these too.
These are the two standard files any ESP-IDF project needs; there was
nothing project-specific to reconstruct.

**Deleted / not carried into this folder**: `uno_link.cpp/.h`,
`uvc_source.cpp/.h`, and the whole `uno_servo/` Arduino sketch folder. The
originals are still sitting in `GC`'s top level if you want to look back
at them, but they no longer match your parts list -- I did not delete
them from `GC` (that's your call), but they should not be built anymore.
The `camera_probe` project (`main.cpp` + `probe_config.h` at `GC`'s top
level) is also now obsolete -- it specifically diagnoses USB UVC cameras,
which you no longer have.

## A pre-existing bug fixed along the way

`sdkconfig.defaults` never actually set `CONFIG_PARTITION_TABLE_CUSTOM`.
Without that, ESP-IDF ignores `partitions.csv` entirely and silently uses
its own default table -- meaning the `slave_fw` partition the C6
co-processor's firmware depends on was never actually being created. This
was true of the old build too; it wasn't something the Arduino/camera
swap introduced.

## Verify before you flash -- three things this rewrite could not confirm

This is a bigger hardware change than the earlier servo-to-gearmotor
swap, and two of the three new pieces (the camera driver and the exact
GPIO pins) are things I could not build-test or read off your physical
board:

1. **`mipi_cam.cpp` is written against the standard V4L2 API shape that
   Espressif's `esp_video` documents itself as compatible with, not
   against a confirmed-working example for your exact component
   versions.** Once `idf.py add-dependency` has pulled `esp_video`,
   `esp_cam_sensor`, and `esp_cam_sensor_imx` into `managed_components/`,
   check `esp_video_init_config_t`/`esp_video_init_csi_config_t`'s actual
   field names in that version's `esp_video_init.h` before your first
   build -- struct layouts have moved across `esp_video` releases. The
   file's own header comment lists exactly what to check.

2. **All four new pin numbers are best-inference placeholders, not read
   off your board**: `kCamSccbSclPin`/`kCamSccbSdaPin` (camera I2C
   control bus) and `kMotorPwmPin`/`kMotorDirPin` (Cytron MD13S) in
   `config.h`. Confirm these against the Waveshare ESP32-P4-WIFI6's own
   schematic/pinout before wiring anything -- same caution that applied
   to the old `kUnoTxPin`/`kUnoRxPin` guess.

3. **`esp_cam_sensor_imx` is a third-party component**, not an official
   Espressif driver (Espressif's own `esp_cam_sensor` doesn't carry IMX708
   support as of this writing). Check it's still maintained and read its
   license before you build against it, and pin an exact version once
   you've confirmed it compiles for you.

## Docs added since the code rewrite

The first pass through this folder was code only. It has since been
rounded out into a complete, standalone handoff package -- everything
someone would need to build this without asking you anything:

- `README.md` -- the front door; start there
- `INSTALL.md` -- full step-by-step build/flash/wire/app-install guide,
  written for someone with no prior context on this project
- `wiring-guide.html` -- diagram + connection tables for the camera,
  motor driver, and solar/battery power system, matching this build
- `BOM.xlsx` -- your original parts list, copied in so the recipient has
  exact part links, not just names
- `app/` -- the Qt/Android app source, copied in from `GC/app` (unchanged
  -- it never needed to change) so this folder doesn't depend on anything
  outside itself

`SBS.md`, `README.md`, and `wiring-guide.html` at `GC`'s top level (the
parent folder, not this one) still describe the old Arduino+L298N+UVC
design and are now fully superseded by the four docs above -- they're
safe to ignore or delete.
