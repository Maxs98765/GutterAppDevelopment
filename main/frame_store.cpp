#include "frame_store.h"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "frames";

bool FrameStore::init(size_t capacity)
{
    m_lock = xSemaphoreCreateMutex();
    if (!m_lock) {
        ESP_LOGE(TAG, "could not create mutex");
        return false;
    }

    // Frames are large, so keep them out of internal RAM.
    m_buf = static_cast<uint8_t *>(
        heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!m_buf) {
        ESP_LOGE(TAG, "no PSRAM for a %u byte frame buffer; is PSRAM enabled?",
                 static_cast<unsigned>(capacity));
        vSemaphoreDelete(m_lock);
        m_lock = nullptr;
        return false;
    }

    m_capacity = capacity;
    m_len      = 0;
    m_seq      = 0;
    m_dropped  = 0;
    return true;
}

void FrameStore::deinit()
{
    if (m_buf) {
        heap_caps_free(m_buf);
        m_buf = nullptr;
    }
    if (m_lock) {
        vSemaphoreDelete(m_lock);
        m_lock = nullptr;
    }
    m_capacity = 0;
}

bool FrameStore::put(const uint8_t *data, size_t len)
{
    if (!m_buf || !data || len == 0) return false;

    if (len > m_capacity) {
        m_dropped++;
        ESP_LOGW(TAG, "frame too large: %u bytes, buffer is %u; raise "
                      "cfg::kMaxFrameBytes",
                 static_cast<unsigned>(len), static_cast<unsigned>(m_capacity));
        return false;
    }

    // Never block the USB callback. A missed frame is better than a stall.
    if (xSemaphoreTake(m_lock, 0) != pdTRUE) {
        m_dropped++;
        return false;
    }

    memcpy(m_buf, data, len);
    m_len = len;
    m_seq++;

    xSemaphoreGive(m_lock);
    return true;
}

bool FrameStore::take(uint8_t *dst, size_t dstCapacity, size_t *outLen,
                      uint32_t *seq)
{
    if (!m_buf || !dst || !outLen || !seq) return false;

    if (xSemaphoreTake(m_lock, pdMS_TO_TICKS(50)) != pdTRUE) return false;

    bool haveNew = (m_len > 0) && (m_seq != *seq) && (m_len <= dstCapacity);
    if (haveNew) {
        memcpy(dst, m_buf, m_len);
        *outLen = m_len;
        *seq    = m_seq;
    }

    xSemaphoreGive(m_lock);
    return haveNew;
}
