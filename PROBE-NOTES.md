# Camera probe and P4 hub notes

Two projects here:

- `firmware/camera_probe/` — answers "what can this camera actually do", standalone
- `firmware/esp32p4_hub/` — the hub, reworked for the P4's C6 radio and the native UVC driver

The Uno sketch and the Android app are unchanged. Neither cares which board
is upstream of them.

## Run the probe first

```sh
cd firmware/camera_probe
idf.py set-target esp32p4
idf.py build flash monitor
```

Leave it alone until it prints "Probe finished". The quick sweep is 27
combinations at four seconds each, so about three minutes. Set
`kQuickSweep = false` in `main/probe_config.h` for the exhaustive 120-combination
run if the quick one finds nothing.

### Reading the table

```
format   resolution  asked  frames    fps  minlen  maxlen bad err ovf unf  verdict
MJPEG     320x240    15         61   15.2   11204   19880   0   0   0   0  works
```

The columns that matter:

- **frames** and **fps** — zero frames with a successful open is a transfer-layer problem, not a format problem
- **bad** — frames that failed the JPEG start/end marker check. Non-zero means the camera is producing malformed MJPEG, which the driver happily accepts and which then looks like a bug in your app. This column is the reason the probe exists.
- **ovf** / **unf** — buffer too small, or the host not keeping up. Both are fixable in config.
- **err** — USB transfer errors. A handful is normal; a steady stream is not.

At the end it prints the best working combination as a paste-ready block for
`esp32p4_hub/main/config.h`, and a short diagnosis if nothing worked.

### Two things to check before concluding the camera is dead

**Your driver version.** `usb_host_uvc` 2.3.0 fixed an abort on an unexpected
EoF flag in bulk transfers. That bug presents exactly as "enumerates, opens,
dies during frame transfer" — your symptom, verbatim. Check what you actually
resolved to:

```sh
grep -A2 usb_host_uvc dependencies.lock
```

If it is below 2.3.0, that is very likely your whole problem. The manifest
here pins `^2.3.0`; delete `dependencies.lock` and `managed_components/` and
rebuild to force a re-resolve.

**Which USB controller you are on.** The P4 has two USB 2.0 OTG peripherals,
one High-Speed and one Full-Speed, and only one can be host at a time in the
current software. An old webcam is almost certainly a Full-Speed device, and
pointing the High-Speed host at one exercises much less well-tested paths.
The selection lives in `idf.py menuconfig` — the symbol has moved between IDF
versions, so look under Component config for the USB-OTG or USB Host entries
rather than trusting a name from me. Worth one run on each controller.

Do **not** add a hub to work around it. The external hub driver has no
Transaction Translator layer, so Full-Speed and Low-Speed devices behind a
hub on a High-Speed host are unsupported. If you need more current, use a
powered Y-cable that feeds VBUS directly.

### If the YUY2 rows pass and the MJPEG rows do not

That is a common outcome on old cameras and it is good news, not bad. Set in
`esp32p4_hub/main/config.h`:

```cpp
inline constexpr bool kCameraSendsMjpeg = false;
inline constexpr bool kUseHardwareJpeg  = true;
```

The hub then pulls uncompressed frames and compresses them with the P4's
hardware JPEG engine before they go over WiFi. Single-digit milliseconds per
frame, and it costs you nothing on the S3's old bottleneck because High-Speed
USB has bandwidth to spare. Software encoding is not an option — don't try.

## What changed in the hub for the P4

**WiFi moved off-chip.** The P4 has no radio. `net.cpp` brings up the SDIO
link to the ESP32-C6 co-processor and then the ordinary `esp_wifi_*` calls are
forwarded across it by `esp_wifi_remote`. The ordering is not optional:

```
esp_hosted_init() -> esp_hosted_connect_to_slave() -> nvs -> netif
  -> event loop -> esp_wifi_init()
```

Touching the WiFi stack before the hosted link is up is the standard way to
make a new P4 board look like it has broken WiFi.

**Check the C6's firmware version on first boot.** The log prints it. If it
reports `0.0.0` the co-processor has never been flashed and nothing will work;
some early board production runs shipped that way. Flash it once:

```sh
esptool.py --chip esp32p4 -p /dev/ttyACM0 write_flash --force 0x310000 slave_fw/network_adapter.bin
```

That offset matches the `slave_fw` partition in `partitions.csv` here. It
persists across app reflashes.

**The camera driver is different.** `usb_stream`, which the S3 firmware used,
only supports the S2 and S3. `uvc_source.cpp` is rewritten against
`espressif/usb_host_uvc`, whose API is native to the ESP-IDF USB Host Library
and looks nothing like the old libuvc-based one.

**Check the UART pins.** `kUnoTxPin` and `kUnoRxPin` in `config.h` are
placeholders. P4 boards differ, and the C6 occupies a fixed SDIO block you
must not collide with. Confirm against your board's schematic before wiring.
Everything else about the serial link is unchanged, including the divider on
the Uno's TX.

## Files

```
camera_probe/main/main.cpp          the sweep, the table, the diagnosis
camera_probe/main/probe_config.h    sweep length and buffer sizing
esp32p4_hub/main/net.cpp            C6 bring-up and WiFi (new)
esp32p4_hub/main/uvc_source.cpp     rewritten for usb_host_uvc
esp32p4_hub/main/jpeg_hw.cpp        hardware JPEG, for the YUY2 path (new)
esp32p4_hub/main/config.h           all hub settings
esp32p4_hub/main/app_main.cpp       startup ordering
esp32p4_hub/main/http_api.cpp       unchanged from the S3 build
esp32p4_hub/main/frame_store.cpp    unchanged
esp32p4_hub/main/uno_link.cpp       unchanged
```

## Two places the code may not compile against your versions

Both are flagged in comments where they appear, and both are small.

1. **The format enum names** in `camera_probe/main/main.cpp` (`UVC_VS_FORMAT_YUY2`, `UVC_VS_FORMAT_H264`). If one is missing from your driver version, open `managed_components/espressif__usb_host_uvc/include/usb/uvc_host.h`, find the enum, and delete the offending row from the table.
2. **The hardware JPEG API** in `jpeg_hw.cpp`. `esp_driver_jpeg` arrived in IDF 5.3 and its struct fields have shifted since. Set `HUB_ENABLE_HW_JPEG` to 0 in `jpeg_hw.h` to drop the whole path — the MJPEG route never calls it.
