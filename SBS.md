# SBS — Servo Rig: Step-By-Step Install & App Setup

This walks through, in order, everything needed to get the code in the `GC`
folder running on real hardware, and to get the phone app talking to it over
WiFi. Follow the steps in order — later steps assume earlier ones already
work, and skipping ahead is the most common way to end up debugging three
problems at once instead of one.

**Where things live now, after the folder cleanup:**

```
GC/
  app_main.cpp, config.h, net.cpp, uvc_source.cpp, jpeg_hw.cpp,   <- ESP32-P4 hub firmware
  idf_component.yml, partitions.csv, sdkconfig.defaults
  main.cpp, probe_config.h                                        <- camera probe firmware
  http_api.cpp, frame_store.cpp, uno_link.cpp                     <- more hub firmware (shared logic)
  PROBE-NOTES.md, README.md                                       <- background notes
  uno_servo/
    uno_servo.ino                                                 <- Arduino Uno sketch
  app/
    AndroidManifest.xml, CMakeLists.txt, Main.qml, config.h,
    mjpegclient.cpp, servolink.cpp, videosurface.cpp               <- the phone app
```

---

## Before you start: two known gaps to resolve first

Being upfront about these now saves confusion later, since both will cause a
build to fail partway through with errors that don't obviously point back
to "a file is missing."

1. **Header files (`.h`) are missing.** Every `.cpp` file above has a
   matching header file that declares its class or functions (for example
   `uvc_source.cpp` needs `uvc_source.h`, `servolink.cpp` needs
   `servolink.h`), and none of those header files are currently in the
   folder — only the `.cpp` files and the two `config.h` files survived
   whatever process exported this project. Nothing here will compile until
   those headers are rebuilt. If this hasn't been done yet, ask for the
   headers to be reconstructed before attempting Part 1 or Part 3 below.

2. **The C6 co-processor's own firmware file is missing.** Step 1.5 below
   requires a file called `network_adapter.bin` that is supposed to live in
   a `slave_fw/` folder, and that file isn't present anywhere in `GC`. It
   needs to be obtained separately (it ships as part of Espressif's
   ESP-Hosted / `esp_wifi_remote` release for the ESP32-C6) or built from
   that project's source. Without it, the P4 board's WiFi will never come
   up, no matter how correctly everything else is done.

Everything below assumes both gaps have been closed before you begin.

---

## Tools you need installed on your computer, before touching any hardware

| Tool | What it's for | Notes |
|---|---|---|
| Arduino IDE | flashing the Uno | any recent version; no extra libraries needed |
| ESP-IDF (v5.3 or newer) | building/flashing the ESP32-P4 hub and the camera probe | install via Espressif's official installer; **on Windows this adds a Start Menu shortcut called "ESP-IDF PowerShell" or "ESP-IDF Command Prompt" — you must use that shortcut (or run its `export.ps1`/`export.bat` script) every time before `idf.py` commands will work in a terminal.** A plain PowerShell/cmd window that hasn't run that export script will say `idf.py is not recognized`. |
| A USB cable / driver for the P4 board | flashing and monitoring | the board's UART bridge port, not the camera port (see Step 1.3) |
| Qt 6.5 or newer, with the Android component | building the phone app | Qt's own installer adds the Android SDK/NDK automatically when you select the Android kit during install |
| esptool.py | flashing the C6 co-processor's firmware once | installed automatically alongside ESP-IDF |

---

# Part 1 — Installing the firmware onto the hardware

### Step 1.1 — Flash the Arduino Uno, and confirm it on its own

1. Open `GC/uno_servo/uno_servo.ino` in the Arduino IDE.
2. Under **Tools → Board**, select **Arduino Uno**.
3. Under **Tools → Port**, select the COM port the Uno shows up as (check
   Windows Device Manager if you're not sure which one).
4. Click **Upload**.
5. Open **Tools → Serial Monitor**, set the baud rate in the bottom-right
   corner to **115200**.
6. Reset the Uno (press its reset button, or unplug/replug the USB cable).
   You must see the line:
   ```
   uno_servo ready
   ```
   If you see garbled text instead of that line, your baud rate is set
   wrong in the Serial Monitor — it must be 115200, not 9600 (9600 is the
   separate link speed to the ESP32, set later, and is not what the USB
   debug console uses).
7. **Do not continue to Step 1.2 until this line appears.** Everything
   after this step assumes the Uno itself is working correctly in
   isolation.

### Step 1.2 — Wire the servo to the Uno

1. Connect the servo's signal wire (usually orange or white) to Uno pin
   **D9**.
2. Power the servo from **its own separate 5V supply** — not from the
   Uno's own 5V pin or USB power. A servo straining against its end-stop
   draws far more current than the Uno can safely provide, and doing this
   wrong is the single most common cause of the Uno randomly resetting.
3. Connect the ground wire of the servo's power supply to the Uno's
   **GND** pin, so both share a common ground. This step is easy to
   forget and causes intermittent, hard-to-diagnose behavior if skipped.
4. Optional but recommended: add a 470 µF capacitor across the servo's
   power leads, close to the servo itself, to absorb current spikes.

### Step 1.3 — Set up and run the camera probe (before configuring the real hub firmware)

Do this even if you're confident about your camera's specs — the whole
point of this step is to stop you from guessing.

1. Open the Windows Start Menu and launch **ESP-IDF PowerShell** (or open
   an ordinary terminal and run ESP-IDF's `export.ps1` script first).
2. In that terminal, navigate to the `GC` folder:
   ```
   cd C:\Users\apmai\OneDrive\Desktop\GC
   ```
3. Set the build target to the P4 chip:
   ```
   idf.py set-target esp32p4
   ```
4. Build, flash, and open the serial monitor in one command (plug the P4
   board in via its UART bridge port first, and make sure the USB camera
   is **not** plugged in yet):
   ```
   idf.py build flash monitor
   ```
5. Once flashing finishes and the board reboots, plug the USB camera into
   the board's USB host port.
6. Wait. The probe will print "waiting for a camera," then begin sweeping
   through combinations of format/resolution/frame rate automatically.
   The default "quick" sweep is 27 combinations at 4 seconds each — about
   3 minutes. Do not unplug anything or interrupt it.
7. When it prints `Probe finished. Save this log.`, scroll up and find the
   block that starts with `Paste this into the hub firmware:`. Copy that
   entire block down — you will need it in Step 1.4. If instead it prints
   "Nothing streamed cleanly," read the diagnosis text it prints
   immediately below that line before proceeding; it will tell you
   whether the issue is your driver version, USB power, or the camera
   itself.
8. Press **Ctrl+]** to exit the serial monitor when you're done reading
   the log.

### Step 1.4 — Configure the hub firmware with the probe's results

1. Open `GC/config.h` in a text editor.
2. Find these lines near the top of the `WiFi` section and fill in your
   actual network name and password:
   ```cpp
   inline constexpr char kStaSsid[] = "YOUR_WIFI";
   inline constexpr char kStaPass[] = "YOUR_PASSWORD";
   ```
   If you would rather the hub create its own WiFi hotspot instead of
   joining your home network, leave `kStaSsid` as an empty string `""`
   — the hub will then always start its own network named `servo-rig`
   with password `servo1234`, and be reachable at `192.168.4.1`.
3. Scroll to the `USB camera` section and replace the placeholder values
   with the exact numbers the probe printed for you in Step 1.3:
   ```cpp
   inline constexpr uint16_t kFrameWidth  = <value from probe>;
   inline constexpr uint16_t kFrameHeight = <value from probe>;
   inline constexpr float    kFrameRate   = <value from probe>;
   inline constexpr bool     kCameraSendsMjpeg = <true or false, from probe>;
   ```
   If the probe told you your camera only sends uncompressed YUY2 video
   (`kCameraSendsMjpeg = false`), also leave these two lines as-is — they
   turn on the P4's built-in hardware JPEG compressor, which is required
   in that case:
   ```cpp
   inline constexpr bool kUseHardwareJpeg  = true;
   inline constexpr int  kJpegQuality      = 80;
   ```
4. Scroll to the `Link to the Arduino Uno` section:
   ```cpp
   inline constexpr int kUnoTxPin = 37;
   inline constexpr int kUnoRxPin = 38;
   ```
   These two numbers are placeholders and are **not guaranteed to be
   correct for your specific P4 board** — different P4 boards wire their
   pins differently, and the C6 co-processor occupies a fixed set of pins
   you must avoid colliding with. Check your board's schematic/pinout
   diagram before wiring, and update these two numbers to match.
5. Save the file.

### Step 1.5 — Flash the ESP32-C6 co-processor's own firmware (P4 only, one-time)

The P4 chip has no WiFi radio of its own; a second chip (the C6) handles
WiFi, and it needs its own separate firmware flashed onto it once before
any of this will work.

1. Confirm you have the `network_adapter.bin` file described in
   "Before you start" above.
2. With the P4 board connected, run (adjust the COM port to match what
   Windows Device Manager shows for your board):
   ```
   esptool.py --chip esp32p4 -p COM5 write_flash --force 0x310000 slave_fw/network_adapter.bin
   ```
3. This only needs to be done once — it survives future reflashes of the
   main application firmware, because it lives in its own reserved section
   of flash storage (the `slave_fw` partition).
4. You'll confirm this worked in Step 1.7, when the boot log prints the
   co-processor's firmware version rather than `0.0.0`.

### Step 1.6 — Wire the camera and the Uno link to the P4 board

1. **Camera:** plug the USB camera into the P4 board's USB host port
   (typically its native USB-OTG jack; check your specific board's
   documentation for which physical port is the host port, since some P4
   dev boards expose more than one USB connector and only one of them
   works in host mode).
2. **Serial link to the Uno:** using the pin numbers you confirmed in
   Step 1.4:
   - P4 **TX pin** (`kUnoTxPin`) → Uno **D10** — direct connection, no
     divider needed in this direction.
   - Uno **D11** → P4 **RX pin** (`kUnoRxPin`) — **this direction needs a
     voltage divider** (roughly 10 kΩ in series, then 15 kΩ to ground) to
     step the Uno's 5V signal down to a safe ~3.0V for the P4's 3.3V-only
     input. Skipping this divider risks damaging the P4 pin.
   - Connect **GND to GND** between the two boards. Required, and easy to
     miss.
3. Double check nothing here conflicts with the C6 co-processor's SDIO
   pins, which are fixed on most P4 boards — consult your board's
   schematic.

### Step 1.7 — Build and flash the real hub firmware, and verify boot-up

1. Back in the ESP-IDF terminal, in the `GC` folder:
   ```
   idf.py build flash monitor
   ```
2. Watch the boot log closely, in this order:
   - `bringing up the C6 co-processor over SDIO` — should succeed. If you
     see `the C6 has no ESP-Hosted firmware`, go back to Step 1.5 — it
     was not flashed correctly.
   - `co-processor firmware <version>` — should show a real version
     number, not `0.0.0`.
   - Either `joining "<your network>"` followed by `station ip: ...`, or
     (if you left the WiFi fields blank) `access point "servo-rig" up`.
   - `arduino says: <something>` — should not say "(nothing -- check the
     divider, the grounds, and the baud rate)". If it does, recheck Step
     1.6's wiring before moving on.
   - `opening the camera at <width>x<height>` followed eventually by
     `streaming`.
   - Finally: `hub ready`.
3. Leave it running. Every 10 seconds it prints a status line showing
   whether the camera is connected, how many frames it's captured, and
   how much memory is free — useful for confirming things stay stable
   over time, not just at boot.
4. Note the IP address printed in the log (or use `192.168.4.1` if it's
   running its own access point) — you'll need this in Part 2.

---

# Part 2 — Setting up and connecting the phone app over WiFi

### Step 2.1 — Install the build tools for the app

1. Install **Qt 6.5 or newer** using Qt's official online installer.
2. During installation, make sure the **Android** component is checked
   for your Qt version (it will also pull down the matching Android
   SDK/NDK automatically — this can take a while and several GB of
   downloads, so budget time for it).
3. Also install **Qt Creator** if it wasn't already included (it usually
   is, by default).

### Step 2.2 — Set the app's default connection settings (optional but recommended)

1. Open `GC/app/config.h` in a text editor.
2. Find this line and set it to match how you plan to connect (see Step
   2.5 for the two options):
   ```cpp
   inline constexpr char kDefaultHost[] = "192.168.4.1";
   ```
   Leaving it as `192.168.4.1` is correct if your phone will join the
   hub's own hotspot. If you instead plan to have both the phone and the
   hub on your regular home WiFi, you can leave this default alone and
   just type the correct address into the app after installing it (Step
   2.6) — you don't have to rebuild the app just to change this.
3. Save the file. (You can skip this step entirely and just type the
   address into the app's settings screen after installing it, if you'd
   rather not rebuild for this.)

### Step 2.3 — Build the app

Using Qt Creator (recommended for a first build):

1. Open Qt Creator.
2. **File → Open File or Project**, then select
   `GC/app/CMakeLists.txt`.
3. When prompted to configure the project, select the **Android Qt 6**
   kit (not desktop).
4. Click the green **Run** (play) button at the bottom-left. This builds
   the app and, if a device or emulator is connected/selected, installs
   it directly.

Or, from a terminal, using the export script Qt's installer sets up for
its own environment (adjust the path/version to match what you installed):

```
cd GC\app
C:\Qt\6.5.3\android_arm64_v8a\bin\qt-cmake -S . -B build-android
cmake --build build-android --target apk
```

This produces an installable `.apk` file under `build-android/`.

### Step 2.4 — Install the app on your phone

1. On your Android phone, go to **Settings → Security** (naming varies by
   phone) and enable **install from unknown sources** for whichever app
   you'll use to open the `.apk` file (this is only needed because the
   app isn't coming from the Play Store).
2. Transfer the `.apk` file to your phone (via USB cable, email to
   yourself, cloud storage, or having Qt Creator install it directly if
   the phone was plugged in during Step 2.3).
3. Open the `.apk` file on the phone and tap **Install**.
4. Launch the app — it will be labeled **Servo rig**.

### Step 2.5 — Connect your phone to the same network as the hub

You have two options; pick whichever matches how you configured
`kStaSsid` back in Step 1.4.

- **Option A — hub creates its own hotspot** (you left `kStaSsid` blank):
  on your phone's WiFi settings, join the network named **servo-rig**
  (password **servo1234**, unless you changed it in `config.h`). The hub
  is always at **192.168.4.1** in this mode.
- **Option B — hub joins your home WiFi**: make sure your phone is
  connected to that **same** home WiFi network. The hub's actual IP
  address will be whatever your router assigned it — check the serial
  log from Step 1.7, or try `http://servo.local/` in a phone browser
  first (see Step 2.6) since the hub also announces itself under that
  name.

### Step 2.6 — Confirm the connection works, independent of the app

Before trusting the app itself, prove the network path works using just
a browser:

1. On your phone, open a web browser.
2. Go to `http://192.168.4.1/` (Option A) or `http://servo.local/` or the
   IP address from the serial log (Option B).
3. You should see a simple page titled **Servo rig** with a live camera
   image, a **Rotate and hold** button, and a **Return to 0** button.
4. Tap **Rotate and hold** and confirm the servo physically moves. This
   single test confirms the WiFi path, the HTTP server, the camera feed,
   and the serial link to the Uno are all working — if this works, the
   app will work.
5. If this page won't load at all: your phone is not actually on the same
   network as the hub — recheck Step 2.5.
6. If the page loads but the camera image is blank or frozen: the WiFi
   and HTTP layers are fine, but the camera itself has a problem — revisit
   Step 1.3/1.4.
7. If the page loads and the image works, but the button does nothing:
   the WiFi/HTTP/camera layers are all fine, and the problem is isolated
   to the serial link to the Uno — recheck Step 1.6's wiring, and the
   `arduino says:` line from Step 1.7.

### Step 2.7 — Point the app at the hub

1. Open the **Servo rig** app on your phone.
2. Tap the small gear icon in the top-right corner of the camera view.
   This opens a settings panel from the bottom of the screen.
3. In the text field labeled **"Address of the ESP32-S3, host or
   host:port"**, type the same address you used in Step 2.6
   (`192.168.4.1`, `servo.local`, or the numeric IP — all work).
4. Tap **Apply**. The video feed should restart and the "Rig not
   responding" text at the bottom of the main screen should change to
   show the servo's current angle.
5. Optional: drag the slider in the settings panel to set how long the
   servo holds its rotated position (1–60 seconds) before tapping
   **Apply**, or tap **Save to the Arduino** to make that hold time the
   new permanent default even after the app is closed.
6. Tap the large **Rotate** button on the main screen and confirm the
   servo moves, the countdown ring drains around the button, and it
   returns to its resting position afterward.

At this point the full chain — camera → hub firmware → WiFi → phone app
— is installed, wired, configured, and confirmed working end to end.

---

## Quick troubleshooting reference

| Symptom | Most likely cause | Where to look |
|---|---|---|
| Build fails with "unknown type" or "no declaration" errors | missing header files | see "Before you start," item 1 |
| P4 board's WiFi never comes up, log shows co-processor version `0.0.0` | C6 not flashed | Step 1.5 |
| `arduino says: (nothing...)` in the boot log | serial wiring, divider, or baud mismatch | Step 1.6 |
| Camera opens but delivers zero frames | USB driver version, wrong USB controller selected, or a hub in the cable path | Step 1.3's probe diagnosis, PROBE-NOTES.md |
| App shows "Rig not responding" | wrong address, or phone on the wrong network | Step 2.5, then Step 2.6 |
| App's video is blank/frozen but rig responds to Rotate | camera-side issue, not WiFi/app | Step 1.3/1.4 |
| Servo buzzes or stalls at the end of its travel | commanded past its mechanical limit | lower `SERVO_MAX_US` in `uno_servo.ino`, in steps of 50 |
