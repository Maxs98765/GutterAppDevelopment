#include "http_api.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "config.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "mipi_cam.h"
#include "motor.h"

static const char *TAG = "api";

namespace {

httpd_handle_t g_server = nullptr;
FrameStore    *g_store  = nullptr;

constexpr char kBoundary[] = "servorigframe";

const char kStreamType[] = "multipart/x-mixed-replace;boundary=" "servorigframe";
const char kPartHeader[] = "\r\n--servorigframe\r\n"
                           "Content-Type: image/jpeg\r\n"
                           "Content-Length: %u\r\n\r\n";

// --- helpers ---------------------------------------------------------------

esp_err_t sendJson(httpd_req_t *req, const char *status, const std::string &body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body.c_str());
}

std::string jsonEscape(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// Reads a single unsigned query parameter. Returns false if absent or bad.
bool queryUint(httpd_req_t *req, const char *key, uint32_t *out)
{
    size_t len = httpd_req_get_url_query_len(req) + 1;
    if (len <= 1) return false;

    std::string query(len, '\0');
    if (httpd_req_get_url_query_str(req, query.data(), len) != ESP_OK) return false;

    char value[16] = {0};
    if (httpd_query_key_value(query.c_str(), key, value, sizeof(value)) != ESP_OK) {
        return false;
    }

    char *end = nullptr;
    unsigned long v = strtoul(value, &end, 10);
    if (end == value || v == 0) return false;

    *out = static_cast<uint32_t>(v);
    return true;
}

// --- video -----------------------------------------------------------------

esp_err_t handleStream(httpd_req_t *req)
{
    if (!g_store) {
        return sendJson(req, "503 Service Unavailable",
                        "{\"ok\":false,\"error\":\"camera not running\"}");
    }

    const size_t cap = g_store->capacity();
    auto *buf = static_cast<uint8_t *>(
        heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        return sendJson(req, "503 Service Unavailable",
                        "{\"ok\":false,\"error\":\"out of memory\"}");
    }

    httpd_resp_set_type(req, kStreamType);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");

    ESP_LOGI(TAG, "stream client attached");

    uint32_t  seq = 0;
    size_t    len = 0;
    char      part[96];
    esp_err_t err = ESP_OK;
    int       idleTicks = 0;

    while (true) {
        if (!g_store->take(buf, cap, &len, &seq)) {
            // No new frame yet. Bail out if the camera has gone quiet for
            // several seconds, so the client sees a clean end rather than
            // an open socket that never delivers.
            if (++idleTicks > 500) {
                ESP_LOGW(TAG, "no frames for 5 s, closing stream");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        idleTicks = 0;

        int n = snprintf(part, sizeof(part), kPartHeader,
                         static_cast<unsigned>(len));

        err = httpd_resp_send_chunk(req, part, n);
        if (err != ESP_OK) break;

        err = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(buf), len);
        if (err != ESP_OK) break;
    }

    heap_caps_free(buf);
    httpd_resp_send_chunk(req, nullptr, 0);   // terminate the response
    ESP_LOGI(TAG, "stream client gone");
    return ESP_OK;
}

esp_err_t handleSnapshot(httpd_req_t *req)
{
    if (!g_store) {
        return sendJson(req, "503 Service Unavailable",
                        "{\"ok\":false,\"error\":\"camera not running\"}");
    }

    const size_t cap = g_store->capacity();
    auto *buf = static_cast<uint8_t *>(
        heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        return sendJson(req, "503 Service Unavailable",
                        "{\"ok\":false,\"error\":\"out of memory\"}");
    }

    // Force a copy of whatever is current by asking for a sequence we cannot
    // already hold, then wait briefly for the next frame if nothing is there.
    uint32_t seq = g_store->sequence() - 1;
    size_t   len = 0;
    bool     got = false;

    for (int i = 0; i < 50 && !got; i++) {
        got = g_store->take(buf, cap, &len, &seq);
        if (!got) vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (!got) {
        heap_caps_free(buf);
        return sendJson(req, "504 Gateway Timeout",
                        "{\"ok\":false,\"error\":\"no frame available\"}");
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, reinterpret_cast<const char *>(buf), len);

    heap_caps_free(buf);
    return err;
}

// --- motor -------------------------------------------------------------
//
// HARDWARE CHANGE: these used to send a command over UART to an Arduino
// Uno and wait for a reply (uno::ask(), with an empty string meaning "the
// Uno did not answer in time"). motor:: calls now run in this same
// process -- there is no serial link and nothing to time out -- so every
// call returns immediately with a real reply ("OK ..." or "ERR ..."). The
// JSON bodies below are unchanged on purpose, so the Android app (which
// only reads "ok"/"error"/"state"/"angle"/"remaining_ms"/"camera") needed
// no changes at all.

esp_err_t handleRotate(httpd_req_t *req)
{
    uint32_t hold = 0;
    bool haveHold = queryUint(req, "hold", &hold);
    // The original (Uno-serial) version of this handler had a second
    // fallback here, cfg::kHoldOverrideMs, for forcing a hold value on
    // requests that didn't specify ?hold=. That constant didn't survive
    // into this rewrite's config.h -- motor::rotate() already falls back to
    // cfg::kHoldMs whenever it's passed 0, which is exactly what a missing
    // ?hold= produces below, so no separate override constant is needed.

    std::string reply = motor::rotate(haveHold ? hold : 0);
    bool ok = reply.rfind("OK", 0) == 0;

    std::string body = "{\"ok\":" + std::string(ok ? "true" : "false");
    if (!ok) body += ",\"error\":\"" + jsonEscape(reply) + "\"";
    body += ",\"reply\":\"" + jsonEscape(reply) + "\"";
    if (haveHold) body += ",\"hold_ms\":" + std::to_string(hold);
    body += "}";
    return sendJson(req, "200 OK", body);
}

esp_err_t handleHome(httpd_req_t *req)
{
    std::string reply = motor::zero();
    return sendJson(req, "200 OK",
                    "{\"ok\":true,\"reply\":\"" + jsonEscape(reply) + "\"}");
}

esp_err_t handleHold(httpd_req_t *req)
{
    uint32_t ms = 0;
    if (!queryUint(req, "ms", &ms)) {
        return sendJson(req, "400 Bad Request",
                        "{\"ok\":false,\"error\":\"add ?ms=<milliseconds>\"}");
    }

    std::string reply = motor::setHold(ms);
    bool ok = reply.rfind("OK", 0) == 0;
    return sendJson(req, "200 OK",
                    "{\"ok\":" + std::string(ok ? "true" : "false") +
                    ",\"hold_ms\":" + std::to_string(ms) +
                    ",\"reply\":\"" + jsonEscape(reply) + "\"}");
}

esp_err_t handleStatus(httpd_req_t *req)
{
    motor::Status s = motor::status();

    std::string body = "{";
    body += "\"ok\":true";
    body += ",\"motor\":true";   // was "arduino":true -- kept camera/app JSON
                                 // fields identical below; this one key just
                                 // no longer names hardware that isn't here
    body += ",\"state\":\"" + jsonEscape(s.state) + "\"";
    body += ",\"angle\":" + std::to_string(s.angle);
    body += ",\"remaining_ms\":" + std::to_string(s.remainingMs);
    body += ",\"camera\":" + std::string(cam::connected() ? "true" : "false");
    body += ",\"frames\":" + std::to_string(cam::framesSeen());
    body += "}";

    return sendJson(req, "200 OK", body);
}

// --- browser test page -----------------------------------------------------

esp_err_t handleRoot(httpd_req_t *req)
{
    static const char kPage[] =
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Servo rig</title><style>"
        "body{margin:0;min-height:100vh;background:#151b24;color:#e8e6e1;"
        "font:400 16px/1.5 system-ui,sans-serif;display:flex;"
        "flex-direction:column;align-items:center;gap:1.25rem;padding:1.5rem}"
        "h1{margin:0;font-size:1.2rem;font-weight:500}"
        "img{width:100%;max-width:480px;background:#0d1219;border-radius:4px}"
        "button{font:inherit;border:0;border-radius:999px;padding:1rem 2.2rem;cursor:pointer}"
        "#go{background:#c8452f;color:#fff}"
        "#zero{background:transparent;color:#9aa4b2;border:1px solid #3a4454}"
        "#out{color:#9aa4b2;font-variant-numeric:tabular-nums;min-height:1.5em}"
        "</style>"
        "<h1>Servo rig</h1>"
        "<img src=/stream alt='Live camera feed'>"
        "<button id=go>Rotate and hold</button>"
        "<button id=zero>Return to 0</button>"
        "<div id=out>Ready</div>"
        "<script>"
        "const o=document.getElementById('out');"
        "async function hit(p){o.textContent='Working';"
        "try{const r=await fetch(p);const j=await r.json();"
        "o.textContent=j.ok?'Done':('Failed: '+(j.error||'unknown'));}"
        "catch(e){o.textContent='No response from the rig';}}"
        "go.onclick=()=>hit('/rotate');zero.onclick=()=>hit('/home');"
        "setInterval(async()=>{try{const j=await(await fetch('/status')).json();"
        "if(j.state==='holding')o.textContent='Holding, '+"
        "(j.remaining_ms/1000).toFixed(1)+' s left';}catch(e){}},400);"
        "</script>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, kPage);
}

} // namespace

namespace api {

bool start(FrameStore *store)
{
    g_store = store;

    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    conf.server_port      = cfg::kHttpPort;
    conf.max_uri_handlers = 10;
    conf.lru_purge_enable = true;
    // The MJPEG handler never returns while a client is attached, so it needs
    // a socket of its own alongside the control requests.
    conf.max_open_sockets = 5;
    conf.stack_size       = 6144;
    conf.recv_wait_timeout = 5;
    conf.send_wait_timeout = 5;

    if (httpd_start(&g_server, &conf) != ESP_OK) {
        ESP_LOGE(TAG, "could not start the http server");
        return false;
    }

    const httpd_uri_t routes[] = {
        { "/",             HTTP_GET, handleRoot,     nullptr },
        { "/stream",       HTTP_GET, handleStream,   nullptr },
        { "/snapshot.jpg", HTTP_GET, handleSnapshot, nullptr },
        { "/rotate",       HTTP_GET, handleRotate,   nullptr },
        { "/home",         HTTP_GET, handleHome,     nullptr },
        { "/hold",         HTTP_GET, handleHold,     nullptr },
        { "/status",       HTTP_GET, handleStatus,   nullptr },
    };

    for (const auto &r : routes) httpd_register_uri_handler(g_server, &r);

    // kHttpPort is uint16_t; see the cast note on the same class of issue
    // in net.cpp/jpeg_hw.cpp/mipi_cam.cpp.
    ESP_LOGI(TAG, "http server listening on port %u",
             static_cast<unsigned>(cfg::kHttpPort));
    return true;
}

void stop()
{
    if (g_server) {
        httpd_stop(g_server);
        g_server = nullptr;
    }
}

} // namespace api
