#pragma once

#include "frame_store.h"

// ---------------------------------------------------------------------------
//  The hub's HTTP server: MJPEG video, a JPEG snapshot, and the servo
//  control routes (/rotate, /home, /hold, /status), plus a plain built-in
//  browser page at "/". See README.md's "HTTP API" table for the full
//  route list.
//
//  Reconstructed header -- http_api.cpp survived, this declaration did
//  not.
// ---------------------------------------------------------------------------

namespace api {

// Starts the HTTP server on cfg::kHttpPort. `store` may be nullptr (e.g.
// if the camera is disabled in config.h, or failed to start); the video
// routes then answer with a 503 instead of streaming, and everything else
// still works.
bool start(FrameStore *store);

void stop();

} // namespace api
