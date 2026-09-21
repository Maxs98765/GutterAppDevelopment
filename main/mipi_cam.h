#pragma once

#include <cstdint>

#include "frame_store.h"

// ---------------------------------------------------------------------------
//  MIPI-CSI camera capture, replacing uvc_source.h/.cpp now that the BOM
//  camera is an Arducam IMX708 module on the board's CSI connector instead
//  of a USB UVC webcam.
//
//  Uses Espressif's esp_video component, which exposes the camera as a
//  /dev/videoN device driven with the same ioctl calls as Linux's V4L2 API
//  (open, VIDIOC_S_FMT, VIDIOC_REQBUFS/QBUF/DQBUF, VIDIOC_STREAMON). The
//  IMX708 sensor itself is detected and driven by a third-party driver
//  component (mushbraindave/esp_cam_sensor_imx -- see idf_component.yml)
//  that registers with esp_cam_sensor/esp_video underneath; this file
//  never talks to the sensor directly.
// ---------------------------------------------------------------------------

namespace cam {

// Brings up the SCCB (I2C) bus, the MIPI-CSI receiver, the IMX708 sensor,
// and the ISP pipeline, then starts a background task pushing captured
// JPEG frames into `store`. Returns false if any stage fails (check the
// log -- it names which one).
bool start(FrameStore *store);

void stop();

bool     connected();   // true once the sensor has been detected and the
                         // capture loop is running
uint32_t framesSeen();
uint32_t errorCount();

} // namespace cam
