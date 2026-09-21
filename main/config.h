#pragma once

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
//  Every knob for the ESP32-P4 hub -- Arduino-free build.
//
//  Rewritten to match ListofMaterials.xlsx exactly. Two hardware changes
//  from the earlier version of this file:
//
//   1. Camera: the BOM part is an Arducam Raspberry Pi Camera Module 3
//      (Sony IMX708 sensor) on a 15/22-pin CSI ribbon -- it plugs into the
//      board's MIPI-CSI connector, not a USB port. All of the old kUvc*/
//      kCameraSendsMjpeg USB settings are gone; see "Camera (MIPI-CSI)"
//      below. Driven by mipi_cam.cpp instead of uvc_source.cpp.
//
//   2. Motor: the BOM driver is a Cytron MD13S (2 control pins: PWM +
//      DIR), not an L298N (3 pins: ENA/IN1/IN2), and there is no Arduino
//      Uno anywhere in the BOM. The motor is now driven directly by this
//      board's own GPIOs -- see "Motor" below -- by motor.cpp, which
//      replaces uno_link.cpp AND the old uno_servo.ino sketch (that
//      sketch's whole timing state machine now runs here instead, since
//      there is no second microcontroller left to run it on).
//
//  WiFi/HTTP/mDNS settings are unchanged from the previous version of
//  this file.
// ---------------------------------------------------------------------------

namespace cfg {

// ---- WiFi (served by the C6 co-processor over SDIO -- see net.cpp) --------
// Leave kStaSsid empty to skip joining and go straight to access-point mode.
inline constexpr char kStaSsid[] = "YOUR_WIFI";
inline constexpr char kStaPass[] = "YOUR_PASSWORD";

// Fallback AP. The phone joins this and the hub is at 192.168.4.1.
inline constexpr char kApSsid[] = "servo-rig";
inline constexpr char kApPass[] = "servo1234";   // 8+ chars, or "" for open

inline constexpr int kStaJoinRetries = 6;

// ---- Camera (MIPI-CSI, Arducam IMX708) -------------------------------------
// Set false to bring the rig up without the camera attached.
inline constexpr bool kCameraEnabled = true;

// SCCB is the camera-control bus -- electrically I2C, used to talk to the
// IMX708's registers (exposure, gain, mode select, etc.), separate from
// the MIPI-CSI lanes that actually carry pixels.
//
// VERIFY these three against the Waveshare ESP32-P4-WIFI6's own
// schematic/pinout before building: they are this project's best
// inference (a typical SCCB pin choice on P4 dev kits), not read off your
// specific board's silkscreen or schematic. Same caution that applied to
// the old kUnoTxPin/kUnoRxPin guess.
inline constexpr int kCamSccbI2cPort = 0;
inline constexpr int kCamSccbSclPin  = 8;
inline constexpr int kCamSccbSdaPin  = 7;

// -1 = the board's CSI connector already supplies the sensor clock (typical
// on P4 dev-kit CSI headers). Set a GPIO here only if your board instead
// expects the MCU to generate XCLK for the camera.
inline constexpr int kCamXclkPin    = -1;
inline constexpr int kCamXclkFreqHz = 24 * 1000 * 1000;

// The Waveshare board's Pi-style 15-pin CSI connector is documented (by
// the third-party IMX708 driver's own notes) as wiring neither a reset
// nor a powerdown line to the sensor -- it free-runs off its own
// oscillator once powered -- so both stay disabled.
inline constexpr int kCamResetPin = -1;
inline constexpr int kCamPwdnPin  = -1;

inline constexpr int kMipiCsiDataLanes = 2;   // IMX708 on this board: 2 lanes

// Capture resolution/rate. 1920x1080 is IMX708's documented sweet spot on
// the P4 (2x2-binned RAW10 at roughly 28 fps); this starts lower to leave
// margin -- raise it once the rig is confirmed stable at this size.
inline constexpr uint16_t kFrameWidth  = 1280;
inline constexpr uint16_t kFrameHeight = 720;
inline constexpr float    kFrameRate   = 20.0f;

// Ask the ISP+hardware-JPEG pipeline to hand back frames already encoded
// as JPEG -- far cheaper than decoding RAW ourselves. If your installed
// esp_video version can't set this output format for the CSI pipeline,
// mipi_cam.cpp automatically falls back to requesting RGB565 and encoding
// it with the P4's hardware JPEG encoder (jpeg_hw.cpp, carried over
// unchanged from the old USB-camera build's uncompressed path).
inline constexpr bool kRequestJpegFromIsp = true;
inline constexpr int  kJpegQuality        = 80;

// Biggest single frame the pipeline is allowed to hand back. Leave real
// headroom above what kFrameWidth x kFrameHeight x kJpegQuality actually
// produces -- too tight and mipi_cam.cpp/jpeg_hw.cpp will drop or refuse
// frames rather than overrun the buffer.
inline constexpr size_t kMaxFrameBytes = 192 * 1024;

// Number of driver-owned capture buffers (V4L2 REQBUFS count). 2-3 is
// normally enough to absorb HTTP-server jitter without adding latency.
inline constexpr int kCamBufferCount = 3;

// ---- Motor (Cytron MD13S + BRINGSMART 12V/16RPM self-locking worm gear) ---
// Direct GPIO control -- no Arduino, no serial link, no divider circuit.
// Avoid the USB and SDIO pins; on most P4 boards the C6 co-processor
// occupies a fixed SDIO block. VERIFY against your board's pinout before
// wiring -- same caution as the camera pins above.
inline constexpr int kMotorPwmPin = 40;   // -> Cytron MD13S "PWM"
inline constexpr int kMotorDirPin = 41;   // -> Cytron MD13S "DIR"

// MD13S accepts a standard PWM speed input; this is comfortably inside its
// documented range and well clear of the LEDC peripheral's limits.
inline constexpr int kMotorPwmFreqHz = 20000;

// Drive at fixed full duty while moving -- this rig doesn't ramp speed,
// only times a fixed run, so 100 = full duty is the natural default.
// Lower this only if the arm's actual swing overshoots home/target too
// hard to control by timing alone.
inline constexpr int kMotorPwmDutyPercent = 100;

// True if the MD13S's DIR pin needs to be HIGH for "toward target"
// motion. Polarity here is arbitrary -- if the arm runs backwards the
// first time you power it up, flip this rather than re-wiring anything.
inline constexpr bool kMotorDirForwardIsHigh = true;

// The motor's rated output speed, from its label (BRINGSMART 12V,
// 16 RPM). This is the whole basis for timing a rotation -- there is no
// shaft position sensor, so a rotate command just runs the motor forward
// for however long the target angle SHOULD take at this speed, and
// trusts it. Real speed varies with load, supply voltage, and
// manufacturing tolerance, so this drifts over repeated cycles -- see
// motor.cpp's header comment for how to recalibrate it if the arm
// over/undershoots.
inline constexpr float kMotorRpm = 16.0f;

inline constexpr int kAngleHome   = 0;
inline constexpr int kAngleTarget = 120;   // degrees

// How long to sit at kAngleTarget before returning home. An HTTP
// request's ?hold=<ms> overrides this on that one rotate only.
inline constexpr uint32_t kHoldMs = 10000;

// How often the motor's internal state machine re-checks its timers.
inline constexpr int kMotorTickMs = 20;

// ---- HTTP -------------------------------------------------------------------
inline constexpr uint16_t kHttpPort   = 80;
inline constexpr char     kMdnsName[] = "servo";

} // namespace cfg
