#include "jpeg_hw.h"

#if HUB_ENABLE_HW_JPEG

#include "driver/jpeg_encode.h"
#include "esp_log.h"

static const char *TAG = "jpeg";

namespace {

jpeg_encoder_handle_t g_engine = nullptr;
uint8_t *g_out     = nullptr;
size_t   g_outCap  = 0;
uint16_t g_width   = 0;
uint16_t g_height  = 0;
int      g_quality = 80;

} // namespace

namespace jpeg_hw {

bool init(uint16_t width, uint16_t height, int quality, size_t maxOutBytes)
{
    g_width   = width;
    g_height  = height;
    g_quality = quality;

    jpeg_encode_engine_cfg_t engineCfg = {};
    engineCfg.timeout_ms = 70;

    esp_err_t err = jpeg_new_encoder_engine(&engineCfg, &g_engine);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "encoder engine failed: %s", esp_err_to_name(err));
        return false;
    }

    // The encoder writes by DMA, so the output buffer has to come from the
    // JPEG allocator rather than plain malloc.
    jpeg_encode_memory_alloc_cfg_t allocCfg = {};
    allocCfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;

    g_out = static_cast<uint8_t *>(
        jpeg_alloc_encoder_mem(maxOutBytes, &allocCfg, &g_outCap));
    if (!g_out) {
        ESP_LOGE(TAG, "could not allocate a %u byte output buffer",
                 static_cast<unsigned>(maxOutBytes));
        jpeg_del_encoder_engine(g_engine);
        g_engine = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "hardware encoder ready: %ux%u, quality %d, %u byte output",
             width, height, quality, static_cast<unsigned>(g_outCap));
    return true;
}

void deinit()
{
    if (g_out) {
        free(g_out);
        g_out    = nullptr;
        g_outCap = 0;
    }
    if (g_engine) {
        jpeg_del_encoder_engine(g_engine);
        g_engine = nullptr;
    }
}

const uint8_t *encode(const uint8_t *yuy2, size_t yuy2Len, size_t *outLen)
{
    if (!g_engine || !g_out || !yuy2 || !outLen) return nullptr;

    jpeg_encode_cfg_t cfg = {};
    cfg.src_type      = JPEG_ENCODE_IN_FORMAT_YUV422;
    cfg.sub_sample    = JPEG_DOWN_SAMPLING_YUV422;
    cfg.image_quality = g_quality;
    cfg.width         = g_width;
    cfg.height        = g_height;

    uint32_t written = 0;
    esp_err_t err = jpeg_encoder_process(g_engine, &cfg,
                                         yuy2, yuy2Len,
                                         g_out, g_outCap, &written);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "encode failed: %s", esp_err_to_name(err));
        return nullptr;
    }

    *outLen = written;
    return g_out;
}

bool ready() { return g_engine != nullptr && g_out != nullptr; }

} // namespace jpeg_hw

#else  // HUB_ENABLE_HW_JPEG

namespace jpeg_hw {
bool init(uint16_t, uint16_t, int, size_t) { return false; }
void deinit() { }
const uint8_t *encode(const uint8_t *, size_t, size_t *) { return nullptr; }
bool ready() { return false; }
} // namespace jpeg_hw

#endif
