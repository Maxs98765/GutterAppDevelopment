#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
//  Every knob for the ESP32-S3 hub lives here.
// ---------------------------------------------------------------------------

namespace cfg {

// ---- WiFi -----------------------------------------------------------------
// Leave kStaSsid empty to skip joining and go straight to access-point mode.
inline constexpr char kStaSsid[] = "YOUR_WIFI";
inline constexpr char kStaPass[] = "YOUR_PASSWORD";

// Fallback AP, used if the join above fails. The phone connects to this and
// the hub is at 192.168.4.1. Password needs 8+ chars, or "" for an open net.
inline constexpr char kApSsid[] = "servo-rig";
inline constexpr char kApPass[] = "servo1234";

inline constexpr int kStaJoinRetries = 6;

// ---- USB camera -----------------------------------------------------------
// Set false to bring the servo half up first, without the camera attached.
inline constexpr bool kCameraEnabled = true;

// Your camera must advertise MJPEG at this exact resolution and rate, or the
// stream will not negotiate. Use kResolutionAny to accept whatever the camera
// offers first, then read the log to see what you actually got.
inline constexpr bool     kResolutionAny = false;
inline constexpr uint16_t kFrameWidth    = 320;
inline constexpr uint16_t kFrameHeight   = 240;
inline constexpr uint32_t kFrameRate     = 15;

// Biggest single JPEG frame we will accept, in bytes. If the log shows
// "frame too large", raise this. 320x240 MJPEG is usually well under 30 KB.
inline constexpr size_t kMaxFrameBytes = 48 * 1024;

// USB transfer scratch buffers. Two are needed for double buffering.
inline constexpr size_t kXferBufferBytes = 48 * 1024;

// ---- Link to the Arduino Uno ---------------------------------------------
// Avoid GPIO19/20 (they are the USB D-/D+ lines) and GPIO26..32 (flash/PSRAM).
inline constexpr int kUnoTxPin = 17;   // -> Uno D10 (SoftwareSerial RX)
inline constexpr int kUnoRxPin = 18;   // <- Uno D11 (SoftwareSerial TX), VIA DIVIDER
inline constexpr int kUnoBaud  = 9600; // must match LINK_BAUD in uno_servo.ino

inline constexpr int kLinkReplyTimeoutMs = 400;

// ---- Servo behaviour ------------------------------------------------------
// The hold duration really lives in uno_servo.ino (HOLD_MS). Set a non-zero
// value here to override it on every rotate request instead, which is handy
// if you would rather tweak the number on this side. 10 s = 10000.
inline constexpr uint32_t kHoldOverrideMs = 0;

// ---- HTTP -----------------------------------------------------------------
inline constexpr uint16_t kHttpPort    = 80;
inline constexpr char     kMdnsName[]  = "servo";

} // namespace cfg
