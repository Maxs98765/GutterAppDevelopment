#pragma once

#include <cstdint>

#include "frame_store.h"

// ---------------------------------------------------------------------------
//  USB host + UVC camera driver. Delivers JPEG frames into a FrameStore --
//  either the camera's own MJPEG output, passed straight through, or, if
//  the camera only sends uncompressed YUY2, frames encoded on the fly via
//  jpeg_hw before being stored. See config.h's kCameraSendsMjpeg /
//  kUseHardwareJpeg.
//
//  Reconstructed header -- uvc_source.cpp survived, this declaration did
//  not.
// ---------------------------------------------------------------------------

namespace uvc {

// Installs the USB host + UVC driver stack and starts the streaming task.
// `store` must outlive the stream; frames are pushed into it as they
// arrive. Returns false if any driver install step failed -- check the
// log for which one.
bool start(FrameStore *store);

void stop();

bool     connected();   // true once the camera has been opened and started
uint32_t framesSeen();  // running count of frames delivered into the store
uint32_t errorCount();  // running count of USB transfer errors

} // namespace uvc
