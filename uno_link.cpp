#include "uno_link.h"

#include <cstdio>
#include <cstring>

#include "config.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "uno";

namespace {

constexpr uart_port_t kPort    = UART_NUM_1;
constexpr int         kRxBytes = 512;
constexpr int         kTxBytes = 256;

SemaphoreHandle_t g_lock = nullptr;
bool              g_ready = false;

int64_t nowMs() { return esp_timer_get_time() / 1000; }

} // namespace

namespace uno {

bool init()
{
    g_lock = xSemaphoreCreateMutex();
    if (!g_lock) return false;

    uart_config_t uc = {};
    uc.baud_rate = cfg::kUnoBaud;
    uc.data_bits = UART_DATA_8_BITS;
    uc.parity    = UART_PARITY_DISABLE;
    uc.stop_bits = UART_STOP_BITS_1;
    uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uc.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_driver_install(kPort, kRxBytes, kTxBytes, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(kPort, &uc));
    ESP_ERROR_CHECK(uart_set_pin(kPort, cfg::kUnoTxPin, cfg::kUnoRxPin,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    g_ready = true;
    ESP_LOGI(TAG, "link up: tx=%d rx=%d @ %d baud",
             cfg::kUnoTxPin, cfg::kUnoRxPin, cfg::kUnoBaud);

    // The Uno may still be booting; give it a moment, then clear its banner.
    vTaskDelay(pdMS_TO_TICKS(300));
    uart_flush_input(kPort);
    return true;
}

std::string ask(const std::string &cmd)
{
    if (!g_ready) return {};
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return {};

    uart_flush_input(kPort);

    std::string line = cmd + "\n";
    uart_write_bytes(kPort, line.c_str(), line.size());

    std::string reply;
    int64_t deadline = nowMs() + cfg::kLinkReplyTimeoutMs;

    while (nowMs() < deadline) {
        uint8_t c = 0;
        int got = uart_read_bytes(kPort, &c, 1, pdMS_TO_TICKS(10));
        if (got != 1) continue;

        if (c == '\n' || c == '\r') {
            if (!reply.empty()) break;
        } else {
            reply.push_back(static_cast<char>(c));
            if (reply.size() > 96) break;
        }
    }

    xSemaphoreGive(g_lock);

    if (reply.empty()) {
        ESP_LOGW(TAG, "no reply to \"%s\" -- check wiring and baud rate",
                 cmd.c_str());
    }
    return reply;
}

Status status()
{
    Status s;
    s.raw = ask("S");
    if (s.raw.rfind("ST ", 0) != 0) return s;

    char state[24] = {0};
    int  angle = 0;
    unsigned long remaining = 0;

    if (sscanf(s.raw.c_str(), "ST %23s %d %lu", state, &angle, &remaining) == 3) {
        s.valid       = true;
        s.state       = state;
        s.angle       = angle;
        s.remainingMs = static_cast<uint32_t>(remaining);
    }
    return s;
}

} // namespace uno
