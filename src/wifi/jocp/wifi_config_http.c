// wifi_config_http.c - Optional WiFi config page (ESP32 only)
// SPDX-License-Identifier: Apache-2.0
//
// When the dongle is in AP mode, runs a small HTTP server on port 80 so
// users can enter router WiFi credentials. On submit, credentials are saved
// to NVS and the device reboots; on next boot it connects to that router (STA).

#if defined(PLATFORM_ESP32) || defined(__ESP32__)

#include "wifi_config_esp32.h"
#include "platform/platform.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <string.h>
#include <stdlib.h>

static const char* TAG = "wifi_cfg_http";

#define CONFIG_HTTP_PORT 80
#define REBOOT_DELAY_MS  2000

static httpd_handle_t server = NULL;
static TimerHandle_t reboot_timer = NULL;
static bool sta_only_mode = false;

// STA mode: simple page to clear WiFi and return to AP on next boot
static const char HTML_STA_PAGE[] =
    "<!DOCTYPE html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Joypad WiFi</title></head><body style=\"font-family:sans-serif;max-width:320px;margin:2em auto\">"
    "<h1>WiFi</h1>"
    "<p>Dongle is connected to your router. To switch back to access point mode (JOYPAD-XXXX), clear saved WiFi.</p>"
    "<form method=post action=/wifi/clear>"
    "<button type=submit style=\"background:#c00;color:#fff;border:none;padding:0.5em 1em;font-size:1em\">Clear saved WiFi and reboot</button>"
    "</form>"
    "<p style=\"margin-top:1em;color:#666;font-size:0.9em\">Next boot will start as access point. Connect to JOYPAD-XXXX to set a different network.</p>"
    "</body></html>";

static const char HTML_FORM[] =
    "<!DOCTYPE html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Joypad WiFi</title></head><body style=\"font-family:sans-serif;max-width:320px;margin:2em auto\">"
    "<h1>WiFi setup</h1>"
    "<p>Connect this device to your router so your phone and dongle are on the same network.</p>"
    "<form method=post action=/wifi>"
    "<label>SSID<br><input type=text name=ssid required maxlength=32 style=\"width:100%%;box-sizing:border-box\"></label><br><br>"
    "<label>Password<br><input type=password name=password maxlength=64 style=\"width:100%%;box-sizing:border-box\"></label><br><br>"
    "<button type=submit>Save and connect</button>"
    "</form>"
    "<p style=\"margin-top:1.5em;padding-top:1em;border-top:1px solid #ccc\">"
    "<form method=post action=/wifi/clear style=\"display:inline\">"
    "<button type=submit style=\"background:#c00;color:#fff;border:none;padding:0.4em 0.8em\">Clear saved WiFi</button>"
    "</form>"
    " &mdash; Next boot will start as access point again.</p>"
    "</body></html>";

static void reboot_timer_cb(TimerHandle_t t)
{
    (void)t;
    platform_reboot();
}

static int urldecode(char* out, size_t out_size, const char* in, size_t in_len)
{
    size_t j = 0;
    for (size_t i = 0; i < in_len && j + 1 < out_size; i++) {
        if (in[i] == '+') {
            out[j++] = ' ';
        } else if (in[i] == '%' && i + 2 < in_len) {
            char a = in[i + 1], b = in[i + 2];
            int ha = (a >= '0' && a <= '9') ? (a - '0') : (a >= 'A' && a <= 'F') ? (a - 'A' + 10) : (a >= 'a' && a <= 'f') ? (a - 'a' + 10) : -1;
            int hb = (b >= '0' && b <= '9') ? (b - '0') : (b >= 'A' && b <= 'F') ? (b - 'A' + 10) : (b >= 'a' && b <= 'f') ? (b - 'a' + 10) : -1;
            if (ha >= 0 && hb >= 0) {
                out[j++] = (char)((ha << 4) | hb);
                i += 2;
            } else {
                out[j++] = in[i];
            }
        } else {
            out[j++] = in[i];
        }
    }
    out[j] = '\0';
    return (int)j;
}

static void parse_form(const char* body, size_t len, char* ssid, size_t ssid_size, char* pass, size_t pass_size)
{
    ssid[0] = pass[0] = '\0';
    const char* p = body;
    const char* end = body + len;
    while (p < end) {
        const char* amp = (const char*)memchr(p, '&', (size_t)(end - p));
        size_t seg_len = amp ? (size_t)(amp - p) : (size_t)(end - p);
        if (seg_len >= 5 && memcmp(p, "ssid=", 5) == 0) {
            urldecode(ssid, ssid_size, p + 5, seg_len - 5);
        } else if (seg_len >= 9 && memcmp(p, "password=", 9) == 0) {
            urldecode(pass, pass_size, p + 9, seg_len - 9);
        }
        p = amp ? amp + 1 : end;
    }
}

static esp_err_t get_root_handler(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html");
    if (sta_only_mode) {
        httpd_resp_send(req, HTML_STA_PAGE, sizeof(HTML_STA_PAGE) - 1);
    } else {
        httpd_resp_send(req, HTML_FORM, sizeof(HTML_FORM) - 1);
    }
    return ESP_OK;
}

static esp_err_t post_wifi_handler(httpd_req_t* req)
{
    size_t total = req->content_len;
    if (total == 0 || total > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_FAIL;
    }
    char* body = (char*)malloc(total + 1);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int r = httpd_req_recv(req, body, total);
    if (r <= 0 || (size_t)r != total) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read error");
        return ESP_FAIL;
    }
    body[total] = '\0';

    char ssid[33];
    char pass[65];
    parse_form(body, total, ssid, sizeof(ssid), pass, sizeof(pass));
    free(body);

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    if (!wifi_sta_creds_save(ssid, pass)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    const char* msg = "Saved. Rebooting in 2s...";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, msg, strlen(msg));

    if (reboot_timer) xTimerStart(reboot_timer, 0);
    return ESP_OK;
}

static esp_err_t post_wifi_clear_handler(httpd_req_t* req)
{
    wifi_sta_creds_clear();
    const char* msg = "Cleared. Rebooting in 2s...";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, msg, strlen(msg));
    if (reboot_timer) xTimerStart(reboot_timer, 0);
    return ESP_OK;
}

void wifi_config_http_start(bool sta_only)
{
    if (server) return;

    sta_only_mode = sta_only;
    reboot_timer = xTimerCreate("reboot", pdMS_TO_TICKS(REBOOT_DELAY_MS), pdFALSE, NULL, reboot_timer_cb);
    if (!reboot_timer) {
        ESP_LOGE(TAG, "Timer create failed");
        return;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CONFIG_HTTP_PORT;
    cfg.max_uri_handlers = 8;

    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return;
    }

    httpd_uri_t get_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = get_root_handler,
    };
    httpd_uri_t post_wifi = {
        .uri = "/wifi",
        .method = HTTP_POST,
        .handler = post_wifi_handler,
    };
    httpd_uri_t post_wifi_clear = {
        .uri = "/wifi/clear",
        .method = HTTP_POST,
        .handler = post_wifi_clear_handler,
    };
    httpd_register_uri_handler(server, &get_root);
    httpd_register_uri_handler(server, &post_wifi);
    httpd_register_uri_handler(server, &post_wifi_clear);

    if (sta_only_mode) {
        ESP_LOGI(TAG, "Config server (STA) http://<dongle-ip>:%d/", CONFIG_HTTP_PORT);
    } else {
        ESP_LOGI(TAG, "Config server http://192.168.4.1:%d/", CONFIG_HTTP_PORT);
    }
}

void wifi_config_http_stop(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    if (reboot_timer) {
        xTimerStop(reboot_timer, 0);
    }
}

#else

void wifi_config_http_start(bool sta_only) { (void)sta_only; }
void wifi_config_http_stop(void)  { (void)0; }

#endif
