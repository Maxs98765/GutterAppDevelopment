// ---------------------------------------------------------------------------
//  app_main.cpp  --  ESP32-P4 hub (Arduino-free build)
//
//    1. MIPI-CSI camera (Arducam IMX708), re-streaming JPEG over WiFi
//    2. HTTP server the Android app talks to
//    3. Direct GPIO control of the Cytron MD13S + worm-gear motor
//
//  HARDWARE CHANGE: step 3 used to be "UART master for the Arduino Uno
//  that drives the servo". ListofMaterials.xlsx has no Arduino Uno and no
//  USB camera anywhere in it, so both of those are gone -- the motor is
//  now driven directly by this chip (motor.cpp) and the camera comes in
//  over the board's own MIPI-CSI connector (mipi_cam.cpp) instead of a
//  USB port.
//
//  The ordering below still matters. The P4 has no radio, so the
//  ESP-Hosted link to the C6 co-processor has to be established before
//  anything touches the WiFi stack. net::start() handles that whole
//  sequence; getting it wrong is the usual reason a new P4 board appears
//  to have broken WiFi.
// ---------------------------------------------------------------------------

#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "frame_store.h"
#include "http_api.h"
#include "mipi_cam.h"
#include "motor.h"
#include "net.h"

static const char *TAG = "hub";

namespace {
FrameStore g_frames;
}

extern "C" void app_main()
{
    // 1. Network first: co-processor, NVS, netif, event loop, WiFi.
    if (!net::start()) {
        ESP_LOGE(TAG, "no network. Motor control still works locally, "
                      "but nothing can reach this board over WiFi.");
    } else {
        net::startMdns();
        ESP_LOGI(TAG, "reachable at http://%s/  (%s)",
                 net::ipAddress().c_str(),
                 net::apMode() ? "own access point" : "joined your network");
    }

    // 2. Motor. No Arduino, no serial link -- this runs directly on the hub.
    if (!motor::init()) {
        ESP_LOGE(TAG, "motor init failed -- check kMotorPwmPin/kMotorDirPin "
                      "in config.h against your wiring");
    }

    // 3. Camera.
    FrameStore *store = nullptr;
    if (cfg::kCameraEnabled) {
        if (g_frames.init(cfg::kMaxFrameBytes) && cam::start(&g_frames)) {
            store = &g_frames;
        } else {
            ESP_LOGE(TAG, "camera did not start; motor control still works");
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
        ESP_LOGI(TAG, "camera=%s frames=%lu dropped=%lu cam_err=%lu heap=%u psram=%u",
                 cam::connected() ? "up" : "down",
                 static_cast<unsigned long>(cam::framesSeen()),
                 static_cast<unsigned long>(g_frames.dropped()),
                 static_cast<unsigned long>(cam::errorCount()),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }
}
