#pragma once

#include <cstddef>

// ---------------------------------------------------------------------------
//  Knobs for the camera probe. The defaults sweep a small matrix in about
//  three minutes; turn off kQuickSweep for the exhaustive run.
// ---------------------------------------------------------------------------

namespace probe {

// How long to let each combination stream before judging it.
inline constexpr int kSecondsPerCandidate = 4;

// How long to wait for the camera to answer an open request.
inline constexpr int kOpenTimeoutMs = 3000;

// true  = a short matrix of the combinations most likely to work
// false = every format, resolution and rate in the tables in main.cpp
inline constexpr bool kQuickSweep = true;

// After the sweep, re-run the most interesting failure with the driver's
// debug logging turned on. This is where the real diagnosis usually comes
// from, so leave it on.
inline constexpr bool kVerboseRetry = true;

// Buffer sizing used for every candidate. Deliberately generous: the point
// of this firmware is to find out what the camera can do, not to be frugal.
//
// kFrameSize 0 means "use dwMaxVideoFrameSize from the negotiation result",
// which is the safest choice while probing. Set it explicitly only if the
// camera reports an absurd value and allocation fails.
inline constexpr size_t kFrameSize    = 0;
inline constexpr int    kFrameBuffers = 3;
inline constexpr int    kUrbs         = 4;
inline constexpr size_t kUrbSize      = 16 * 1024;

// Treat a run as a pass only if it delivered at least this many good frames.
inline constexpr int kMinFramesToPass = 5;

} // namespace probe
