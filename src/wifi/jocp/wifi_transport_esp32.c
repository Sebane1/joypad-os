// wifi_transport_esp32.c - WiFi Transport Layer for JOCP (ESP32-S3)
// SPDX-License-Identifier: Apache-2.0
//
// Implements wifi_transport.h using ESP-IDF WiFi soft-AP and LwIP/BSD sockets.
// Same API as wifi_transport.c (Pico W CYW43) so JOCP and app code are shared.

#include "wifi_transport.h"
#include "jocp.h"
#include "platform/platform.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <string.h>
#include <stdio.h>
#include <fcntl.h>

static const char* TAG = "wifi_jocp";

// ============================================================================
// CONFIG
// ============================================================================

#define MAX_TCP_CLIENTS 4
// Default ESP-IDF soft-AP IP is 192.168.4.1 (no need to set manually)

// ============================================================================
// STATE
// ============================================================================

static wifi_transport_config_t config;
static bool initialized = false;
static bool ap_ready = false;
static esp_netif_t* ap_netif = NULL;

static char ap_ssid[32];
static char ap_password[32];
static char ap_ip_str[16] = "192.168.4.1";

// Pairing mode
static bool pairing_mode = true;
static uint32_t pairing_timeout_ms = 0;
static uint32_t pairing_start_ms = 0;

// UDP socket for JOCP INPUT packets
static int udp_socket = -1;

// TCP server and clients
static int tcp_listen_socket = -1;
typedef struct {
    int fd;
    uint32_t ip;
    uint16_t port;
    bool connected;
} tcp_client_t;
static tcp_client_t tcp_clients[MAX_TCP_CLIENTS];

// ============================================================================
// WIFI INIT
// ============================================================================

static void wifi_ap_event_handler(void* arg, esp_event_base_t event_base,
                                  int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* e = (wifi_event_ap_staconnected_t*)event_data;
        ESP_LOGI(TAG, "Station connected, AID=%d", e->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* e = (wifi_event_ap_stadisconnected_t*)event_data;
        ESP_LOGI(TAG, "Station disconnected, AID=%d", e->aid);
    }
}

static void set_ssid_hidden(bool hidden)
{
    wifi_config_t wifi_config;
    if (esp_wifi_get_config(WIFI_IF_AP, &wifi_config) != ESP_OK) return;
    wifi_config.ap.ssid_hidden = hidden ? 1 : 0;
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    ESP_LOGI(TAG, "SSID %s", hidden ? "hidden" : "visible");
}

bool wifi_transport_init(const wifi_transport_config_t* cfg)
{
    if (initialized) {
        ESP_LOGI(TAG, "Already initialized");
        return true;
    }

    memcpy(&config, cfg, sizeof(config));

    // Unique SSID from MAC (last 2 bytes); fixed simple password (WPA2 needs 8+ chars)
    uint8_t id[8];
    platform_get_unique_id(id, sizeof(id));
    snprintf(ap_ssid, sizeof(ap_ssid), "%s%02X%02X",
             config.ssid_prefix, id[6], id[7]);
    (void)snprintf(ap_password, sizeof(ap_password), "%s", "slimevr1");

    ESP_LOGI(TAG, "Starting AP: %s channel=%d", ap_ssid, config.channel);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_ap_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .channel = config.channel,
            .max_connection = config.max_connections,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .ssid_hidden = pairing_mode ? 0 : 1,
            .pmf_cfg = { .required = false },
        },
    };
    size_t ssid_len = strlen(ap_ssid);
    size_t pwd_len = strlen(ap_password);
    memcpy(wifi_config.ap.ssid, ap_ssid, ssid_len + 1);
    wifi_config.ap.ssid_len = (uint8_t)ssid_len;
    memcpy(wifi_config.ap.password, ap_password, pwd_len + 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Default soft-AP IP is 192.168.4.1
    snprintf(ap_ip_str, sizeof(ap_ip_str), "192.168.4.1");

    // UDP socket for JOCP INPUT
    udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket");
        return false;
    }
    int opt = 1;
    setsockopt(udp_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(config.udp_port);
    if (bind(udp_socket, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "Failed to bind UDP port %d", config.udp_port);
        close(udp_socket);
        udp_socket = -1;
        return false;
    }
    // Non-blocking
    int flags = fcntl(udp_socket, F_GETFL, 0);
    fcntl(udp_socket, F_SETFL, flags | O_NONBLOCK);

    // TCP listen socket for JOCP CONTROL
    tcp_listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_listen_socket < 0) {
        ESP_LOGE(TAG, "Failed to create TCP socket");
        close(udp_socket);
        udp_socket = -1;
        return false;
    }
    setsockopt(tcp_listen_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    addr.sin_port = htons(config.tcp_port);
    if (bind(tcp_listen_socket, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "Failed to bind TCP port %d", config.tcp_port);
        close(tcp_listen_socket);
        close(udp_socket);
        tcp_listen_socket = -1;
        udp_socket = -1;
        return false;
    }
    if (listen(tcp_listen_socket, MAX_TCP_CLIENTS) != 0) {
        ESP_LOGE(TAG, "Failed to listen");
        close(tcp_listen_socket);
        close(udp_socket);
        tcp_listen_socket = -1;
        udp_socket = -1;
        return false;
    }
    flags = fcntl(tcp_listen_socket, F_GETFL, 0);
    fcntl(tcp_listen_socket, F_SETFL, flags | O_NONBLOCK);

    memset(tcp_clients, 0, sizeof(tcp_clients));
    jocp_init();

    initialized = true;
    ap_ready = true;
    ESP_LOGI(TAG, "WiFi transport ready. Connect to %s, then JOCP to %s:%d",
             ap_ssid, ap_ip_str, config.udp_port);
    return true;
}

void wifi_transport_deinit(void)
{
    if (!initialized) return;
    ap_ready = false;

    if (udp_socket >= 0) {
        close(udp_socket);
        udp_socket = -1;
    }
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (tcp_clients[i].connected && tcp_clients[i].fd >= 0) {
            close(tcp_clients[i].fd);
            tcp_clients[i].connected = false;
            tcp_clients[i].fd = -1;
        }
    }
    if (tcp_listen_socket >= 0) {
        close(tcp_listen_socket);
        tcp_listen_socket = -1;
    }

    esp_wifi_stop();
    initialized = false;
    ESP_LOGI(TAG, "WiFi transport deinitialized");
}

// ============================================================================
// TASK
// ============================================================================

static int find_free_tcp_slot(void)
{
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (!tcp_clients[i].connected) return i;
    }
    return -1;
}

void wifi_transport_task(void)
{
    if (!initialized) return;

    uint32_t now = platform_time_ms();

    // Pairing timeout
    if (pairing_mode && pairing_timeout_ms > 0 &&
        (now - pairing_start_ms) >= pairing_timeout_ms) {
        ESP_LOGI(TAG, "Pairing timeout, hiding SSID");
        wifi_transport_set_pairing_mode(false);
    }

    // Accept new TCP connections
    if (tcp_listen_socket >= 0) {
        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int fd = accept(tcp_listen_socket, (struct sockaddr*)&client_addr, &len);
        if (fd >= 0) {
            int slot = find_free_tcp_slot();
            if (slot >= 0) {
                int flags = fcntl(fd, F_GETFL, 0);
                fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                tcp_clients[slot].fd = fd;
                tcp_clients[slot].ip = ntohl(client_addr.sin_addr.s_addr);
                tcp_clients[slot].port = ntohs(client_addr.sin_port);
                tcp_clients[slot].connected = true;
                ESP_LOGI(TAG, "TCP client connected slot %d from %lu:%d",
                        slot, (unsigned long)tcp_clients[slot].ip, tcp_clients[slot].port);
            } else {
                close(fd);
            }
        }
    }

    // Receive UDP (JOCP INPUT packets)
    if (udp_socket >= 0) {
        uint8_t buffer[128];
        struct sockaddr_in src_addr;
        socklen_t addr_len = sizeof(src_addr);
        int len = recvfrom(udp_socket, buffer, sizeof(buffer), 0,
                          (struct sockaddr*)&src_addr, &addr_len);
        if (len > 0) {
            uint32_t src_ip = ntohl(src_addr.sin_addr.s_addr);
            uint16_t src_port = ntohs(src_addr.sin_port);
            jocp_process_input_packet(buffer, (uint16_t)len, src_ip, src_port);
        }
    }

    // TODO: TCP recv for control messages (optional)
}

bool wifi_transport_is_ready(void) { return ap_ready; }

void wifi_transport_restart(void)
{
    ESP_LOGI(TAG, "Restarting WiFi AP...");
    wifi_transport_deinit();
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_transport_init(&config);
}

const char* wifi_transport_get_ssid(void) { return ap_ssid; }
const char* wifi_transport_get_ip(void)   { return ap_ip_str; }

// ============================================================================
// PAIRING MODE
// ============================================================================

void wifi_transport_set_pairing_mode(bool enabled)
{
    if (pairing_mode == enabled) return;
    pairing_mode = enabled;
    pairing_timeout_ms = 0;
    set_ssid_hidden(!enabled);
    ESP_LOGI(TAG, "Pairing mode %s", enabled ? "ON" : "OFF");
}

bool wifi_transport_is_pairing_mode(void) { return pairing_mode; }

void wifi_transport_start_pairing(uint32_t timeout_sec)
{
    pairing_mode = true;
    pairing_start_ms = platform_time_ms();
    pairing_timeout_ms = timeout_sec * 1000;
    set_ssid_hidden(false);
    ESP_LOGI(TAG, "Pairing mode ON for %lu s", (unsigned long)timeout_sec);
}

void wifi_transport_on_controller_connected(void)
{
    if (pairing_mode) {
        ESP_LOGI(TAG, "Controller connected, exiting pairing mode");
        wifi_transport_set_pairing_mode(false);
    }
}

// ============================================================================
// UDP SEND
// ============================================================================

int wifi_transport_send_udp(uint32_t dest_ip, uint16_t dest_port,
                            const uint8_t* data, uint16_t len)
{
    if (udp_socket < 0 || !ap_ready || !data) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(dest_ip);
    addr.sin_port = htons(dest_port);
    int n = sendto(udp_socket, data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
    return (n == (int)len) ? n : -1;
}

// ============================================================================
// TCP
// ============================================================================

int wifi_transport_send_tcp(uint32_t client_id, const uint8_t* data, uint16_t len)
{
    if (client_id >= MAX_TCP_CLIENTS || !tcp_clients[client_id].connected ||
        tcp_clients[client_id].fd < 0 || !data)
        return -1;
    int n = send(tcp_clients[client_id].fd, data, len, 0);
    return (n == (int)len) ? n : -1;
}

int wifi_transport_find_tcp_client_by_ip(uint32_t ip)
{
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (tcp_clients[i].connected && tcp_clients[i].ip == ip)
            return i;
    }
    return -1;
}
