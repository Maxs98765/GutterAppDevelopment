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

## Pre-existing bugs fixed along the way

`sdkconfig.defaults` never actually set `CONFIG_PARTITION_TABLE_CUSTOM`.
Without that, ESP-IDF ignores `partitions.csv` entirely and silently uses
its own default table -- meaning the `slave_fw` partition the C6
co-processor's firmware depends on was never actually being created. This
was true of the old build too; it wasn't something the Arduino/camera
swap introduced.

`sdkconfig.defaults` also never set `CONFIG_ESPTOOLPY_FLASHSIZE`, so
ESP-IDF assumed the smallest possible flash chip (2MB) when checking the
partition table's total size against it. `partitions.csv`'s own table
(the factory app plus `slave_fw`) adds up to about 4.1MB -- comfortably
real flash, just bigger than that unstated 2MB default -- so the very
first build failed with "Partitions tables occupies 4.1MB ... does not
fit in configured flash size 2MB." Fixed by setting it explicitly to
16MB, which is what Waveshare's own published specs list for the
ESP32-P4-WIFI6 Dev Kit. If you're building for a different board, check
its actual flash size and change this to match -- setting it larger than
the real chip fails at flash time, not at build time, so it's worth
getting right rather than just picking the biggest number.

`idf_component.yml` never declared a dependency on `espressif/mdns`,
even though `net.cpp` calls `mdns_init()`/`mdns_hostname_set()`/
`mdns_service_add()` so the rig is reachable at `http://<name>.local/`.
On older IDF releases mDNS shipped bundled with the SDK itself, so this
went unnoticed; current IDF versions split it out into its own
separately-versioned component, and without it declared the build fails
partway through with "Missing 'mdns.h' file name found in the following
component(s): lwip(...), openthread(...)" -- both of those are red
herrings (unrelated headers that happen to share the name); the real fix
is declaring the real `mdns` component. This was missing the same way
`esp_hosted` was (see above) -- not something the camera/motor rewrite
introduced. Fixed by adding `espressif/mdns` to `idf_component.yml`.

## Mistakes introduced by this rewrite, now fixed

Unlike the two bugs above, this one wasn't pre-existing -- I introduced
it and it's worth being upfront about. The original `http_api.cpp`'s
`/rotate` handler had a fallback constant, `cfg::kHoldOverrideMs`, for
forcing a hold duration on requests that didn't pass `?hold=`. When I
rewrote `config.h` for the new camera/motor settings, that constant
didn't make it back in, so the build failed with `'kHoldOverrideMs' is
not a member of 'cfg'`. Rather than re-adding an unused constant, I
removed the now-dead fallback block in `http_api.cpp`: `motor::rotate()`
already falls back to `cfg::kHoldMs` whenever it's passed `0`, which is
exactly what a missing `?hold=` produces, so nothing was lost -- the
behavior with no `?hold=` param is identical to before.

## Build errors found once real compilation started

The three sections above were all found by reasoning about the code before
a real ESP-IDF toolchain ever touched it. The two below only surfaced once
an actual build ran against the actual installed component versions and
actual compiler -- which is exactly what risk #1 below warned would be
needed. Recording them here the same way as everything above.

**`mipi_cam.cpp`'s `esp_video_init_csi_config_t`/`esp_video_init_sccb_config_t`
field names were wrong.** This is risk #1 below, materializing: the file
was written against the general shape Espressif's docs describe, and the
actually-installed `esp_video` version's real struct (in
`managed_components/espressif__esp_video/include/esp_video_init.h`) uses
`sccb_config.i2c_config.port`/`.scl_pin`/`.sda_pin`/`.freq` (nested inside
a union), not the flat `i2c_port`/`freq_hz` names guessed originally, and
`scl_pin`/`sda_pin`/`reset_pin`/`pwdn_pin` are all typed `gpio_num_t`, not
plain `int`. Fixed by reading that installed header directly and matching
its real field names and types (config.h's plain-`int` pin constants now
get an explicit `static_cast<gpio_num_t>(...)`).

**Several log lines format a `uint16_t`/`uint32_t` value with a bare `%d`
or `%u`, which this toolchain treats as a hard error (`-Werror=format=`)
rather than a warning.** In C/C++ varargs, a `uint16_t` argument is
promoted to plain `int` before it reaches `printf`-family functions, so it
no longer matches `%u` (which expects `unsigned int`) -- and this
particular toolchain's `uint32_t` is typedef'd to `unsigned long`, so it
doesn't match plain `%d`/`%u` either. Went through every `ESP_LOGx`/
`snprintf` call in this folder's `.cpp` files checking the actual type of
every argument against its format specifier, and fixed the ones that
didn't match: `net.cpp`'s co-processor version string (`%d.%d.%d` on
`uint32_t` fields -> `%" PRIu32 "` from `<cinttypes>`), and four spots
formatting `uint16_t` frame/port values with a bare `%u`
(`jpeg_hw.cpp`, `mipi_cam.cpp` x2, `http_api.cpp`) -> each now casts
explicitly to `unsigned` first, matching the style `frame_store.cpp` and
`app_main.cpp` already used everywhere. None of this touched actual
behavior -- only how the numbers are printed to the log.

## Verify before you flash -- three things this rewrite could not confirm

This is a bigger hardware change than the earlier servo-to-gearmotor
swap, and two of the three new pieces (the camera driver and the exact
GPIO pins) are things I could not build-test or read off your physical
board:

1. ~~**`mipi_cam.cpp` is written against the standard V4L2 API shape...**~~
   **Update: confirmed and fixed.** Once your build actually pulled in
   `esp_video`, the real struct in `esp_video_init.h` turned out to use
   different field names/types than guessed -- see "Build errors found
   once real compilation started" above for exactly what changed. The
   rest of `mipi_cam.cpp` (the V4L2 `ioctl()` calls: `VIDIOC_S_FMT`,
   `VIDIOC_REQBUFS`, `VIDIOC_QBUF`, `VIDIOC_DQBUF`, `VIDIOC_STREAMON/OFF`)
   compiled clean against your installed `linux/videodev2.h`, so that part
   of the original caution turned out fine. What's still unverified is
   runtime behavior: whether the sensor is actually detected, whether
   `VIDIOC_S_FMT` with `V4L2_PIX_FMT_JPEG` is accepted for this CSI path
   (falls back to RGB565 + hardware JPEG automatically and logs it if not)
   -- that can only be confirmed once this actually flashes and boots.

2. ~~**All four new pin numbers are best-inference placeholders...**~~
   **Update: confirmed against the board's own pinout diagram, and one
   was wrong.** Waveshare publishes a "Pinout Definition" header table
   for the ESP32-P4-WIFI6-DEV-KIT (on its wiki page for this board) that
   labels every pin on the 40-pin header. Checking `config.h`'s four pin
   numbers against it:
   - `kCamSccbSclPin=8` / `kCamSccbSdaPin=7` -- **confirmed correct**. The
     diagram itself labels these two "SCL"/"SDA"; this is the board's
     documented default I2C pair.
   - `kMotorPwmPin`/`kMotorDirPin`, originally `40`/`41` -- **confirmed
     wrong**. Those two aren't on the 40-pin header at all; the same
     diagram (cross-checked against this board's SD-card pin list) shows
     GPIO40/41 are used internally for the onboard microSD card slot's
     data lines. Wiring the motor there would have shared the Cytron's
     PWM/DIR signals with the SD card interface. Reassigned to `4`/`5`,
     which the diagram confirms are plain, unused header pins -- not I2C,
     not UART (37/38, silkscreened TXD/RXD), not the SD card bus.
     `wiring-guide.html`'s diagram, connection table, and callout are
     updated to match.

   If you ever build this on a *different* P4 board, don't assume these
   four numbers carry over -- re-check that board's own pinout diagram
   the same way, the way this one turned out to matter.

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
