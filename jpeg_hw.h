#pragma once

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
//  Hardware JPEG encoder, used only on the path where the camera sends
//  uncompressed YUY2 instead of MJPEG (config.h's kCameraSendsMjpeg=false,
//  kUseHardwareJpeg=true).
//
//  Set HUB_ENABLE_HW_JPEG to 0 below (or define it before this header is
//  first included) to drop the whole hardware-JPEG path -- for example if
//  your installed ESP-IDF version's esp_driver_jpeg API doesn't match what
//  jpeg_hw.cpp expects (see PROBE-NOTES.md, "Two places the code may not
//  compile against your versions"). The MJPEG path never calls into this
//  file, so disabling it is safe as long as your camera sends MJPEG.
//
//  Reconstructed header -- jpeg_hw.cpp survived, this declaration did not.
// ---------------------------------------------------------------------------
#ifndef HUB_ENABLE_HW_JPEG
#define HUB_ENABLE_HW_JPEG 1
#endif

namespace jpeg_hw {

// One-time setup of the hardware encoder and its output buffer (sized to
// maxOutBytes -- pass cfg::kMaxFrameBytes).
bool init(uint16_t width, uint16_t height, int quality, size_t maxOutBytes);
void deinit();

// Encodes one YUY2 frame. Returns a pointer to an internal buffer valid
// until the next call to encode(), and sets *outLen -- or returns nullptr
// on failure (check the log). Not thread-safe; call from one task at a
// time (uvc_source.cpp's dedicated encode task does this).
const uint8_t *encode(const uint8_t *yuy2, size_t yuy2Len, size_t *outLen);

bool ready();

} // namespace jpeg_hw
