#pragma once

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
//  Every knob for the ESP32-P4 hub.
//
//  Changed from the S3 version:
//   - the P4 has no WiFi radio, so the radio lives on an ESP32-C6
//     co-processor reached over SDIO (see net.cpp)
//   - the camera driver is espressif/usb_host_uvc, not usb_stream, which
//     is S2/S3 only
// ---------------------------------------------------------------------------

namespace cfg {

// ---- WiFi (served by the C6 co-processor) ---------------------------------
// Leave kStaSsid empty to skip joining and go straight to access-point mode.
inline constexpr char kStaSsid[] = "YOUR_WIFI";
inline constexpr char kStaPass[] = "YOUR_PASSWORD";

// Fallback AP. The phone joins this and the hub is at 192.168.4.1.
inline constexpr char kApSsid[] = "servo-rig";
inline constexpr char kApPass[] = "servo1234";   // 8+ chars, or "" for open

inline constexpr int kStaJoinRetries = 6;

// ---- USB camera -----------------------------------------------------------
// Set false to bring the servo half up without the camera attached.
inline constexpr bool kCameraEnabled = true;

// Fill these in from the camera probe's output. Do not guess: the probe
// prints a block you can copy directly.
//
// kFrameRate 0 asks for the camera's own default rate, which is sometimes
// the only rate a badly-behaved camera honours.
inline constexpr uint16_t kFrameWidth  = 320;
inline constexpr uint16_t kFrameHeight = 240;
inline constexpr float    kFrameRate   = 15.0f;

// true  = the camera sends MJPEG, which is re-streamed as-is
// false = the camera sends uncompressed YUY2, which has to be encoded
//         before it goes over WiFi (see kUseHardwareJpeg)
inline constexpr bool kCameraSendsMjpeg = true;

// Only consulted when kCameraSendsMjpeg is false. Uses the P4's hardware
// JPEG encoder; software encoding at any useful resolution is hopeless.
inline constexpr bool kUseHardwareJpeg  = true;
inline constexpr int  kJpegQuality      = 80;

// Biggest single frame we will hold. The probe reports the real maximum;
// leave about 25% headroom above it.
inline constexpr size_t kMaxFrameBytes = 48 * 1024;

// USB transfer sizing. Larger URBs mean fewer interrupts and more memory.
// 0 for kUvcFrameSize means "use dwMaxVideoFrameSize from negotiation".
inline constexpr size_t kUvcFrameSize   = 0;
inline constexpr int    kUvcFrameBufs   = 3;
inline constexpr int    kUvcUrbs        = 4;
inline constexpr size_t kUvcUrbSize     = 16 * 1024;

// ---- Link to the Arduino Uno ---------------------------------------------
// Check these against your board's pinout before wiring. Avoid the USB and
// SDIO pins; on most P4 boards the C6 occupies a fixed SDIO block.
inline constexpr int kUnoTxPin = 37;   // -> Uno D10 (SoftwareSerial RX)
inline constexpr int kUnoRxPin = 38;   // <- Uno D11 (SoftwareSerial TX), VIA DIVIDER
inline constexpr int kUnoBaud  = 9600; // must match LINK_BAUD in uno_servo.ino

inline constexpr int kLinkReplyTimeoutMs = 400;

// ---- Servo behaviour ------------------------------------------------------
// The hold duration really lives in uno_servo.ino (HOLD_MS). Non-zero here
// overrides it on every rotate request instead. 10 s = 10000.
inline constexpr uint32_t kHoldOverrideMs = 0;

// ---- HTTP -----------------------------------------------------------------
inline constexpr uint16_t kHttpPort   = 80;
inline constexpr char     kMdnsName[] = "servo";

} // namespace cfg
