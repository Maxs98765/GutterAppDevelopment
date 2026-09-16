#include "uvc_source.h"

#include <atomic>

#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "usb_stream.h"

static const char *TAG = "uvc";

namespace {

FrameStore *g_store = nullptr;

uint8_t *g_xferA  = nullptr;
uint8_t *g_xferB  = nullptr;
uint8_t *g_frame  = nullptr;

std::atomic<bool>     g_connected{false};
std::atomic<uint32_t> g_frames{0};

// Runs in the USB stream task. Keep it short: copy and get out.
void onFrame(uvc_frame_t *frame, void *)
{
    if (!frame || !g_store) return;

    if (frame->frame_format != UVC_FRAME_FORMAT_MJPEG) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            ESP_LOGW(TAG, "camera is sending an uncompressed format; this "
                          "build only re-streams MJPEG");
        }
        return;
    }

    if (g_store->put(static_cast<const uint8_t *>(frame->data),
                     frame->data_bytes)) {
        uint32_t n = ++g_frames;
        if (n == 1) {
            ESP_LOGI(TAG, "first frame: %ux%u, %u bytes",
                     frame->width, frame->height,
                     static_cast<unsigned>(frame->data_bytes));
        }
    }
}

void onStateChange(usb_stream_state_t state, void *)
{
    switch (state) {
    case STREAM_CONNECTED:
        g_connected = true;
        ESP_LOGI(TAG, "camera connected");
        break;
    case STREAM_DISCONNECTED:
        g_connected = false;
        ESP_LOGW(TAG, "camera disconnected");
        break;
    default:
        break;
    }
}

uint8_t *psram(size_t bytes, const char *what)
{
    auto *p = static_cast<uint8_t *>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!p) {
        ESP_LOGE(TAG, "no PSRAM for %s (%u bytes)", what,
                 static_cast<unsigned>(bytes));
    }
    return p;
}

void freeAll()
{
    for (uint8_t **p : {&g_xferA, &g_xferB, &g_frame}) {
        if (*p) {
            heap_caps_free(*p);
            *p = nullptr;
        }
    }
}

} // namespace

namespace uvc {

bool start(FrameStore *store)
{
    g_store = store;

    g_xferA = psram(cfg::kXferBufferBytes, "USB transfer buffer A");
    g_xferB = psram(cfg::kXferBufferBytes, "USB transfer buffer B");
    g_frame = psram(cfg::kMaxFrameBytes,   "UVC frame buffer");
    if (!g_xferA || !g_xferB || !g_frame) {
        freeAll();
        return false;
    }

    uvc_config_t uc = {};
    uc.frame_width       = cfg::kResolutionAny ? FRAME_RESOLUTION_ANY
                                               : cfg::kFrameWidth;
    uc.frame_height      = cfg::kResolutionAny ? FRAME_RESOLUTION_ANY
                                               : cfg::kFrameHeight;
    uc.frame_interval    = FPS2INTERVAL(cfg::kFrameRate);
    uc.xfer_buffer_size  = cfg::kXferBufferBytes;
    uc.xfer_buffer_a     = g_xferA;
    uc.xfer_buffer_b     = g_xferB;
    uc.frame_buffer_size = cfg::kMaxFrameBytes;
    uc.frame_buffer      = g_frame;
    uc.frame_cb          = &onFrame;
    uc.frame_cb_arg      = nullptr;

    esp_err_t err = uvc_streaming_config(&uc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uvc_streaming_config failed: %s", esp_err_to_name(err));
        freeAll();
        return false;
    }

    err = usb_streaming_state_register(&onStateChange, nullptr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "state callback not registered: %s", esp_err_to_name(err));
    }

    err = usb_streaming_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_streaming_start failed: %s", esp_err_to_name(err));
        freeAll();
        return false;
    }

    ESP_LOGI(TAG, "waiting for a camera on the USB port");

    // Not fatal if this times out -- hot-plugging later still works.
    if (usb_streaming_connect_wait(pdMS_TO_TICKS(5000)) != ESP_OK) {
        ESP_LOGW(TAG, "no camera yet; plug one in and it will pick up");
    }
    return true;
}

void stop()
{
    usb_streaming_stop();
    freeAll();
    g_connected = false;
}

bool     connected()  { return g_connected.load(); }
uint32_t framesSeen() { return g_frames.load(); }

} // namespace uvc
