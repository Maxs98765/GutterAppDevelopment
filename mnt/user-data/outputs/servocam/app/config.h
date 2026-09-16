#pragma once

// ---------------------------------------------------------------------------
//  Every knob for the Android app lives here. The host and the hold duration
//  can also be edited at runtime from the app's settings panel; these are the
//  values it starts with.
// ---------------------------------------------------------------------------

namespace cfg {

// Where the ESP32-S3 hub lives, host or host:port, no scheme.
//   "192.168.4.1"  the hub's own access point
//   "servo.local"  when your router passes mDNS through
inline constexpr auto kDefaultHost = "192.168.4.1";

// How long the servo sits at the target angle before returning, in
// milliseconds. This is the number you asked to be freely tweakable.
//
// Set kSendHoldWithRotate to false and the app stops sending a duration, so
// HOLD_MS in uno_servo.ino becomes the single source of truth instead.
inline constexpr int  kDefaultHoldMs      = 10000;
inline constexpr bool kSendHoldWithRotate = true;

// Shown in the readout. Keep these matched to ANGLE_HOME and ANGLE_TARGET in
// uno_servo.ino, which is what actually drives the servo.
inline constexpr int kHomeAngle   = 0;
inline constexpr int kTargetAngle = 180;

// Polling. kStatusPollMs asks the rig what it is doing; kUiTickMs only
// animates the countdown smoothly between those answers.
inline constexpr int kStatusPollMs     = 400;
inline constexpr int kUiTickMs         = 50;
inline constexpr int kRequestTimeoutMs = 3000;

// Video. The hub serves multipart MJPEG at /stream. If the stream proves
// unreliable on your network, set kUseSnapshotFallback to true and the app
// polls /snapshot.jpg at kSnapshotFps instead, which is choppier but far
// more forgiving.
inline constexpr auto kStreamPath         = "/stream";
inline constexpr auto kSnapshotPath       = "/snapshot.jpg";
inline constexpr bool kUseSnapshotFallback = false;
inline constexpr int  kSnapshotFps         = 8;

// Seconds without a frame before the video pane reports a stall and retries.
inline constexpr int kVideoStallSeconds = 4;

} // namespace cfg
