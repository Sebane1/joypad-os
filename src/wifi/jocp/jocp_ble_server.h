// jocp_ble_server.h - JOCP BLE GATT server (peripheral) for Android/ImuToXInput
// SPDX-License-Identifier: Apache-2.0
//
// When built with BTSTACK_USE_ESP32 (and att_server), the dongle advertises
// service 0xFFF0; the client writes 12-byte Xbox 360 reports to 0xFFF1 and
// subscribes to 0xFFF2 for rumble. See .dev/docs/jocp-ble-server-spec.md

#ifndef JOCP_BLE_SERVER_H
#define JOCP_BLE_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include "core/output_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize JOCP BLE server: build GATT DB, register with ATT server, start advertising.
// Call once after BTstack and platform are up (e.g. from bt2usb app init).
// No-op if not built with JOCP_BLE_SERVER (e.g. RP2040).
void jocp_ble_server_init(void);

// Send rumble/feedback to the connected BLE JOCP client (if any).
// Rate-limited; call from the same place as jocp_send_feedback_all().
void jocp_ble_send_feedback(const output_feedback_t* fb);

// Returns true if a BLE JOCP client is connected and we're receiving reports.
bool jocp_ble_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif // JOCP_BLE_SERVER_H
