// cdc_commands.h - CDC command handlers
// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Robert Dale Smith

#ifndef CDC_COMMANDS_H
#define CDC_COMMANDS_H

#include "cdc_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize command handlers
void cdc_commands_init(void);

// Task function - call periodically to handle auto-stop rumble, etc.
void cdc_commands_task(void);

// Process a received command packet
// Called by protocol layer when valid CMD packet received
void cdc_commands_process(const cdc_packet_t* packet);

// Get protocol context (for sending events)
cdc_protocol_t* cdc_commands_get_protocol(void);

// Send input event (if streaming enabled) - raw input before profile mapping
void cdc_commands_send_input_event(uint32_t buttons, const uint8_t* axes);

// Send output event (if streaming enabled) - processed output after profile mapping
void cdc_commands_send_output_event(uint32_t buttons, const uint8_t* axes);

// Send controller connect/disconnect event
void cdc_commands_send_connect_event(uint8_t port, const char* name,
                                     uint16_t vid, uint16_t pid);
void cdc_commands_send_disconnect_event(uint8_t port);

// Send a log line immediately as a protocol event (for config.joypad.ai Log).
// Use from button handler etc. so events appear even if printf/ring path is delayed.
void cdc_commands_send_log_line(const char* msg);

#ifdef __cplusplus
}
#endif

#endif // CDC_COMMANDS_H
