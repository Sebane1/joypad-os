// wifi_config_esp32.h - ESP32-only WiFi config (STA credentials)
// SPDX-License-Identifier: Apache-2.0
//
// Optional STA credentials stored in NVS. Used by wifi_transport_esp32.c
// and the config HTTP server to save/clear router WiFi for STA mode.

#ifndef WIFI_CONFIG_ESP32_H
#define WIFI_CONFIG_ESP32_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Save SSID and password for STA (connect to router). Called from config page.
bool wifi_sta_creds_save(const char* ssid, const char* pass);

// Clear saved STA credentials (e.g. to force AP-only mode again).
void wifi_sta_creds_clear(void);

// Start the config HTTP server (when in AP mode). Serves GET / with a form
// and POST /wifi to save credentials and reboot.
void wifi_config_http_start(void);
void wifi_config_http_stop(void);

#ifdef __cplusplus
}
#endif

#endif
