// app.c - BT2USB App Entry Point
// Bluetooth to USB HID gamepad adapter for Pico W
//
// Uses Pico W's built-in CYW43 Bluetooth to receive controllers,
// outputs as USB HID device.

#include "app.h"
#include "core/router/router.h"
#include "core/services/players/manager.h"
#include "core/services/players/feedback.h"
#include "core/services/button/button.h"
#include "core/input_interface.h"
#include "core/output_interface.h"
#include "usb/usbd/usbd.h"
#include "bt/transport/bt_transport.h"
#include "bt/btstack/btstack_host.h"
#include "core/services/leds/leds.h"

#include "tusb.h"
#include "platform/platform.h"
#include <stdio.h>

#ifdef BTSTACK_USE_ESP32
#include "driver/gpio.h"
#include "wifi/jocp/jocp.h"
#include "wifi/jocp/jocp_ble_server.h"
#include "wifi/jocp/wifi_transport.h"
#include "usb/usbd/cdc/cdc_commands.h"
#include "display_st7735.h"
extern const bt_transport_t bt_transport_esp32;
#define WIFI_AP_SSID_PREFIX "JOYPAD-"
#define JOCP_UDP_PORT 30100
#define JOCP_TCP_PORT 30101
#define WIFI_AP_CHANNEL 6
#define WIFI_MAX_CONNECTIONS 4
#define DISPLAY_STATUS_LINE_Y 16
#define DISPLAY_DEBUG_LINE_Y  24
#define DISPLAY_STATUS_BLINK_MS 400
#define BUTTON_FEEDBACK_MS 2000
// On ESP32 with display we read both GPIO 0 and 9 raw so LCD/actions work regardless of Kconfig.
#define DISPLAY_BUTTON_GPIO0  0
#define DISPLAY_BUTTON_GPIO9  9
#define display_button_pressed()  (!gpio_get_level(DISPLAY_BUTTON_GPIO0) || !gpio_get_level(DISPLAY_BUTTON_GPIO9))
#ifdef BTSTACK_USE_ESP32
static uint32_t button_feedback_until_ms = 0;
static char button_feedback_msg[20] = "";
#endif
// Status LED GPIO (Seeed XIAO ESP32-S3 = GPIO 21, active low)
#ifndef STATUS_LED_GPIO
#define STATUS_LED_GPIO 21
#endif
#ifndef STATUS_LED_ACTIVE_LOW
#define STATUS_LED_ACTIVE_LOW 1
#endif
static bool display_available = false;
#else
#include "pico/cyw43_arch.h"
extern const bt_transport_t bt_transport_cyw43;
#endif

// ============================================================================
// LED STATUS
// ============================================================================

static uint32_t led_last_toggle = 0;
static bool led_state = false;

// Update LED based on connection status
// - Blink (0.8s): No device connected (scanning, connecting, or idle)
// - Solid on: Device connected
static void platform_led_set(bool on)
{
#ifdef BTSTACK_USE_ESP32
    gpio_set_level(STATUS_LED_GPIO, (on ^ STATUS_LED_ACTIVE_LOW) ? 1 : 0);
#else
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on ? 1 : 0);
#endif
}

static void led_status_update(void)
{
    uint32_t now = platform_time_ms();

    if (btstack_classic_get_connection_count() > 0) {
        // Device connected - solid on
        if (!led_state) {
            platform_led_set(true);
            led_state = true;
        }
    } else {
        // No device connected - blink
        if (now - led_last_toggle >= 400) {
            led_state = !led_state;
            platform_led_set(led_state);
            led_last_toggle = now;
        }
    }
}

// ============================================================================
// BUTTON EVENT HANDLER
// ============================================================================

static void on_button_event(button_event_t event)
{
#ifdef BTSTACK_USE_ESP32
    // Show button feedback on LCD so user can confirm the button works
    button_feedback_until_ms = platform_time_ms() + BUTTON_FEEDBACK_MS;
    switch (event) {
        case BUTTON_EVENT_CLICK:       snprintf(button_feedback_msg, sizeof(button_feedback_msg), "Btn: Scan 60s "); break;
        case BUTTON_EVENT_DOUBLE_CLICK: snprintf(button_feedback_msg, sizeof(button_feedback_msg), "Btn: Mode    "); break;
        case BUTTON_EVENT_TRIPLE_CLICK: snprintf(button_feedback_msg, sizeof(button_feedback_msg), "Btn: HID     "); break;
        case BUTTON_EVENT_HOLD:         snprintf(button_feedback_msg, sizeof(button_feedback_msg), "Btn: Clear   "); break;
        default:                        button_feedback_until_ms = 0; break;
    }
    // Send button event to config.joypad.ai Log immediately (does not rely on printf/ring)
    {
        static char log_line[64];
        const char* name = "?";
        switch (event) {
            case BUTTON_EVENT_CLICK:       name = "CLICK"; break;
            case BUTTON_EVENT_DOUBLE_CLICK: name = "DOUBLE_CLICK"; break;
            case BUTTON_EVENT_TRIPLE_CLICK: name = "TRIPLE_CLICK"; break;
            case BUTTON_EVENT_HOLD:         name = "HOLD"; break;
            default: break;
        }
        snprintf(log_line, sizeof(log_line), "[app] Button: %s", name);
        cdc_commands_send_log_line(log_line);
    }
#endif
    switch (event) {
        case BUTTON_EVENT_CLICK:
            // Start/extend 60-second BT scan for additional devices
            printf("[app:bt2usb] CLICK -> request timed scan 60s\n");
#ifdef BTSTACK_USE_ESP32
            btstack_host_request_timed_scan(60000);  // Runs in BTstack task
            wifi_transport_start_pairing(30);
#else
            btstack_host_start_timed_scan(60000);
#endif
            break;

        case BUTTON_EVENT_DOUBLE_CLICK: {
            // Double-click to cycle USB output mode
            printf("[app:bt2usb] Double-click - switching USB output mode...\n");
            tud_task_ext(1, false);
            platform_sleep_ms(50);
            tud_task_ext(1, false);

            usb_output_mode_t next = usbd_get_next_mode();
            printf("[app:bt2usb] Switching to %s\n", usbd_get_mode_name(next));
            if (!usbd_set_mode(next)) {
                printf("[app:bt2usb] usbd_set_mode returned false (no reboot)\n");
            }
            break;
        }

        case BUTTON_EVENT_TRIPLE_CLICK:
            // Triple-click to reset to default HID mode
            printf("[app:bt2usb] Triple-click - resetting to HID mode...\n");
            if (!usbd_reset_to_hid()) {
                printf("[app:bt2usb] Already in HID mode\n");
            }
            break;

        case BUTTON_EVENT_HOLD:
            // Long press to disconnect all devices and clear all bonds
            printf("[app:bt2usb] HOLD -> request disconnect+clear\n");
#ifdef BTSTACK_USE_ESP32
            btstack_host_request_disconnect_clear();  // Runs in BTstack task; WiFi restart done in app_task
#else
            btstack_host_disconnect_all_devices();
            btstack_host_delete_all_bonds();
#endif
            break;

        default:
            break;
    }
}

// ============================================================================
// APP INPUT INTERFACES
// ============================================================================

// BT2USB has no InputInterface - BT transport handles input internally
// via bthid drivers that call router_submit_input()

const InputInterface** app_get_input_interfaces(uint8_t* count)
{
    *count = 0;
    return NULL;
}

// ============================================================================
// APP OUTPUT INTERFACES
// ============================================================================

static const OutputInterface* output_interfaces[] = {
    &usbd_output_interface,
};

const OutputInterface** app_get_output_interfaces(uint8_t* count)
{
    *count = sizeof(output_interfaces) / sizeof(output_interfaces[0]);
    return output_interfaces;
}

// ============================================================================
// APP INITIALIZATION
// ============================================================================

void app_init(void)
{
    printf("[app:bt2usb] Initializing BT2USB v%s\n", APP_VERSION);
#ifdef BTSTACK_USE_ESP32
    printf("[app:bt2usb] ESP32-S3 BLE -> USB HID\n");
    // Init status LED GPIO
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << STATUS_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led_cfg);
    gpio_set_level(STATUS_LED_GPIO, STATUS_LED_ACTIVE_LOW ? 1 : 0);  // Start OFF
#else
    printf("[app:bt2usb] Pico W built-in Bluetooth -> USB HID\n");
#endif

    // Initialize button service (uses BOOTSEL button on Pico W)
    button_init();
    button_set_callback(on_button_event);

    // Configure router for BT2USB
    router_config_t router_cfg = {
        .mode = ROUTING_MODE,
        .merge_mode = MERGE_MODE,
        .max_players_per_output = {
            [OUTPUT_TARGET_USB_DEVICE] = USB_OUTPUT_PORTS,
        },
        .merge_all_inputs = true,  // Merge all BT inputs to single output
        .transform_flags = TRANSFORM_FLAGS,
    };
    router_init(&router_cfg);

    // Add default route: BLE Central → USB Device
    router_add_route(INPUT_SOURCE_BLE_CENTRAL, OUTPUT_TARGET_USB_DEVICE, 0);
#ifdef BTSTACK_USE_ESP32
    // ESP32-S3: also accept WiFi JOCP input (same firmware = BLE + WiFi)
    router_add_route(INPUT_SOURCE_WIFI, OUTPUT_TARGET_USB_DEVICE, 0);
#endif

    // Configure player management
    player_config_t player_cfg = {
        .slot_mode = PLAYER_SLOT_MODE,
        .max_slots = MAX_PLAYER_SLOTS,
        .auto_assign_on_press = AUTO_ASSIGN_ON_PRESS,
    };
    players_init_with_config(&player_cfg);

    // Initialize Bluetooth transport
    // Must use bt_init() to set global transport pointer and register drivers
    printf("[app:bt2usb] Initializing Bluetooth...\n");
#ifdef BTSTACK_USE_ESP32
    bt_init(&bt_transport_esp32);
    // Initialize WiFi JOCP (AP mode for controller input over WiFi)
    wifi_transport_config_t wifi_cfg = {
        .ssid_prefix = WIFI_AP_SSID_PREFIX,
        .password = "",
        .channel = WIFI_AP_CHANNEL,
        .max_connections = WIFI_MAX_CONNECTIONS,
        .udp_port = JOCP_UDP_PORT,
        .tcp_port = JOCP_TCP_PORT,
    };
    if (wifi_transport_init(&wifi_cfg)) {
        printf("[app:bt2usb] WiFi JOCP AP: %s (connect for WiFi controllers)\n", wifi_transport_get_ssid());
    } else {
        printf("[app:bt2usb] WARNING: WiFi JOCP init failed\n");
    }
    jocp_ble_server_init();
    // ST7735 display (Pocket-Dongle-S3 / T-Dongle S3)
    display_available = display_init();
    if (display_available) {
        display_clear();
        display_draw_text(0, 0, "Joypad OS");
        display_draw_text(0, 8, "BT+WiFi");
        // Ensure GPIO 0 is input (BOOT) so we can read it; button driver already does GPIO 9
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << DISPLAY_BUTTON_GPIO0),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
    }
#else
    bt_init(&bt_transport_cyw43);
#endif

    printf("[app:bt2usb] Initialization complete\n");
    printf("[app:bt2usb]   Routing: Bluetooth -> USB Device (HID Gamepad)\n");
    printf("[app:bt2usb]   Player slots: %d\n", MAX_PLAYER_SLOTS);
    printf("[app:bt2usb]   Click BOOTSEL for 60s BT scan\n");
    printf("[app:bt2usb]   Hold BOOTSEL to disconnect all + clear bonds\n");
    printf("[app:bt2usb]   Double-click BOOTSEL to switch USB mode\n");
}

// Called from app_task only when BTSTACK_USE_ESP32 and display is present
static void app_display_status_update(bool display_available)
{
#ifdef BTSTACK_USE_ESP32
    if (!display_available) return;
    uint32_t now = platform_time_ms();
    // Direct trigger: read GPIO 0 and 9 raw (active-low). Fires actions on release; does not depend on button driver/Kconfig.
    {
        static bool prev_pressed = false;
        static uint32_t press_start_ms = 0;
        static uint32_t last_release_ms = 0;
        static uint8_t release_count = 0;
        bool pressed = display_button_pressed();
        if (pressed) {
            if (!prev_pressed)
                press_start_ms = now;
            if (release_count == 1)
                release_count = 2;  // second press within window
            prev_pressed = true;
        } else {
            if (release_count == 1 && (now - last_release_ms) >= BUTTON_DOUBLE_CLICK_MS) {
                on_button_event(BUTTON_EVENT_CLICK);
                release_count = 0;
            }
            if (prev_pressed) {
                prev_pressed = false;
                uint32_t held = now - press_start_ms;
                if (held >= BUTTON_HOLD_MS) {
                    on_button_event(BUTTON_EVENT_HOLD);
                    release_count = 0;
                } else if (held < BUTTON_CLICK_MAX_MS) {
                    if (release_count == 2) {
                        on_button_event(BUTTON_EVENT_DOUBLE_CLICK);
                        release_count = 0;
                    } else {
                        last_release_ms = now;
                        release_count = 1;
                    }
                } else {
                    release_count = 0;
                }
            }
        }
    }
    // Debug: raw GP0 and GP9 (L = pressed)
    {
        int l0 = gpio_get_level(DISPLAY_BUTTON_GPIO0);
        int l9 = gpio_get_level(DISPLAY_BUTTON_GPIO9);
        static char dbg[28];
        snprintf(dbg, sizeof(dbg), "G0:%s G9:%s (L=press)", l0 ? "H" : "L", l9 ? "H" : "L");
        display_draw_text(0, DISPLAY_DEBUG_LINE_Y, dbg);
    }
    if (display_button_pressed()) {
        display_draw_text(0, DISPLAY_STATUS_LINE_Y, "Button down   ");
        return;
    }
    // If we recently had a button event (click/double/hold), show that for a few seconds
    if (button_feedback_until_ms != 0 && now < button_feedback_until_ms) {
        display_draw_text(0, DISPLAY_STATUS_LINE_Y, button_feedback_msg);
        return;
    }
    if (button_feedback_until_ms != 0 && now >= button_feedback_until_ms) {
        button_feedback_until_ms = 0;
    }
    uint32_t conn = (uint32_t)btstack_classic_get_connection_count() + (uint32_t)jocp_get_connected_count();
    bool scanning = btstack_host_is_scanning();
    static uint32_t last_status_ms = 0;
    static uint8_t last_conn = 255;
    static bool last_scanning = false;
    static uint8_t scan_dots = 0;

    if (conn > 0) {
        static char buf[20];
        if (conn != last_conn) {
            last_conn = (uint8_t)conn;
            if (conn == 1) {
                snprintf(buf, sizeof(buf), "1 controller  ");
            } else {
                snprintf(buf, sizeof(buf), "%u controllers ", (unsigned)conn);
            }
            display_draw_text(0, DISPLAY_STATUS_LINE_Y, buf);
        }
        return;
    }
    last_conn = 0;

    if (scanning) {
        if (!last_scanning || (now - last_status_ms) >= DISPLAY_STATUS_BLINK_MS) {
            last_scanning = true;
            last_status_ms = now;
            scan_dots = (scan_dots + 1) % 4;
            static const char *scan_strs[] = { "Scanning    ", "Scanning.   ", "Scanning..  ", "Scanning... " };
            display_draw_text(0, DISPLAY_STATUS_LINE_Y, scan_strs[scan_dots]);
        }
        return;
    }
    if (last_scanning) {
        last_scanning = false;
        display_draw_text(0, DISPLAY_STATUS_LINE_Y, "Ready       ");
    }
#else
    (void)display_available;
#endif
}

// ============================================================================
// APP TASK (Called from main loop)
// ============================================================================

void app_task(void)
{
    // Process button input
    button_task();

    // Update LED and display when USB output mode changes
    static usb_output_mode_t last_led_mode = USB_OUTPUT_MODE_COUNT;
    usb_output_mode_t mode = usbd_get_mode();
    if (mode != last_led_mode) {
        uint8_t r, g, b;
        usbd_get_mode_color(mode, &r, &g, &b);
        leds_set_color(r, g, b);
#ifdef BTSTACK_USE_ESP32
        display_clear();
        display_draw_text(0, 0, "Joypad OS");
        display_draw_text(0, 8, usbd_get_mode_name(mode));
#endif
        last_led_mode = mode;
    }

    // Process Bluetooth transport
    bt_task();
#ifdef BTSTACK_USE_ESP32
    // If BTstack task completed disconnect+clear, restart WiFi AP
    if (btstack_host_consume_pending_wifi_restart()) {
        wifi_transport_restart();
    }
    // Process WiFi JOCP (UDP/TCP, submit input via jocp_process_input_packet in transport)
    wifi_transport_task();
#endif

    // Update LED status (BLE + WiFi controller count)
#ifdef BTSTACK_USE_ESP32
    leds_set_connected_devices(btstack_classic_get_connection_count() + jocp_get_connected_count() + (jocp_ble_is_connected() ? 1 : 0));
    app_display_status_update(display_available);
#else
    leds_set_connected_devices(btstack_classic_get_connection_count());
#endif
    led_status_update();

    // Route feedback from USB device output to BT controllers
    if (usbd_output_interface.get_feedback) {
        output_feedback_t fb;
        if (usbd_output_interface.get_feedback(&fb)) {
            for (int i = 0; i < playersCount; i++) {
                feedback_set_rumble(i, fb.rumble_left, fb.rumble_right);
                if (fb.led_player > 0) {
                    feedback_set_led_player(i, fb.led_player);
                }
                if (fb.led_r || fb.led_g || fb.led_b) {
                    feedback_set_led_rgb(i, fb.led_r, fb.led_g, fb.led_b);
                }
            }
#ifdef BTSTACK_USE_ESP32
            // Also send feedback to WiFi JOCP controllers (rumble, LED)
            jocp_send_feedback_all(&fb);
            jocp_ble_send_feedback(&fb);
#endif
        }
    }
}
