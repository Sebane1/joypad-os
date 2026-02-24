// jocp_ble_server.c - JOCP BLE GATT server (peripheral) for Android/ImuToXInput
// SPDX-License-Identifier: Apache-2.0
//
// Build with att_server + att_db_util (ESP32 bt2usb). Advertises 0xFFF0;
// client writes 12-byte report to 0xFFF1, we notify rumble on 0xFFF2.

#include "jocp_ble_server.h"
#include "core/input_event.h"
#include "core/buttons.h"
#include "core/router/router.h"
#include "platform/platform.h"

#if defined(BTSTACK_USE_ESP32) && defined(ENABLE_LE_PERIPHERAL)

#include "btstack_defines.h"
#include "btstack_event.h"
#include "bluetooth.h"
#include "gap.h"
#include "ble/att_db.h"
#include "ble/att_db_util.h"
#include "ble/att_server.h"
#include "btstack_util.h"
#include <string.h>
#include <stdio.h>

// ============================================================================
// Constants (match .dev/docs/jocp-ble-server-spec.md)
// ============================================================================

#define JOCP_BLE_SVC_UUID_16           0xFFF0
#define JOCP_BLE_CHAR_REPORT_UUID_16   0xFFF1
#define JOCP_BLE_CHAR_FEEDBACK_UUID_16 0xFFF2
#define JOCP_BLE_REPORT_LEN            12
#define JOCP_BLE_RUMBLE_LEN            6

// Dev addr for this virtual BLE JOCP controller (distinct from WiFi 0xE0+)
#define JOCP_BLE_DEV_ADDR              0xE1

// Advertising: connectable undirected, 30ms interval (0x0030 units of 0.625ms)
#define ADV_INTERVAL_MIN               0x0030
#define ADV_INTERVAL_MAX               0x0050
#define ADV_TYPE_CONNECTABLE_UNDIRECTED 0

// Rate limit feedback (same as JOCP TCP)
#define FEEDBACK_INTERVAL_MS           50

// ============================================================================
// State
// ============================================================================

static uint8_t report_value_data[JOCP_BLE_REPORT_LEN];
static uint8_t feedback_value_data[JOCP_BLE_RUMBLE_LEN];

static uint16_t service_start_handle;
static uint16_t service_end_handle;
static uint16_t report_value_handle;
static uint16_t feedback_value_handle;
static uint16_t feedback_cccd_handle;

static hci_con_handle_t jocp_ble_con_handle = HCI_CON_HANDLE_INVALID;
static uint16_t feedback_cccd_value;  // 0 = off, 1 = notify
static uint32_t last_feedback_ms;
static bool initialized;

static att_service_handler_t jocp_ble_service_handler;
static btstack_packet_callback_registration_t hci_event_callback;

// Advertising data: complete list of 16-bit UUIDs (0xFFF0) + local name
// Type 0x03 = complete list of 16-bit UUIDs, 0x09 = complete local name
static uint8_t adv_data[] = {
    0x03, 0x03, 0xF0, 0xFF,           // 16-bit UUID 0xFFF0
    0x0C, 0x09, 'J','o','y','p','a','d','O','S','-','B','L','E'  // "JoypadOS-BLE"
};
#define ADV_DATA_LEN (sizeof(adv_data))

// ============================================================================
// 12-byte Xbox 360 report -> input_event_t
// ============================================================================
// Layout: lx(2), ly(2), rx(2), ry(2), buttons(2), lt(1), rt(1) little-endian
// Xbox button bits: 0=A, 1=B, 2=X, 3=Y, 4=LB, 5=RB, 6=Back, 7=Start, 8=LS, 9=RS,
//                   12=Up, 13=Down, 14=Left, 15=Right

static void report_to_input_event(const uint8_t* report, input_event_t* ev)
{
    memset(ev, 0, sizeof(*ev));
    ev->dev_addr = JOCP_BLE_DEV_ADDR;
    ev->instance = 0;
    ev->type = INPUT_TYPE_GAMEPAD;
    ev->transport = INPUT_TRANSPORT_BT_BLE;

    int16_t lx = (int16_t)(report[0] | (report[1] << 8));
    int16_t ly = (int16_t)(report[2] | (report[3] << 8));
    int16_t rx = (int16_t)(report[4] | (report[5] << 8));
    int16_t ry = (int16_t)(report[6] | (report[7] << 8));
    uint16_t buttons = report[8] | (report[9] << 8);
    uint8_t lt = report[10];
    uint8_t rt = report[11];

    // Sticks: int16 (-32768..32767) -> uint8 (0..255), 128 center. Y: HID = up 0, down 255 -> invert
    int32_t v;
    v = ((int32_t)lx + 32768) >> 8; ev->analog[0] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    v = ((int32_t)(-ly) + 32768) >> 8; ev->analog[1] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    v = ((int32_t)rx + 32768) >> 8; ev->analog[2] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    v = ((int32_t)(-ry) + 32768) >> 8; ev->analog[3] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    ev->analog[4] = lt;
    ev->analog[5] = rt;

    if (buttons & (1 << 0))  ev->buttons |= JP_BUTTON_B1;
    if (buttons & (1 << 1))  ev->buttons |= JP_BUTTON_B2;
    if (buttons & (1 << 2))  ev->buttons |= JP_BUTTON_B3;
    if (buttons & (1 << 3))  ev->buttons |= JP_BUTTON_B4;
    if (buttons & (1 << 4))  ev->buttons |= JP_BUTTON_L1;
    if (buttons & (1 << 5))  ev->buttons |= JP_BUTTON_R1;
    if (buttons & (1 << 6))  ev->buttons |= JP_BUTTON_L2;
    if (buttons & (1 << 7))  ev->buttons |= JP_BUTTON_R2;
    if (buttons & (1 << 8))  ev->buttons |= JP_BUTTON_S1;
    if (buttons & (1 << 9))  ev->buttons |= JP_BUTTON_S2;
    if (buttons & (1 << 10)) ev->buttons |= JP_BUTTON_L3;
    if (buttons & (1 << 11)) ev->buttons |= JP_BUTTON_R3;
    if (buttons & (1 << 12)) ev->buttons |= JP_BUTTON_DU;
    if (buttons & (1 << 13)) ev->buttons |= JP_BUTTON_DD;
    if (buttons & (1 << 14)) ev->buttons |= JP_BUTTON_DL;
    if (buttons & (1 << 15)) ev->buttons |= JP_BUTTON_DR;
}

// ============================================================================
// ATT write callback
// ============================================================================

static int jocp_ble_write_cb(hci_con_handle_t con_handle, uint16_t attribute_handle,
                             uint16_t transaction_mode, uint16_t offset,
                             uint8_t* buffer, uint16_t buffer_size)
{
    (void)offset;
    if (transaction_mode != ATT_TRANSACTION_MODE_NONE)
        return 0;

    if (attribute_handle == report_value_handle) {
        if (buffer_size >= JOCP_BLE_REPORT_LEN) {
            memcpy(report_value_data, buffer, JOCP_BLE_REPORT_LEN);
            input_event_t event;
            report_to_input_event(report_value_data, &event);
            router_submit_input(&event);
        }
        return 0;
    }

    if (attribute_handle == feedback_cccd_handle) {
        if (buffer_size >= 2) {
            feedback_cccd_value = little_endian_read_16(buffer, 0);
            jocp_ble_con_handle = (feedback_cccd_value != 0) ? con_handle : HCI_CON_HANDLE_INVALID;
        }
        return 0;
    }

    return 0;
}

// ============================================================================
// HCI event: connection / disconnection (peripheral role)
// ============================================================================

static void jocp_ble_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size)
{
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event_type = packet[0];
    if (event_type == HCI_EVENT_LE_META) {
        uint8_t subevent = packet[2];
        if (subevent == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
            uint8_t status = packet[3];
            hci_con_handle_t handle = little_endian_read_16(packet, 4);
            uint8_t role = packet[6];  // 0 = central (we initiated), 1 = peripheral (they connected to us)
            if (status == 0 && role == 1) {
                jocp_ble_con_handle = handle;
                gap_advertisements_enable(0);
                printf("[jocp_ble] Client connected, handle %u\n", (unsigned)handle);
            }
        }
    } else if (event_type == HCI_EVENT_DISCONNECTION_COMPLETE) {
        hci_con_handle_t handle = little_endian_read_16(packet, 3);
        if (handle == jocp_ble_con_handle) {
            jocp_ble_con_handle = HCI_CON_HANDLE_INVALID;
            feedback_cccd_value = 0;
            gap_advertisements_enable(1);
            printf("[jocp_ble] Client disconnected\n");
        }
    }
}

// ============================================================================
// Public API
// ============================================================================

void jocp_ble_server_init(void)
{
    if (initialized) return;

    att_db_util_init();
    service_start_handle = att_db_util_add_service_uuid16(JOCP_BLE_SVC_UUID_16);
    report_value_handle = att_db_util_add_characteristic_uuid16(
        JOCP_BLE_CHAR_REPORT_UUID_16,
        ATT_PROPERTY_WRITE | ATT_PROPERTY_WRITE_WITHOUT_RESPONSE,
        ATT_SECURITY_NONE, ATT_SECURITY_NONE,
        report_value_data, JOCP_BLE_REPORT_LEN);
    feedback_value_handle = att_db_util_add_characteristic_uuid16(
        JOCP_BLE_CHAR_FEEDBACK_UUID_16,
        ATT_PROPERTY_NOTIFY,
        ATT_SECURITY_NONE, ATT_SECURITY_NONE,
        feedback_value_data, JOCP_BLE_RUMBLE_LEN);
    // CCCD is value_handle + 1 for notify characteristic
    feedback_cccd_handle = feedback_value_handle + 1;
    service_end_handle = feedback_cccd_handle;

    att_server_init(att_db_util_get_address(), NULL, NULL);

    jocp_ble_service_handler.start_handle   = service_start_handle;
    jocp_ble_service_handler.end_handle     = service_end_handle;
    jocp_ble_service_handler.read_callback  = NULL;
    jocp_ble_service_handler.write_callback = jocp_ble_write_cb;
    jocp_ble_service_handler.packet_handler = jocp_ble_packet_handler;
    att_server_register_service_handler(&jocp_ble_service_handler);

    hci_event_callback.callback = jocp_ble_packet_handler;
    hci_add_event_handler(&hci_event_callback);

    gap_advertisements_set_data(ADV_DATA_LEN, adv_data);
    gap_advertisements_set_params(ADV_INTERVAL_MIN, ADV_INTERVAL_MAX,
                                  ADV_TYPE_CONNECTABLE_UNDIRECTED,
                                  0, NULL, 0x07, 0);
    gap_advertisements_enable(1);

    last_feedback_ms = 0;
    initialized = true;
    printf("[jocp_ble] Server init, advertising JOCP 0xFFF0\n");
}

void jocp_ble_send_feedback(const output_feedback_t* fb)
{
    if (!initialized || !fb) return;
    if (jocp_ble_con_handle == HCI_CON_HANDLE_INVALID || feedback_cccd_value == 0) return;

    uint32_t now = platform_time_ms();
    if (now - last_feedback_ms < FEEDBACK_INTERVAL_MS) return;
    last_feedback_ms = now;

    feedback_value_data[0] = fb->rumble_left;
    feedback_value_data[1] = 0;
    feedback_value_data[2] = fb->rumble_right;
    feedback_value_data[3] = 0;
    little_endian_store_16(feedback_value_data, 4, 0);  // duration_ms = 0 (until changed)

    att_server_notify(jocp_ble_con_handle, feedback_value_handle, feedback_value_data, JOCP_BLE_RUMBLE_LEN);
}

bool jocp_ble_is_connected(void)
{
    return initialized && (jocp_ble_con_handle != HCI_CON_HANDLE_INVALID);
}

#else
// Stubs when not building with BLE peripheral (RP2040 or no att_server)

void jocp_ble_server_init(void) { (void)0; }
void jocp_ble_send_feedback(const output_feedback_t* fb) { (void)fb; }
bool jocp_ble_is_connected(void) { return false; }

#endif // BTSTACK_USE_ESP32 && ENABLE_LE_PERIPHERAL
