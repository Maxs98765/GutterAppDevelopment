# Install guide

This is a complete walkthrough for building this project from a bare
board and a bare phone. It assumes no prior knowledge of this project --
if a term isn't explained, it's explained a few lines above or below
where it's used.

**What you're building:** a camera + rotating-arm rig, controlled from an
Android phone over WiFi. One ESP32-P4 board does everything -- there is
no separate microcontroller. Read `README.md` first for the one-paragraph
overview if you haven't already.

**Read `wiring-guide.html` in a browser alongside this document.** It
shows every wire; this document tells you when to make each connection
and how to verify it worked.

---

## Part 0 -- What you need

**Hardware:** every part in `BOM.xlsx`, assembled per `wiring-guide.html`.
Don't substitute parts -- this firmware was written for the exact camera
and motor driver in that list, and a different part (a different camera
sensor, a different motor driver's control scheme) will not work without
code changes.

**A computer** (Windows, macOS, or Linux) with:
- A USB port and cable to the ESP32-P4 board (for flashing/monitoring)
- Internet access (the build downloads components the first time)

**Software to install, in this order:**

1. **Python 3.9+** -- ESP-IDF's installer needs it. Most Mac/Linux
   machines already have it; on Windows, get it from python.org and check
   "Add to PATH" during install.
2. **ESP-IDF v5.4 or newer** -- Espressif's toolchain for this chip.
   Follow Espressif's official "Get Started" guide for your OS at
   `https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/get-started/`
   -- it walks through installing the compiler toolchain and the `idf.py`
   command. Confirm it worked by running `idf.py --version` in a new
   terminal; it should print 5.4 or higher.
3. **Qt 6.5 or newer, with the Android kit** -- get the Qt online
   installer from qt.io, and during setup check the box for "Qt for
   Android" (this pulls in a matching Android SDK/NDK automatically --
   you do not need to install those separately). This is only needed to
   build the phone app, not the firmware.
4. **An Android phone**, Android 9 (Pie) or newer, with USB debugging
   enabled if you want to install the app via a cable (Settings ->
   About phone -> tap "Build number" 7 times -> Developer options ->
   USB debugging). You can also just copy the built APK file to the
   phone and install it directly, which skips this step.

---

## Part 1 -- Wire the hardware

Do this with everything unpowered. Follow `wiring-guide.html` for the
physical connections; the table below is the same information in text
form, matching the pin names used in the firmware's `main/config.h`.

| Connection | Pin | Notes |
|---|---|---|
| Camera ribbon cable | the board's CSI connector | Ribbon has a "this side up" convention -- check the blue/silver stiffener faces the correct way per the connector's own latch markings. A reversed ribbon is the single most common reason a camera is never detected. |
| Cytron MD13S "PWM" | ESP32-P4 GPIO 40 | `config.h`: `kMotorPwmPin`. **Verify against your board's actual pinout before wiring** -- see the callout below. |
| Cytron MD13S "DIR" | ESP32-P4 GPIO 41 | `config.h`: `kMotorDirPin`. Same caveat. |
| Cytron MD13S logic GND | ESP32-P4 GND | Required -- shared ground between the board and the driver. |
| Motor red wire | Cytron MD13S "M1+" (or M1A -- check your unit's silkscreen) | Polarity is arbitrary; if the arm runs backwards on first test, swap this and the black wire rather than re-wiring the control pins. |
| Motor black wire | Cytron MD13S "M1-" (M1B) | |
| Cytron MD13S VIN / GND (power in) | the 12V rail (through a fuse) | 6-30V input, up to 13A -- this is separate from the logic GND above, though both grounds must ultimately tie together. |
| Camera SCCB (I2C control bus) | ESP32-P4 GPIO 7 (SDA) / GPIO 8 (SCL) | `config.h`: `kCamSccbSdaPin`/`kCamSccbSclPin`. Same verify-before-wiring caveat as the motor pins. On many boards this bus is already wired internally between the CSI connector and the main chip -- check your board's documentation before assuming you need to add these wires yourself. |
| 12V-5V buck converter output | ESP32-P4 board's 5V/USB-C power input | Powers the board itself. |
| Solar panel -> charge controller -> battery -> main switch -> fuses -> (motor driver / buck converter) | | Full power-system diagram in `wiring-guide.html`. |

> **Before you wire the camera and motor pins:** `kCamSccbSdaPin`,
> `kCamSccbSclPin`, `kMotorPwmPin`, and `kMotorDirPin` in `main/config.h`
> are this project's best inference for a P4 dev-kit-style board, not
> numbers read off your specific board's schematic. Check your board's
> own documentation/silkscreen for these four pins before wiring, and if
> they differ, edit `main/config.h` to match your board -- everything
> else in the firmware reads from those constants, so a one-line edit
> there is all a different pin assignment needs.

---

## Part 2 -- Configure and build the firmware

1. Open a terminal in this folder (the one containing this file and
   `CMakeLists.txt`).

2. Set the build target once:

   ```sh
   idf.py set-target esp32p4
   ```

3. Open `main/config.h` and fill in your WiFi network:

   ```cpp
   inline constexpr char kStaSsid[] = "YOUR_WIFI";
   inline constexpr char kStaPass[] = "YOUR_PASSWORD";
   ```

   Leaving `kStaSsid` blank is also valid -- the board will instead create
   its own WiFi network named `servo-rig` (password `servo1234`) that the
   phone can join directly. Useful if there's no WiFi network on site.

4. Confirm the four pin numbers from Part 1's wiring table match your
   physical wiring (edit `main/config.h` if not).

5. Build:

   ```sh
   idf.py build
   ```

   The first build downloads several dependencies automatically
   (`esp_hosted`, `esp_video`, `esp_cam_sensor`, and a third-party IMX708
   camera driver -- see `main/idf_component.yml`) and can take several
   minutes. **If the build fails here**, read the error carefully -- the
   comments in `main/idf_component.yml`, `main/mipi_cam.cpp`, and
   `CHANGES.md` each flag specific version-compatibility risk points that
   could not be confirmed without a physical build.

6. Connect the board over USB and flash it:

   ```sh
   idf.py -p <PORT> flash monitor
   ```

   Replace `<PORT>` with the board's serial port (e.g. `/dev/ttyACM0` on
   Linux, `/dev/cu.usbmodemXXXX` on macOS, `COM3` on Windows -- if you're
   not sure which, unplug the board, run the command that lists ports for
   your OS, plug it back in, and see what appeared).

7. Watch the boot log. You're looking for lines like:

   ```
   net: bringing up the C6 co-processor over SDIO
   net: co-processor firmware X.Y.Z
   net: co-processor ready
   net: reachable at http://<some IP>/
   motor: ready: pwm=gpio40 dir=gpio41, ...
   cam: capturing JPEG directly from the ISP pipeline   (or: capturing RGB565 and encoding...)
   hub: hub ready
   ```

   **If you instead see the co-processor firmware reported as `0.0.0`**,
   the C6 WiFi co-processor's own firmware is missing and WiFi will never
   come up no matter what else is right. See "The WiFi co-processor
   (C6) has no firmware" below. Many of these boards ship with this
   firmware already installed from the factory -- try booting first
   before doing anything about this.

8. Press Ctrl+] to exit the serial monitor once you've confirmed the log
   looks healthy.

---

## Part 3 -- Build and install the Android app

1. Open a terminal in this folder's `app/` subfolder.

2. Build:

   **Using Qt Creator** (simpler): open `app/CMakeLists.txt` in Qt
   Creator, pick the Android Qt 6 kit when prompted, and click Run with
   the phone connected over USB (with USB debugging on) -- this builds,
   installs, and launches the app in one step.

   **From a terminal:**

   ```sh
   ~/Qt/6.5.3/android_arm64_v8a/bin/qt-cmake -S . -B build-android
   cmake --build build-android --target apk
   ```

   (Adjust the Qt path to match your actual install location and
   version.) This produces an `.apk` file under `build-android/`. Copy it
   to the phone and open it to install (you may need to allow "install
   unknown apps" for whichever app you copied it with), or install it
   directly with `adb install path/to/app.apk` if the phone is connected
   over USB.

3. Open the app on the phone.

4. Connect the phone to the rig's network: either join the same WiFi
   network the rig joined (see the board's boot log for its IP address),
   or, if you left `kStaSsid` blank, join the phone to the `servo-rig`
   WiFi network the board created itself (password `servo1234`) -- the
   rig is then at `192.168.4.1`.

5. In the app's settings (the drawer/gear icon), enter the rig's address
   -- either the IP address from the boot log, or `192.168.4.1` if you
   joined the rig's own network.

6. You should see the live camera feed appear. Tap the rotate button --
   the arm should sweep to 120°, hold, then return.

---

## Part 4 -- Verifying everything without the app

Useful for isolating whether a problem is the app, the network, or the
rig itself. With a phone or computer on the same network as the rig,
open a web browser to `http://<rig-ip>/` -- this loads a plain built-in
control page (no app needed) with the same rotate/home buttons and the
live video feed. If this page works but the app doesn't, the problem is
specific to the app/phone; if this page doesn't load either, the problem
is the rig or the network.

Individual routes, useful for scripting or `curl`:

| Route | Effect |
|---|---|
| `GET /stream` | multipart MJPEG video |
| `GET /snapshot.jpg` | one JPEG frame |
| `GET /rotate` | rotate to 120°, hold, return |
| `GET /rotate?hold=4000` | same, with a one-shot 4 s hold |
| `GET /home` | return to 0 immediately |
| `GET /hold?ms=10000` | change the default hold duration |
| `GET /status` | JSON: state, angle, ms remaining, camera health |
| `GET /` | the plain control page above |

---

## Troubleshooting

**Board won't connect over USB / `idf.py` can't find the port.** Install
your OS's USB-serial driver for the board's USB-to-UART chip if it's not
auto-detected (varies by manufacturer -- check the board's own product
page). Try a different USB cable; some are power-only.

**Build fails pulling in `esp_video`/`esp_cam_sensor`/the IMX708 driver.**
Check your internet connection and that ESP-IDF's component registry
isn't blocked by a firewall. If it fails with a version-compatibility
error specifically, see `CHANGES.md`'s "Verify before you flash" section
-- this is the single riskiest part of this build and the comments in
`main/mipi_cam.cpp` explain exactly what to check.

**Boot log never shows "co-processor ready" / WiFi never comes up.** This
is the C6 co-processor, not the P4 you just flashed. Its own firmware
lives in a separate flash region (`slave_fw` in `partitions.csv`) and is
not touched by the `idf.py flash` command above. Many of these boards
ship from the factory with this firmware already installed, so try
booting first. If the log explicitly reports firmware version `0.0.0`,
it's genuinely missing and needs to be built (from Espressif's
`esp-hosted-mcu` "slave" example project, matched to the same
`esp_hosted` version pinned in `main/idf_component.yml`) and flashed to
that partition separately with `esptool.py`. This is a real gap in this
project as handed to you -- there was no way to hand over a working
firmware binary sight-unseen and have any confidence it was genuine and
version-matched, so this one step may take real effort if your board
needs it.

**Camera never shows a frame / log shows a capture error.** Check the
ribbon cable is fully seated and the right way round (see Part 1). If the
log shows the sensor was never detected at all, double check
`kCamSccbSdaPin`/`kCamSccbSclPin` in `main/config.h` against your board.

**Arm rotates the wrong direction.** Flip `kMotorDirForwardIsHigh` in
`main/config.h` (true <-> false) and reflash -- no rewiring needed.

**Arm over/undershoots 120°.** There's no position sensor; the timing is
computed from the motor's rated speed (`kMotorRpm` in `main/config.h`,
16 RPM from the part's label). Real speed varies with load and supply
voltage. Measure the arm's actual travel with a protractor over a few
cycles and adjust `kMotorRpm` (lower it if the arm is overshooting --
moving faster than 16 RPM under your load -- raise it if undershooting),
then reflash.

**App shows "Cannot reach the rig".** Confirm the address entered in the
app's settings, confirm the phone is on the same network as the rig (or
joined the rig's own `servo-rig` network), and try loading
`http://<rig-ip>/status` in the phone's own browser to isolate whether
it's the app or the network/rig.

**Everything builds and flashes but the board resets in a loop.** Almost
always power -- the motor's stall current can brown out an
undersized/shared supply. Confirm the motor driver has its own fused 12V
feed, separate from the board's 5V supply, as shown in
`wiring-guide.html`.
