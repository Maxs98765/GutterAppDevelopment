// ---------------------------------------------------------------------------
//  camera_probe  --  what can this USB camera actually do?
//
//  Opens the attached UVC camera once per candidate format, streams for a few
//  seconds, and records what came back: frame count, real frame rate, frame
//  sizes, JPEG integrity, and every transfer error or buffer event the driver
//  reported. Prints a table at the end, plus a config block you can paste
//  straight into the hub firmware.
//
//  Built against espressif/usb_host_uvc v2.5.x, which is native to the
//  ESP-IDF USB Host Library (the libuvc-based 1.x API is gone).
//
//  Run it, let it finish, and keep the serial log. The table is the answer to
//  "do we need a new camera", and the verbose retry at the end is usually the
//  answer to "why not".
// ---------------------------------------------------------------------------

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "probe_config.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

static const char *TAG = "probe";

// The exact type names in the driver have moved around between versions, so
// borrow them from the config struct instead of spelling them out. This keeps
// the file compiling across 2.x releases.
using VsFormat   = decltype(uvc_host_stream_config_t::vs_format);
using FormatKind = decltype(VsFormat::format);

namespace {

// --- what to try ----------------------------------------------------------
//
// If one of these format names does not compile, your driver version does not
// have it: open `managed_components/espressif__usb_host_uvc/include/usb/
// uvc_host.h`, find the format enum, and delete the line.

struct FormatEntry {
    FormatKind  kind;
    const char *name;
};

const FormatEntry kFormats[] = {
    { UVC_VS_FORMAT_MJPEG,  "MJPEG" },
    { UVC_VS_FORMAT_YUY2,   "YUY2"  },   // uncompressed; the quirk-camera escape hatch
    { UVC_VS_FORMAT_H264,   "H264"  },
};

struct Resolution { uint16_t w, h; };

const Resolution kQuickRes[] = {
    { 160, 120 }, { 320, 240 }, { 640, 480 },
};

const Resolution kFullRes[] = {
    { 160, 120 }, { 320, 240 }, { 352, 288 }, { 640, 360 },
    { 640, 480 }, { 800, 600 }, { 1280, 720 }, { 1920, 1080 },
};

// 0 asks the driver for the camera's own default rate, which is often the
// only rate a badly-behaved camera will actually honour.
const float kQuickFps[] = { 0.0f, 5.0f, 15.0f };
const float kFullFps[]  = { 0.0f, 5.0f, 10.0f, 15.0f, 30.0f };

// --- one candidate's outcome ----------------------------------------------

struct Result {
    FormatKind  kind;
    const char *formatName;
    uint16_t    w, h;
    float       askedFps;

    esp_err_t openErr = ESP_OK;
    bool      opened  = false;

    int      frames    = 0;
    int      badJpeg   = 0;
    size_t   minLen    = SIZE_MAX;
    size_t   maxLen    = 0;
    uint64_t totalLen  = 0;
    float    realFps   = 0.0f;

    int xferErrors = 0;
    int overflows  = 0;
    int underflows = 0;

    bool passed() const
    {
        return opened
            && frames >= probe::kMinFramesToPass
            && badJpeg * 4 < frames;   // tolerate the odd corrupt frame
    }

    // Short human verdict, which is the column people will actually read.
    const char *verdict() const
    {
        if (!opened)                return "will not open";
        if (frames == 0)            return "opens, no frames";
        if (badJpeg * 4 >= frames)  return "frames are corrupt";
        if (frames < probe::kMinFramesToPass) return "a few frames, then dies";
        if (overflows > 0)          return "works, buffer too small";
        if (underflows > 0)         return "works, host too slow";
        if (xferErrors > 0)         return "works, with USB errors";
        return "works";
    }
};

// The callbacks run in the driver task, so counters are shared state.
struct Live {
    std::atomic<int>      frames{0};
    std::atomic<int>      badJpeg{0};
    std::atomic<size_t>   minLen{SIZE_MAX};
    std::atomic<size_t>   maxLen{0};
    std::atomic<uint64_t> totalLen{0};
    std::atomic<int>      xferErrors{0};
    std::atomic<int>      overflows{0};
    std::atomic<int>      underflows{0};
    std::atomic<bool>     disconnected{false};
    std::atomic<bool>     checkJpeg{false};
};

Live g_live;

// --- callbacks ------------------------------------------------------------

// Returning true hands the buffer straight back to the driver. We only look
// at a few bytes, so there is no reason to queue anything.
bool frameCb(const uvc_host_frame_t *frame, void *)
{
    if (!frame || frame->data_len == 0) return true;

    const size_t len = frame->data_len;

    g_live.frames++;
    g_live.totalLen += len;

    size_t cur = g_live.minLen.load();
    while (len < cur && !g_live.minLen.compare_exchange_weak(cur, len)) { }
    cur = g_live.maxLen.load();
    while (len > cur && !g_live.maxLen.compare_exchange_weak(cur, len)) { }

    // A JPEG must start FF D8 and end FF D9. Cameras with a half-finished
    // MJPEG implementation deliver frames that fail this, which looks like
    // "streaming works" to the driver but produces garbage downstream. This
    // check is the whole reason the probe exists.
    if (g_live.checkJpeg.load() && len >= 4) {
        const uint8_t *d = frame->data;
        const bool soi = d[0] == 0xFF && d[1] == 0xD8;
        const bool eoi = d[len - 2] == 0xFF && d[len - 1] == 0xD9;
        if (!soi || !eoi) g_live.badJpeg++;
    }

    return true;
}

void streamCb(const uvc_host_stream_event_data_t *event, void *)
{
    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        g_live.xferErrors++;
        ESP_LOGD(TAG, "transfer error %i", event->transfer_error.error);
        break;

    case UVC_HOST_DEVICE_DISCONNECTED:
        g_live.disconnected = true;
        ESP_LOGW(TAG, "camera disconnected mid-probe");
        uvc_host_stream_close(event->device_disconnected.stream_hdl);
        break;

    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        g_live.overflows++;
        break;

    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        g_live.underflows++;
        break;

    default:
        break;
    }
}

// --- one candidate --------------------------------------------------------

void resetLive(bool checkJpeg)
{
    g_live.frames     = 0;
    g_live.badJpeg    = 0;
    g_live.minLen     = SIZE_MAX;
    g_live.maxLen     = 0;
    g_live.totalLen   = 0;
    g_live.xferErrors = 0;
    g_live.overflows  = 0;
    g_live.underflows = 0;
    g_live.checkJpeg  = checkJpeg;
    // deliberately not clearing `disconnected`: once the camera is gone,
    // the whole run is over.
}

Result tryCandidate(const FormatEntry &fmt, Resolution res, float fps)
{
    Result r;
    r.kind       = fmt.kind;
    r.formatName = fmt.name;
    r.w          = res.w;
    r.h          = res.h;
    r.askedFps   = fps;

    resetLive(fmt.kind == UVC_VS_FORMAT_MJPEG);

    uvc_host_stream_config_t cfg = {};
    cfg.event_cb = streamCb;
    cfg.frame_cb = frameCb;
    cfg.user_ctx = nullptr;

    cfg.usb.vid              = UVC_HOST_ANY_VID;
    cfg.usb.pid              = UVC_HOST_ANY_PID;
    cfg.usb.uvc_stream_index = 0;

    cfg.vs_format.h_res  = res.w;
    cfg.vs_format.v_res  = res.h;
    cfg.vs_format.fps    = fps;
    cfg.vs_format.format = fmt.kind;

    cfg.advanced.frame_size               = probe::kFrameSize;
    cfg.advanced.number_of_frame_buffers  = probe::kFrameBuffers;
    cfg.advanced.number_of_urbs           = probe::kUrbs;
    cfg.advanced.urb_size                 = probe::kUrbSize;
    cfg.advanced.frame_heap_caps          = MALLOC_CAP_SPIRAM;

    uvc_host_stream_hdl_t stream = nullptr;
    r.openErr = uvc_host_stream_open(&cfg, pdMS_TO_TICKS(probe::kOpenTimeoutMs),
                                     &stream);

    if (r.openErr != ESP_OK || stream == nullptr) {
        return r;   // negotiation refused this combination
    }
    r.opened = true;

    // The driver commits the format when the stream starts, so errors can
    // still surface after a successful open.
    const int64_t t0 = esp_timer_get_time();
    esp_err_t startErr = uvc_host_stream_start(stream);

    if (startErr == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(probe::kSecondsPerCandidate * 1000));
        uvc_host_stream_stop(stream);
    } else {
        ESP_LOGD(TAG, "stream_start: %s", esp_err_to_name(startErr));
    }

    const float seconds = (esp_timer_get_time() - t0) / 1000000.0f;

    uvc_host_stream_close(stream);

    r.frames     = g_live.frames.load();
    r.badJpeg    = g_live.badJpeg.load();
    r.minLen     = g_live.minLen.load();
    r.maxLen     = g_live.maxLen.load();
    r.totalLen   = g_live.totalLen.load();
    r.xferErrors = g_live.xferErrors.load();
    r.overflows  = g_live.overflows.load();
    r.underflows = g_live.underflows.load();
    r.realFps    = seconds > 0.1f ? r.frames / seconds : 0.0f;

    if (r.minLen == SIZE_MAX) r.minLen = 0;

    // Let the bus settle before the next attempt. Cameras that are already
    // unhappy get much worse without this.
    vTaskDelay(pdMS_TO_TICKS(600));
    return r;
}

// --- reporting ------------------------------------------------------------

void printRow(const Result &r)
{
    char asked[16];
    if (r.askedFps <= 0.0f) {
        snprintf(asked, sizeof(asked), "default");
    } else {
        snprintf(asked, sizeof(asked), "%.0f", r.askedFps);
    }

    printf("%-6s %4ux%-4u %-8s %6d %6.1f %7u %7u %3d %3d %3d %3d  %s\n",
           r.formatName, r.w, r.h, asked,
           r.frames, r.realFps,
           static_cast<unsigned>(r.minLen), static_cast<unsigned>(r.maxLen),
           r.badJpeg, r.xferErrors, r.overflows, r.underflows,
           r.verdict());
}

void printHeader()
{
    printf("\n");
    printf("format   resolution  asked  frames    fps  minlen  maxlen bad err ovf unf  verdict\n");
    printf("------------------------------------------------------------------------------------------\n");
}

void printSuggestion(const Result &r)
{
    printf("\n");
    printf("Paste this into the hub firmware:\n\n");
    printf("    cfg.vs_format.h_res  = %u;\n", r.w);
    printf("    cfg.vs_format.v_res  = %u;\n", r.h);
    if (r.askedFps <= 0.0f) {
        printf("    cfg.vs_format.fps    = 0;      // camera default, measured %.1f\n",
               r.realFps);
    } else {
        printf("    cfg.vs_format.fps    = %.0f;     // measured %.1f\n",
               r.askedFps, r.realFps);
    }
    printf("    cfg.vs_format.format = UVC_VS_FORMAT_%s;\n", r.formatName);
    printf("\n");
    printf("    cfg.advanced.frame_size              = %u;  // measured max was %u\n",
           static_cast<unsigned>(r.maxLen + r.maxLen / 4),
           static_cast<unsigned>(r.maxLen));
    printf("    cfg.advanced.number_of_frame_buffers = %d;\n", probe::kFrameBuffers);
    printf("    cfg.advanced.number_of_urbs          = %d;\n", probe::kUrbs);
    printf("    cfg.advanced.urb_size                = %u;\n",
           static_cast<unsigned>(probe::kUrbSize));
    printf("    cfg.advanced.frame_heap_caps         = MALLOC_CAP_SPIRAM;\n");

    if (r.kind != UVC_VS_FORMAT_MJPEG) {
        printf("\n");
        printf("Note: this is an uncompressed format, so the hub has to encode it\n");
        printf("before streaming over WiFi. Use the P4's hardware JPEG encoder;\n");
        printf("do not try to compress %ux%u in software.\n", r.w, r.h);
    }
}

void printAdvice(const std::vector<Result> &all, bool anyPass)
{
    int opened = 0, corrupt = 0, refused = 0;
    for (const auto &r : all) {
        if (!r.opened) refused++;
        else if (r.frames == 0) opened++;
        else if (r.badJpeg * 4 >= r.frames) corrupt++;
    }

    printf("\n");
    printf("What this means\n");
    printf("---------------\n");

    if (anyPass) {
        printf("The camera works. Use the suggested block above and move on.\n");
        return;
    }

    if (opened > 0 && corrupt == 0) {
        printf("Every combination negotiated and then delivered nothing. That is a\n");
        printf("transfer-layer problem, not a format problem. In order of likelihood:\n");
        printf("  1. Driver version. usb_host_uvc 2.3.0 fixed an abort on an\n");
        printf("     unexpected EoF flag in bulk transfers, which looks exactly\n");
        printf("     like this. Check what you resolved to and force the newest.\n");
        printf("  2. Power. Current spikes when streaming starts, not at open.\n");
        printf("     Feed VBUS from a powered Y-cable, not a hub.\n");
        printf("  3. Wrong controller. If the camera is Full-Speed, run the probe\n");
        printf("     on the P4's Full-Speed peripheral instead.\n");
    } else if (corrupt > 0) {
        printf("Frames arrive but fail the JPEG start/end marker check. The camera\n");
        printf("is producing malformed MJPEG, or the driver is misreading its frame\n");
        printf("boundaries. Try the YUY2 rows above; an uncompressed path often\n");
        printf("works on a camera whose MJPEG does not. Failing that, this is a\n");
        printf("camera to replace.\n");
    } else if (refused == static_cast<int>(all.size())) {
        printf("Nothing negotiated at all. Read the configuration descriptor dump\n");
        printf("at the top of this log and add the exact resolutions and rates it\n");
        printf("lists to the tables in main.cpp. The camera may only offer sizes\n");
        printf("that are not in the default sweep.\n");
    }

    printf("\nEither way: a MIPI-CSI module is the P4's native camera path and\n");
    printf("costs about the same as a replacement webcam.\n");
}

// --- verbose retry --------------------------------------------------------

void verboseRetry(const std::vector<Result> &all)
{
    // The most informative failure is one that opened but produced nothing.
    const Result *pick = nullptr;
    for (const auto &r : all) {
        if (r.opened && r.frames == 0) { pick = &r; break; }
    }
    if (!pick) {
        for (const auto &r : all) {
            if (r.opened && !r.passed()) { pick = &r; break; }
        }
    }
    if (!pick) return;

    printf("\n");
    printf("==========================================================\n");
    printf(" Verbose retry: %s %ux%u\n", pick->formatName, pick->w, pick->h);
    printf(" Everything below is driver-level detail. This is the part\n");
    printf(" worth pasting into a forum post or a bug report.\n");
    printf("==========================================================\n\n");

    esp_log_level_set("uvc", ESP_LOG_DEBUG);
    esp_log_level_set("uvc-control", ESP_LOG_DEBUG);
    esp_log_level_set("uvc-isoc", ESP_LOG_DEBUG);
    esp_log_level_set("uvc-bulk", ESP_LOG_DEBUG);
    esp_log_level_set("uvc-stream", ESP_LOG_DEBUG);
    esp_log_level_set("USBH", ESP_LOG_DEBUG);
    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    FormatEntry fmt = { pick->kind, pick->formatName };
    tryCandidate(fmt, { pick->w, pick->h }, pick->askedFps);

    esp_log_level_set("uvc", ESP_LOG_INFO);
    esp_log_level_set("uvc-control", ESP_LOG_INFO);
    esp_log_level_set("uvc-isoc", ESP_LOG_INFO);
    esp_log_level_set("uvc-bulk", ESP_LOG_INFO);
    esp_log_level_set("uvc-stream", ESP_LOG_INFO);
    esp_log_level_set("USBH", ESP_LOG_INFO);
}

// --- usb plumbing ---------------------------------------------------------

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

// Blocks until a camera answers, so the sweep does not report 120 failures
// because nothing was plugged in.
bool waitForCamera()
{
    ESP_LOGI(TAG, "waiting for a camera");

    uvc_host_stream_config_t cfg = {};
    cfg.event_cb = streamCb;
    cfg.frame_cb = frameCb;
    cfg.usb.vid  = UVC_HOST_ANY_VID;
    cfg.usb.pid  = UVC_HOST_ANY_PID;
    cfg.usb.uvc_stream_index = 0;
    cfg.vs_format.h_res  = 320;
    cfg.vs_format.v_res  = 240;
    cfg.vs_format.fps    = 0;                      // whatever it likes
    cfg.vs_format.format = UVC_VS_FORMAT_MJPEG;
    cfg.advanced.number_of_frame_buffers = 2;
    cfg.advanced.number_of_urbs          = 2;
    cfg.advanced.urb_size                = 4 * 1024;
    cfg.advanced.frame_heap_caps         = MALLOC_CAP_SPIRAM;

    for (int attempt = 0; attempt < 30; attempt++) {
        uvc_host_stream_hdl_t s = nullptr;
        if (uvc_host_stream_open(&cfg, pdMS_TO_TICKS(2000), &s) == ESP_OK) {
            uvc_host_stream_close(s);
            ESP_LOGI(TAG, "camera present");
            vTaskDelay(pdMS_TO_TICKS(500));
            return true;
        }
        // An open failure here can also mean "present but refuses 320x240
        // MJPEG", which is fine: the sweep will find what it does accept.
        // Only give up if nothing responds at all for a full minute.
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return false;
}

} // namespace

// --- main -----------------------------------------------------------------

extern "C" void app_main()
{
    printf("\n\n");
    printf("==========================================================\n");
    printf(" USB camera probe\n");
    printf(" Streams every format the camera might support and reports\n");
    printf(" what actually arrived. Let it run to the end.\n");
    printf("==========================================================\n\n");

    const usb_host_config_t hostCfg = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LOWMED,
    };
    ESP_ERROR_CHECK(usb_host_install(&hostCfg));

    xTaskCreatePinnedToCore(usbLibTask, "usb_lib", 4096, nullptr, 15, nullptr,
                            tskNO_AFFINITY);

    const uvc_host_driver_config_t drvCfg = {
        .driver_task_stack_size = 6 * 1024,
        .driver_task_priority   = 16,
        .xCoreID                = tskNO_AFFINITY,
        .create_background_task = true,
    };
    ESP_ERROR_CHECK(uvc_host_install(&drvCfg));

    if (!waitForCamera()) {
        ESP_LOGE(TAG, "no camera responded in 15 s. Check VBUS and the cable, "
                      "and make sure the board is in host mode.");
        return;
    }

    const Resolution *resList = probe::kQuickSweep ? kQuickRes : kFullRes;
    const size_t resCount = probe::kQuickSweep
                                ? sizeof(kQuickRes) / sizeof(kQuickRes[0])
                                : sizeof(kFullRes) / sizeof(kFullRes[0]);

    const float *fpsList = probe::kQuickSweep ? kQuickFps : kFullFps;
    const size_t fpsCount = probe::kQuickSweep
                                ? sizeof(kQuickFps) / sizeof(kQuickFps[0])
                                : sizeof(kFullFps) / sizeof(kFullFps[0]);

    const size_t formatCount = sizeof(kFormats) / sizeof(kFormats[0]);
    const size_t total = formatCount * resCount * fpsCount;

    printf("Sweeping %u combinations at %d s each, roughly %u minutes.\n",
           static_cast<unsigned>(total), probe::kSecondsPerCandidate,
           static_cast<unsigned>((total * (probe::kSecondsPerCandidate + 1)) / 60 + 1));

    printHeader();

    std::vector<Result> all;
    all.reserve(total);

    for (size_t f = 0; f < formatCount; f++) {
        for (size_t rr = 0; rr < resCount; rr++) {
            for (size_t ff = 0; ff < fpsCount; ff++) {
                if (g_live.disconnected.load()) {
                    printf("\nCamera vanished, stopping the sweep early.\n");
                    goto done;
                }
                Result r = tryCandidate(kFormats[f], resList[rr], fpsList[ff]);
                printRow(r);
                all.push_back(r);
            }
        }
    }

done:
    printf("------------------------------------------------------------------------------------------\n");

    // Pick the best pass: highest pixel throughput that actually worked.
    const Result *best = nullptr;
    uint64_t bestScore = 0;
    for (const auto &r : all) {
        if (!r.passed()) continue;
        const uint64_t score =
            static_cast<uint64_t>(r.w) * r.h * static_cast<uint64_t>(r.realFps * 10);
        if (score > bestScore) { bestScore = score; best = &r; }
    }

    if (best) {
        printf("\nBest working combination: %s %ux%u at %.1f fps measured.\n",
               best->formatName, best->w, best->h, best->realFps);
        printSuggestion(*best);
    } else {
        printf("\nNothing streamed cleanly.\n");
    }

    printAdvice(all, best != nullptr);

    if (probe::kVerboseRetry && !best) {
        verboseRetry(all);
    }

    printf("\nProbe finished. Save this log.\n");
}
