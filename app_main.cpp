// ---------------------------------------------------------------------------
//  app_main.cpp  --  ESP32-S3 hub
//
//  Does three jobs at once:
//    1. USB host for the UVC camera, re-streaming MJPEG over WiFi
//    2. HTTP server the Android app talks to
//    3. UART master for the Arduino Uno that drives the servo
// ---------------------------------------------------------------------------

#include <cstring>

#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"

#include "frame_store.h"
#include "http_api.h"
#include "uno_link.h"
#include "uvc_source.h"

static const char *TAG = "hub";

namespace {

FrameStore g_frames;

EventGroupHandle_t g_wifiEvents = nullptr;
constexpr int kGotIpBit   = BIT0;
constexpr int kGaveUpBit  = BIT1;
int  g_retries = 0;
bool g_apMode  = false;

void onWifiEvent(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (g_retries < cfg::kStaJoinRetries) {
            g_retries++;
            ESP_LOGW(TAG, "join attempt %d failed, retrying", g_retries);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(g_wifiEvents, kGaveUpBit);
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "station ip: " IPSTR, IP2STR(&event->ip_info.ip));
        g_retries = 0;
        xEventGroupSetBits(g_wifiEvents, kGotIpBit);
    }
}

void startAccessPoint()
{
    g_apMode = true;

    esp_netif_create_default_wifi_ap();

    wifi_config_t wc = {};
    std::strncpy(reinterpret_cast<char *>(wc.ap.ssid), cfg::kApSsid,
                 sizeof(wc.ap.ssid) - 1);
    wc.ap.ssid_len       = std::strlen(cfg::kApSsid);
    wc.ap.channel        = 1;
    wc.ap.max_connection = 4;

    if (std::strlen(cfg::kApPass) >= 8) {
        std::strncpy(reinterpret_cast<char *>(wc.ap.password), cfg::kApPass,
                     sizeof(wc.ap.password) - 1);
        wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wc.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "access point \"%s\" up; the hub is at 192.168.4.1",
             cfg::kApSsid);
}

// Tries the configured network first, falls back to its own AP.
void startWifi()
{
    g_wifiEvents = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    if (std::strlen(cfg::kStaSsid) == 0) {
        startAccessPoint();
        return;
    }

    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &onWifiEvent, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &onWifiEvent, nullptr, nullptr));

    wifi_config_t wc = {};
    std::strncpy(reinterpret_cast<char *>(wc.sta.ssid), cfg::kStaSsid,
                 sizeof(wc.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char *>(wc.sta.password), cfg::kStaPass,
                 sizeof(wc.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "joining \"%s\"", cfg::kStaSsid);

    EventBits_t bits = xEventGroupWaitBits(
        g_wifiEvents, kGotIpBit | kGaveUpBit, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(20000));

    if (bits & kGotIpBit) return;

    ESP_LOGW(TAG, "could not join that network, switching to access point");
    esp_wifi_stop();
    esp_wifi_deinit();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    startAccessPoint();
}

void startMdns()
{
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set(cfg::kMdnsName);
    mdns_instance_name_set("Servo rig");
    mdns_service_add(nullptr, "_http", "_tcp", cfg::kHttpPort, nullptr, 0);
    ESP_LOGI(TAG, "also reachable at http://%s.local/", cfg::kMdnsName);
}

} // namespace

extern "C" void app_main()
{
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    startWifi();
    startMdns();

    if (!uno::init()) {
        ESP_LOGE(TAG, "serial link to the arduino did not come up");
    } else {
        std::string pong = uno::ask("P");
        ESP_LOGI(TAG, "arduino says: %s",
                 pong.empty() ? "(nothing -- check wiring)" : pong.c_str());
    }

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

    if (!api::start(store)) {
        ESP_LOGE(TAG, "http server failed, nothing to do");
        return;
    }

    ESP_LOGI(TAG, "hub ready");

    // Heartbeat, so the serial monitor tells you whether frames are flowing.
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "camera=%s frames=%lu dropped=%lu heap=%u",
                 uvc::connected() ? "up" : "down",
                 static_cast<unsigned long>(uvc::framesSeen()),
                 static_cast<unsigned long>(g_frames.dropped()),
                 static_cast<unsigned>(esp_get_free_heap_size()));
    }
}
