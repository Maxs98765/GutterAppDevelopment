#include "uvc_source.h"

#include <atomic>

#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "jpeg_hw.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

static const char *TAG = "uvc";

namespace {

FrameStore *g_store = nullptr;

std::atomic<bool>     g_connected{false};
std::atomic<uint32_t> g_frames{0};
std::atomic<uint32_t> g_errors{0};

uvc_host_stream_hdl_t g_stream = nullptr;

// Only used on the uncompressed path, where the frame has to leave the
// driver callback to be encoded.
QueueHandle_t g_encodeQueue = nullptr;

// --- callbacks -------------------------------------------------------------

// Runs in the driver task.
//
// On the MJPEG path the frame is already a JPEG, so it is copied into the
// FrameStore here and handed straight back (returning true). On the
// uncompressed path it goes to a worker instead, because hardware encoding
// is far too much work to do inside a driver callback.
bool frameCb(const uvc_host_frame_t *frame, void *)
{
    if (!frame || frame->data_len == 0 || !g_store) return true;

    if (cfg::kCameraSendsMjpeg) {
        if (g_store->put(frame->data, frame->data_len)) {
            uint32_t n = ++g_frames;
            if (n == 1) {
                ESP_LOGI(TAG, "first frame: %u bytes",
                         static_cast<unsigned>(frame->data_len));
            }
        }
        return true;   // done with the buffer
    }

    if (xQueueSendToBack(g_encodeQueue, &frame, 0) != pdPASS) {
        return true;   // encoder is behind; drop rather than stall USB
    }
    return false;      // the worker will call uvc_host_frame_return()
}

void streamCb(const uvc_host_stream_event_data_t *event, void *)
{
    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        g_errors++;
        ESP_LOGD(TAG, "transfer error %i", event->transfer_error.error);
        break;

    case UVC_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "camera disconnected");
        g_connected = false;
        uvc_host_stream_close(event->device_disconnected.stream_hdl);
        g_stream = nullptr;
        break;

    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        // A frame arrived bigger than the allocated buffer. Raise
        // cfg::kUvcFrameSize, or trust the negotiated size by setting it to 0.
        ESP_LOGW(TAG, "frame buffer overflow");
        break;

    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        // No free buffer was available. Raise cfg::kUvcFrameBufs, or find out
        // what is holding frames too long.
        ESP_LOGW(TAG, "frame buffer underflow");
        break;

    default:
        break;
    }
}

// --- encode worker (uncompressed path only) -------------------------------

void encodeTask(void *)
{
    while (true) {
        const uvc_host_frame_t *frame = nullptr;
        if (xQueueReceive(g_encodeQueue, &frame, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (!frame) continue;

        size_t jpegLen = 0;
        const uint8_t *jpeg = jpeg_hw::encode(frame->data, frame->data_len,
                                              &jpegLen);
        if (jpeg && g_store->put(jpeg, jpegLen)) {
            uint32_t n = ++g_frames;
            if (n == 1) {
                ESP_LOGI(TAG, "first encoded frame: %u bytes in, %u out",
                         static_cast<unsigned>(frame->data_len),
                         static_cast<unsigned>(jpegLen));
            }
        }

        if (g_stream) uvc_host_frame_return(g_stream, const_cast<uvc_host_frame_t *>(frame));
    }
}

// --- stream lifecycle ------------------------------------------------------

uvc_host_stream_config_t makeConfig()
{
    uvc_host_stream_config_t cfg = {};
    cfg.event_cb = streamCb;
    cfg.frame_cb = frameCb;
    cfg.user_ctx = nullptr;

    cfg.usb.vid              = UVC_HOST_ANY_VID;
    cfg.usb.pid              = UVC_HOST_ANY_PID;
    cfg.usb.uvc_stream_index = 0;

    cfg.vs_format.h_res  = cfg::kFrameWidth;
    cfg.vs_format.v_res  = cfg::kFrameHeight;
    cfg.vs_format.fps    = cfg::kFrameRate;
    cfg.vs_format.format = cfg::kCameraSendsMjpeg ? UVC_VS_FORMAT_MJPEG
                                                  : UVC_VS_FORMAT_YUY2;

    cfg.advanced.frame_size              = cfg::kUvcFrameSize;
    cfg.advanced.number_of_frame_buffers = cfg::kUvcFrameBufs;
    cfg.advanced.number_of_urbs          = cfg::kUvcUrbs;
    cfg.advanced.urb_size                = cfg::kUvcUrbSize;
    cfg.advanced.frame_heap_caps         = MALLOC_CAP_SPIRAM;

    return cfg;
}

// Keeps the stream alive, reopening after a hot unplug.
void streamTask(void *)
{
    const uvc_host_stream_config_t cfg = makeConfig();

    while (true) {
        if (g_stream == nullptr) {
            ESP_LOGI(TAG, "opening the camera at %ux%u",
                     cfg::kFrameWidth, cfg::kFrameHeight);

            esp_err_t err = uvc_host_stream_open(&cfg, pdMS_TO_TICKS(3000),
                                                 &g_stream);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "open failed (%s); is the camera plugged in and "
                              "does it support this format? Run camera_probe.",
                         esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }

            // The format is committed on start, so failures can surface here.
            err = uvc_host_stream_start(g_stream);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
                uvc_host_stream_close(g_stream);
                g_stream = nullptr;
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }

            g_connected = true;
            ESP_LOGI(TAG, "streaming");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void usbLibTask(void *)
{
    while (true) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

} // namespace

namespace uvc {

bool start(FrameStore *store)
{
    g_store = store;

    const usb_host_config_t hostCfg = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LOWMED,
    };
    esp_err_t err = usb_host_install(&hostCfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(err));
        return false;
    }

    xTaskCreatePinnedToCore(usbLibTask, "usb_lib", 4096, nullptr, 15, nullptr,
                            tskNO_AFFINITY);

    const uvc_host_driver_config_t drvCfg = {
        .driver_task_stack_size = 6 * 1024,
        .driver_task_priority   = 16,
        .xCoreID                = tskNO_AFFINITY,
        .create_background_task = true,
    };
    err = uvc_host_install(&drvCfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uvc_host_install: %s", esp_err_to_name(err));
        return false;
    }

    if (!cfg::kCameraSendsMjpeg) {
        if (!cfg::kUseHardwareJpeg) {
            ESP_LOGE(TAG, "an uncompressed camera needs the hardware encoder; "
                          "set kUseHardwareJpeg true");
            return false;
        }
        if (!jpeg_hw::init(cfg::kFrameWidth, cfg::kFrameHeight,
                           cfg::kJpegQuality, cfg::kMaxFrameBytes)) {
            return false;
        }

        g_encodeQueue = xQueueCreate(cfg::kUvcFrameBufs,
                                     sizeof(uvc_host_frame_t *));
        if (!g_encodeQueue) return false;

        xTaskCreatePinnedToCore(encodeTask, "jpeg_enc", 4096, nullptr, 12,
                                nullptr, tskNO_AFFINITY);
        ESP_LOGI(TAG, "uncompressed camera: encoding in hardware before "
                      "streaming");
    }

    xTaskCreatePinnedToCore(streamTask, "uvc_stream", 4096, nullptr, 13,
                            nullptr, tskNO_AFFINITY);
    return true;
}

void stop()
{
    if (g_stream) {
        uvc_host_stream_stop(g_stream);
        uvc_host_stream_close(g_stream);
        g_stream = nullptr;
    }
    jpeg_hw::deinit();
    g_connected = false;
}

bool     connected()  { return g_connected.load(); }
uint32_t framesSeen() { return g_frames.load(); }
uint32_t errorCount() { return g_errors.load(); }

} // namespace uvc
