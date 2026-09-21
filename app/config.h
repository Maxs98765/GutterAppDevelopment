#pragma once

// ---------------------------------------------------------------------------
//  Every knob for the Servo Rig Android app lives here.
//
//  Reconstructed file: the original app/config.h was lost (both the hub
//  firmware and this app had a file named "config.h", and only one survived
//  when everything got flattened into a single folder). This version was
//  rebuilt from what servolink.cpp and mjpegclient.cpp actually reference,
//  matching the defaults implied by the project's README and by
//  uno_servo.ino's HOLD_MS. Double check the values against your own setup,
//  especially kDefaultHost.
//
//  IMPORTANT: this file must live in its own "app" folder, separate from the
//  hub firmware's config.h. The two projects are built by completely
//  different tools (CMake/Qt for this one, idf.py for the hub) and each
//  needs its own config.h with its own contents -- they cannot share a
//  folder or a filename.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace cfg {

// ---- connecting to the rig -------------------------------------------------

// Shown pre-filled in the app's settings drawer. "192.168.4.1" is correct
// when the phone joins the hub's own fallback access point ("servo-rig").
// If you normally join your home WiFi instead, set this to the hub's mDNS
// name ("servo.local") or its usual DHCP address.
inline constexpr char kDefaultHost[] = "192.168.4.1";

// How long to wait for any HTTP request (rotate/home/hold/status) before
// treating the rig as unreachable.
inline constexpr int kRequestTimeoutMs = 4000;

// ---- servo control ----------------------------------------------------------

// Matches HOLD_MS in uno_servo.ino (10 s). The app's settings slider runs
// 1000-60000 ms and overrides this once the user moves it.
inline constexpr int kDefaultHoldMs = 10000;

// If true, every /rotate request carries "?hold=<ms>" so the app is the
// source of truth for hold duration. Set false to let the Arduino's own
// HOLD_MS (or a value pushed once via "Save to the Arduino") govern it
// instead, and have the app just ask for a plain rotate.
inline constexpr bool kSendHoldWithRotate = true;

// How often the app polls GET /status for the servo's angle/state and the
// camera's health, in milliseconds.
inline constexpr int kStatusPollMs = 1000;

// How often the on-screen countdown ring repaints between status polls.
// Purely cosmetic smoothness; does not touch the network.
inline constexpr int kUiTickMs = 100;

// ---- video ------------------------------------------------------------------

// Paths on the hub, appended to whatever host is set above.
inline constexpr char kStreamPath[]   = "/stream";
inline constexpr char kSnapshotPath[] = "/snapshot.jpg";

// false = open the MJPEG stream and decode frames as they arrive (smooth,
//         but sensitive to a weak signal).
// true  = poll snapshot.jpg on a timer instead (choppier, more resilient).
// The settings drawer's "Poll still images" switch flips this at runtime;
// this is only the value the app starts with.
inline constexpr bool kUseSnapshotFallback = false;

// Only consulted in snapshot mode: how many still images to request per
// second.
inline constexpr int kSnapshotFps = 4;

// If no new video frame has arrived in this many seconds, the app declares
// the feed stalled, shows "No frames coming through", and (in streaming
// mode) reopens the connection.
inline constexpr int kVideoStallSeconds = 5;

} // namespace cfg
