// display_st7735.h - ST7735 80x160 LCD driver for Pocket-Dongle-S3 / T-Dongle S3
//
// Minimal SPI driver for 0.96" ST7735 IPS. Use display_init() then
// display_draw_text() / display_clear() to show mode and status.

#ifndef DISPLAY_ST7735_H
#define DISPLAY_ST7735_H

#include <stdbool.h>
#include <stdint.h>

// Initialize SPI and ST7735. Call once after GPIO/SPI is available.
// Returns true on success.
bool display_init(void);

// Clear screen to black.
void display_clear(void);

// Draw a string at (x, y) in 6x8 font. Coordinates in pixels.
// Clips to display bounds (80x160). With TEXT_ROTATE_90, each character is 8x6 and text advances in x.
//
// ASCII only: one byte per character (0x20–0x7F). Bytes outside that range are shown as space.
void display_draw_text(int x, int y, const char *str);

// Optional: set backlight on/off (GPIO 37 on T-Dongle S3).
void display_set_backlight(bool on);

#endif
