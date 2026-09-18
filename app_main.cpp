// ---------------------------------------------------------------------------
//  app_main.cpp  --  ESP32-P4 hub
//
//    1. USB host for the UVC camera, re-streaming JPEG over WiFi
//    2. HTTP server the Android app talks to
//    3. UART master for the Arduino Uno that drives the servo
//
//  The ordering below matters. The P4 has no radio, so the ESP-Hosted link
//  to the C6 co-processor has to be established before anything touches the
//  WiFi stack. net::start() handles that whole sequence; getting it wrong is
//  the usual reason a new P4 board appears to have broken WiFi.
// ---------------------------------------------------------------------------

#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "frame_store.h"
#include "http_api.h"
#include "net.h"
#include "uno_link.h"
#include "uvc_source.h"

static const char *TAG = "hub";

namespace {
FrameStore g_frames;
}

extern "C" void app_main()
{
    // 1. Network first: co-processor, NVS, netif, event loop, WiFi.
    if (!net::start()) {
        ESP_LOGE(TAG, "no network. The servo link may still work over serial, "
                      "but nothing can reach it.");
    } else {
        net::startMdns();
        ESP_LOGI(TAG, "reachable at http://%s/  (%s)",
                 net::ipAddress().c_str(),
                 net::apMode() ? "own access point" : "joined your network");
    }

    // 2. Serial link to the Uno.
    if (!uno::init()) {
        ESP_LOGE(TAG, "serial link to the arduino did not come up");
    } else {
        std::string pong = uno::ask("P");
        ESP_LOGI(TAG, "arduino says: %s",
                 pong.empty() ? "(nothing -- check the divider, the grounds, "
                                "and the baud rate)"
                              : pong.c_str());
    }

    // 3. Camera.
    FrameStore *store = nullptr;
    if (cfg::kCameraEnabled) {
        if (g_frames.init(cfg::kMaxFrameBytes) && uvc::start(&g_frames)) {
            store = &g_frames;
        } else {
            ESP_LOGE(TAG, "camera did not start; servo control still works");
        }
    } else {
        ESP_LOGI(TAG, "camera disabled in config.h");
    }

    // 4. HTTP.
    if (!api::start(store)) {
        ESP_LOGE(TAG, "http server failed, nothing to do");
        return;
    }

    ESP_LOGI(TAG, "hub ready");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "camera=%s frames=%lu dropped=%lu usb_err=%lu heap=%u psram=%u",
                 uvc::connected() ? "up" : "down",
                 static_cast<unsigned long>(uvc::framesSeen()),
                 static_cast<unsigned long>(g_frames.dropped()),
                 static_cast<unsigned long>(uvc::errorCount()),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }
}
