// ---------------------------------------------------------------------------
//  mipi_cam.cpp -- MIPI-CSI capture for the Arducam IMX708 module
//
//  Replaces uvc_source.cpp now that the BOM camera (ListofMaterials.xlsx:
//  "Arducam ... Camera Module 3 ... IMX708 ... Autofocus ... 15-22pin")
//  is a CSI ribbon module, not a USB UVC webcam. There is no USB host
//  driver here at all -- the camera plugs into the board's MIPI-CSI
//  connector and is driven by Espressif's esp_video component, which
//  exposes it as /dev/video0 with the same open()/ioctl() calls Linux
//  V4L2 programs use (VIDIOC_S_FMT, VIDIOC_REQBUFS, VIDIOC_QBUF,
//  VIDIOC_DQBUF, VIDIOC_STREAMON/OFF), on top of an ISP pipeline that can
//  optionally hand back frames already encoded as JPEG.
//
//  VERIFY BEFORE YOU BUILD -- this is the single riskiest file in this
//  rewrite, for the same reason the original header reconstructions
//  needed a "verify" flag: it was written from the esp_video/esp_cam_sensor
//  component documentation and the standard Linux V4L2 API shape, not
//  from a successful build against your exact installed component
//  versions. Specifically check, once `idf.py add-dependency` has pulled
//  esp_video/esp_cam_sensor/esp_cam_sensor_imx into managed_components/:
//   - esp_video_init_config_t / esp_video_init_csi_config_t field names
//     in that version's esp_video_init.h (struct layouts have changed
//     across esp_video releases)
//   - that <linux/videodev2.h> is the header esp_video actually installs
//     (some versions may put V4L2 definitions under a different path)
//   - that VIDIOC_S_FMT with V4L2_PIX_FMT_JPEG is actually accepted for
//     the CSI capture path in your version -- if not, this file's
//     fallback to RGB565 + jpeg_hw's hardware encoder takes over
//     automatically and logs that it did so
//   - CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE and the ISP Kconfig
//     options are actually enabled (see sdkconfig.defaults)
// ---------------------------------------------------------------------------

#include "mipi_cam.h"

#include <atomic>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpeg_hw.h"

#include "esp_video_init.h"
#include "linux/videodev2.h"

static const char *TAG = "cam";

namespace {

FrameStore *g_store = nullptr;

std::atomic<bool>     g_connected{false};
std::atomic<uint32_t> g_frames{0};
std::atomic<uint32_t> g_errors{0};

int  g_fd = -1;
bool g_jpegFromIsp = false;   // which of the two capture paths is live

struct MappedBuf {
    void  *start  = nullptr;
    size_t length = 0;
};

constexpr int kMaxBufs = 8;
MappedBuf g_bufs[kMaxBufs];
int       g_bufCount = 0;

// --- device bring-up ---------------------------------------------------

bool initVideoSubsystem()
{
    // Field names/types below are taken from this project's actual installed
    // esp_video_init.h (managed_components/espressif__esp_video), not the
    // earlier guess -- see the header comment above. The SCCB (camera I2C)
    // config is a union of {port, scl_pin, sda_pin} OR a pre-made I2C
    // handle; we use the first form. scl_pin/sda_pin/reset_pin/pwdn_pin are
    // all gpio_num_t, not plain int, so config.h's int constants need an
    // explicit cast (a -1 "not present" sentinel casts to GPIO_NUM_NC).
    esp_video_init_csi_config_t csiCfg = {};
    csiCfg.sccb_config.init_sccb          = true;
    csiCfg.sccb_config.i2c_config.port    = cfg::kCamSccbI2cPort;
    csiCfg.sccb_config.i2c_config.scl_pin = static_cast<gpio_num_t>(cfg::kCamSccbSclPin);
    csiCfg.sccb_config.i2c_config.sda_pin = static_cast<gpio_num_t>(cfg::kCamSccbSdaPin);
    csiCfg.sccb_config.freq                = 100000;   // SCCB/I2C bus speed, 100kHz
    csiCfg.reset_pin = static_cast<gpio_num_t>(cfg::kCamResetPin);
    csiCfg.pwdn_pin  = static_cast<gpio_num_t>(cfg::kCamPwdnPin);

    esp_video_init_config_t videoCfg = {};
    videoCfg.csi = &csiCfg;

    esp_err_t err = esp_video_init(&videoCfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed: %s -- check the SCCB pins in "
                      "config.h, and that the CSI ribbon is seated the "
                      "right way round (a reversed ribbon is the single "
                      "most common reason a sensor is never detected)",
                 esp_err_to_name(err));
        return false;
    }
    return true;
}

// Tries to set the capture format. Returns false (without logging an
// error -- this is an expected, handled outcome for the JPEG attempt) if
// the running esp_video build cannot do this, so the caller can fall back.
bool tryFormat(int fd, uint32_t fourcc, uint16_t w, uint16_t h)
{
    v4l2_format fmt = {};
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = w;
    fmt.fmt.pix.height      = h;
    fmt.fmt.pix.pixelformat = fourcc;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    return ioctl(fd, VIDIOC_S_FMT, &fmt) == 0;
}

bool requestAndMapBuffers(int fd)
{
    v4l2_requestbuffers req = {};
    req.count  = cfg::kCamBufferCount;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed: %s", strerror(errno));
        return false;
    }

    g_bufCount = static_cast<int>(req.count) < kMaxBufs
        ? static_cast<int>(req.count) : kMaxBufs;

    for (int i = 0; i < g_bufCount; i++) {
        v4l2_buffer buf = {};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF[%d] failed: %s", i, strerror(errno));
            return false;
        }

        g_bufs[i].length = buf.length;
        g_bufs[i].start  = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, buf.m.offset);
        if (g_bufs[i].start == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap[%d] failed: %s", i, strerror(errno));
            g_bufs[i].start = nullptr;
            return false;
        }

        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF[%d] failed: %s", i, strerror(errno));
            return false;
        }
    }
    return true;
}

// --- capture loop --------------------------------------------------------

void captureTask(void *)
{
    while (true) {
        v4l2_buffer buf = {};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(g_fd, VIDIOC_DQBUF, &buf) != 0) {
            g_errors++;
            ESP_LOGW(TAG, "VIDIOC_DQBUF failed: %s", strerror(errno));
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        const uint8_t *data = static_cast<const uint8_t *>(g_bufs[buf.index].start);
        const size_t   len  = buf.bytesused;

        bool stored = false;
        if (g_jpegFromIsp) {
            stored = g_store->put(data, len);
        } else {
            size_t jpegLen = 0;
            const uint8_t *jpeg = jpeg_hw::encode(data, len, &jpegLen);
            stored = jpeg && g_store->put(jpeg, jpegLen);
        }

        if (stored) {
            uint32_t n = ++g_frames;
            if (n == 1) {
                ESP_LOGI(TAG, "first frame: %u bytes",
                         static_cast<unsigned>(len));
            }
            g_connected = true;
        }

        // Hand the buffer back to the driver for the next capture.
        if (ioctl(g_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGW(TAG, "VIDIOC_QBUF (requeue) failed: %s", strerror(errno));
        }
    }
}

} // namespace

namespace cam {

bool start(FrameStore *store)
{
    g_store = store;

    if (!initVideoSubsystem()) return false;

    g_fd = open("/dev/video0", O_RDWR);
    if (g_fd < 0) {
        ESP_LOGE(TAG, "open(/dev/video0) failed: %s -- esp_video_init "
                      "reported success but no device node appeared; check "
                      "CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE is set "
                      "in sdkconfig.defaults", strerror(errno));
        return false;
    }

    // Prefer letting the ISP+hardware-JPEG pipeline hand back ready-made
    // JPEG frames -- far less CPU/memory than decoding RAW ourselves. Fall
    // back to RGB565 + the P4's hardware JPEG encoder (jpeg_hw.cpp, the
    // same component the old USB-camera build used for its uncompressed
    // path) if this esp_video build can't set that output format for CSI.
    if (cfg::kRequestJpegFromIsp &&
        tryFormat(g_fd, V4L2_PIX_FMT_JPEG, cfg::kFrameWidth, cfg::kFrameHeight)) {
        g_jpegFromIsp = true;
        ESP_LOGI(TAG, "capturing JPEG directly from the ISP pipeline");
    } else {
        if (!tryFormat(g_fd, V4L2_PIX_FMT_RGB565, cfg::kFrameWidth, cfg::kFrameHeight)) {
            // kFrameWidth/kFrameHeight are uint16_t; varargs promote that to
            // plain int, which -Werror=format= treats as a mismatch against
            // a bare %u (same fix as net.cpp/jpeg_hw.cpp above).
            ESP_LOGE(TAG, "could not set RGB565 either -- VIDIOC_S_FMT "
                          "rejected both formats at %ux%u; try a smaller "
                          "resolution in config.h",
                     static_cast<unsigned>(cfg::kFrameWidth),
                     static_cast<unsigned>(cfg::kFrameHeight));
            close(g_fd);
            g_fd = -1;
            return false;
        }
        if (!jpeg_hw::init(cfg::kFrameWidth, cfg::kFrameHeight,
                           cfg::kJpegQuality, cfg::kMaxFrameBytes)) {
            ESP_LOGE(TAG, "hardware JPEG encoder init failed");
            close(g_fd);
            g_fd = -1;
            return false;
        }
        g_jpegFromIsp = false;
        ESP_LOGI(TAG, "ISP could not hand back JPEG directly; capturing "
                      "RGB565 and encoding with the hardware JPEG engine");
    }

    if (!requestAndMapBuffers(g_fd)) {
        close(g_fd);
        g_fd = -1;
        return false;
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(g_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed: %s", strerror(errno));
        close(g_fd);
        g_fd = -1;
        return false;
    }

    xTaskCreatePinnedToCore(captureTask, "cam_capture", 6144, nullptr, 13,
                            nullptr, tskNO_AFFINITY);
    ESP_LOGI(TAG, "capturing %ux%u @ ~%.0f fps",
             static_cast<unsigned>(cfg::kFrameWidth),
             static_cast<unsigned>(cfg::kFrameHeight), cfg::kFrameRate);
    return true;
}

void stop()
{
    if (g_fd >= 0) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(g_fd, VIDIOC_STREAMOFF, &type);
        for (int i = 0; i < g_bufCount; i++) {
            if (g_bufs[i].start) munmap(g_bufs[i].start, g_bufs[i].length);
            g_bufs[i].start = nullptr;
        }
        close(g_fd);
        g_fd = -1;
    }
    jpeg_hw::deinit();
    g_connected = false;
}

bool     connected()  { return g_connected.load(); }
uint32_t framesSeen() { return g_frames.load(); }
uint32_t errorCount() { return g_errors.load(); }

} // namespace cam
