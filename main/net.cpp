#include "net.h"

#include <cinttypes>
#include <cstring>

#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mdns.h"
#include "nvs_flash.h"

// The ESP-Hosted transport to the C6. On a P4 target this header comes from
// the espressif/esp_hosted managed component.
#include "esp_hosted.h"

// The call that reports the co-processor's firmware version has been renamed
// once or twice across esp_hosted releases. If this block does not compile,
// set this to 0: it only affects a diagnostic log line.
#define HAVE_HOSTED_VERSION_API 1

static const char *TAG = "net";

namespace {

EventGroupHandle_t g_events = nullptr;
constexpr int kGotIpBit  = BIT0;
constexpr int kGaveUpBit = BIT1;

int         g_retries = 0;
bool        g_apMode  = false;
std::string g_ip      = "0.0.0.0";
std::string g_slaveVer = "unknown";

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
            xEventGroupSetBits(g_events, kGaveUpBit);
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(data);
        char buf[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, buf, sizeof(buf));
        g_ip = buf;
        ESP_LOGI(TAG, "station ip: %s", buf);
        g_retries = 0;
        xEventGroupSetBits(g_events, kGotIpBit);
    }
}

// Starts the SDIO link to the C6. Nothing in the WiFi stack may be touched
// before this returns.
bool startCoprocessor()
{
    ESP_LOGI(TAG, "bringing up the C6 co-processor over SDIO");

    esp_err_t err = esp_hosted_init();
    if (err == ESP_ERR_INVALID_STATE) {
        // Some builds initialise hosted during the bootloader stage, in
        // which case it is already up and this is not an error.
        ESP_LOGI(TAG, "co-processor already initialised");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_hosted_connect_to_slave();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not reach the C6: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "check the SDIO wiring, and that the C6 has ESP-Hosted "
                      "firmware flashed");
        return false;
    }

#if HAVE_HOSTED_VERSION_API
    esp_hosted_coprocessor_fwver_t ver = {};
    if (esp_hosted_get_coprocessor_fwversion(&ver) == ESP_OK) {
        char buf[24];
        // ver.major1/minor1/patch1 are uint32_t. Plain %d expects a signed
        // int and this toolchain treats that mismatch as a hard error
        // (-Werror=format=); PRIu32 (from <cinttypes>) is the portable way
        // to format a uint32_t regardless of how the target typedefs it.
        snprintf(buf, sizeof(buf), "%" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 ver.major1, ver.minor1, ver.patch1);
        g_slaveVer = buf;
        ESP_LOGI(TAG, "co-processor firmware %s", buf);

        if (ver.major1 == 0 && ver.minor1 == 0 && ver.patch1 == 0) {
            ESP_LOGE(TAG, "the C6 has no ESP-Hosted firmware. Flash it once "
                          "to the slave_fw partition before WiFi will work.");
        }
    }
#endif

    ESP_LOGI(TAG, "co-processor ready");
    return true;
}

void startAccessPoint()
{
    g_apMode = true;
    g_ip     = "192.168.4.1";

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

} // namespace

namespace net {

bool start()
{
    // 1. The radio link comes first. Everything else depends on it.
    if (!startCoprocessor()) return false;

    // 2. NVS, which the WiFi stack needs.
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // 3. TCP/IP and the event loop.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 4. Now the WiFi stack, which is forwarded to the C6 by esp_wifi_remote.
    g_events = xEventGroupCreate();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    if (std::strlen(cfg::kStaSsid) == 0) {
        startAccessPoint();
        return true;
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
        g_events, kGotIpBit | kGaveUpBit, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(25000));

    if (bits & kGotIpBit) return true;

    ESP_LOGW(TAG, "could not join that network, switching to access point");
    esp_wifi_stop();
    startAccessPoint();
    return true;
}

void startMdns()
{
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set(cfg::kMdnsName);
    mdns_instance_name_set("Servo rig");
    mdns_service_add(nullptr, "_http", "_tcp", cfg::kHttpPort, nullptr, 0);
    ESP_LOGI(TAG, "also reachable at http://%s.local/", cfg::kMdnsName);
}

bool        apMode()       { return g_apMode; }
std::string ipAddress()    { return g_ip; }
std::string slaveVersion() { return g_slaveVer; }

} // namespace net
