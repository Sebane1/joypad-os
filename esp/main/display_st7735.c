// display_st7735.c - ST7735 80x160 LCD driver for Pocket-Dongle-S3 / T-Dongle S3
//
// Minimal SPI driver for 0.96" ST7735 IPS. Pinout selectable via Kconfig.

#include "display_st7735.h"
#include "sdkconfig.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "display";

#if CONFIG_DISPLAY_ST7735_PINOUT_SCHEMATIC
// Schematic A: CS=13, DC=12, RST=11, MOSI=14, SCK=16, BL=10
#define PIN_NUM_DC    12
#define PIN_NUM_CS    13
#define PIN_NUM_RST   11
#define PIN_NUM_MOSI  14
#define PIN_NUM_SCK   16
#define PIN_NUM_BL    10
#elif CONFIG_DISPLAY_ST7735_PINOUT_SCHEMATIC_B
// Schematic B: SCLK=10, MOSI=11, CE1/CS=12, DC=13, RST=14, BL=8
#define PIN_NUM_DC    13
#define PIN_NUM_CS    12
#define PIN_NUM_RST   14
#define PIN_NUM_MOSI  11
#define PIN_NUM_SCK   10
#define PIN_NUM_BL    8
#elif CONFIG_DISPLAY_ST7735_PINOUT_POCKET_DEVKIT
// ESP32-S3 Pocket DevKit / Supermini (TFT_eSPI style): CS=9, DC=10, RST=11, MOSI=12, SCK=13, BL=8
#define PIN_NUM_DC    10
#define PIN_NUM_CS    9
#define PIN_NUM_RST   11
#define PIN_NUM_MOSI  12
#define PIN_NUM_SCK   13
#define PIN_NUM_BL    8
#elif CONFIG_DISPLAY_ST7735_PINOUT_POCKET_ALT
// Pocket-Dongle-S3 / N16R8: CS=16, MOSI=17, SCK=18, DC=4, RST=5
#define PIN_NUM_DC    4
#define PIN_NUM_CS    16
#define PIN_NUM_RST   5
#define PIN_NUM_MOSI  17
#define PIN_NUM_SCK   18
#define PIN_NUM_BL    37
#else
// LilyGo T-Dongle S3: DC=2, CS=4, RST=1, MOSI=3, SCK=5
#define PIN_NUM_DC    2
#define PIN_NUM_CS    4
#define PIN_NUM_RST   1
#define PIN_NUM_MOSI  3
#define PIN_NUM_SCK   5
#define PIN_NUM_BL    37
#endif

#define LCD_WIDTH    80
#define LCD_HEIGHT   160
#define SPI_CLOCK_HZ (4 * 1000 * 1000)   // 4 MHz for marginal boards / long traces

#if CONFIG_DISPLAY_ST7735_PANEL_OFFSET_0_0
#define COLSTART     0
#define ROWSTART     0
#else
#define COLSTART     26   // ST7735 80x160 physical offset (LilyGo style)
#define ROWSTART     1
#endif

// ST7735 commands
#define ST7735_NOP     0x00
#define ST7735_SWRESET 0x01
#define ST7735_SLPOUT  0x11
#define ST7735_NORON   0x13
#define ST7735_INVOFF  0x20
#define ST7735_INVON   0x21
#define ST7735_DISPOFF 0x28
#define ST7735_DISPON  0x29
#define ST7735_CASET   0x2A
#define ST7735_RASET   0x2B
#define ST7735_RAMWR   0x2C
#define ST7735_MADCTL  0x36
#define ST7735_COLMOD  0x3A
#define ST7735_FRMCTR1 0xB1
#define ST7735_FRMCTR2 0xB2
#define ST7735_FRMCTR3 0xB3
#define ST7735_INVCTR  0xB4
#define ST7735_PWCTR1  0xC0
#define ST7735_PWCTR2  0xC1
#define ST7735_PWCTR3  0xC2
#define ST7735_PWCTR4  0xC3
#define ST7735_PWCTR5  0xC4
#define ST7735_VMCTR1  0xC5
#define ST7735_GMCTRP1 0xE0
#define ST7735_GMCTRN1 0xE1

static spi_device_handle_t spi;
static bool initialized = false;

// 6x8 font: 96 chars (0x20-0x7F), 6 bytes per char (6 cols x 8 rows), LSB = top.
// Minimal ASCII subset - space through ~. Public-domain style 6x8.
static const uint8_t font6x8[96][6] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, // space
    { 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 }, // !
    { 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00 }, // "
    { 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x00 }, // #
    { 0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E }, // $
    { 0x18, 0x19, 0x02, 0x04, 0x13, 0x03 }, // %
    { 0x08, 0x14, 0x08, 0x15, 0x12, 0x0D }, // &
    { 0x04, 0x04, 0x00, 0x00, 0x00, 0x00 }, // '
    { 0x02, 0x04, 0x08, 0x08, 0x04, 0x02 }, // (
    { 0x08, 0x04, 0x02, 0x02, 0x04, 0x08 }, // )
    { 0x00, 0x04, 0x15, 0x0E, 0x15, 0x04 }, // *
    { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04 }, // +
    { 0x00, 0x00, 0x00, 0x00, 0x04, 0x08 }, // ,
    { 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 }, // -
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 }, // .
    { 0x01, 0x02, 0x04, 0x08, 0x10, 0x00 }, // /
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x0E }, // 0
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x0E }, // 1
    { 0x0E, 0x11, 0x01, 0x0E, 0x10, 0x1F }, // 2
    { 0x0E, 0x11, 0x06, 0x01, 0x11, 0x0E }, // 3
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02 }, // 4
    { 0x1F, 0x10, 0x1E, 0x01, 0x11, 0x0E }, // 5
    { 0x0E, 0x10, 0x1E, 0x11, 0x11, 0x0E }, // 6
    { 0x1F, 0x01, 0x02, 0x04, 0x04, 0x04 }, // 7
    { 0x0E, 0x11, 0x0E, 0x11, 0x11, 0x0E }, // 8
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x0E }, // 9
    { 0x00, 0x04, 0x00, 0x00, 0x04, 0x00 }, // :
    { 0x00, 0x04, 0x00, 0x00, 0x04, 0x08 }, // ;
    { 0x02, 0x04, 0x08, 0x10, 0x08, 0x04 }, // <
    { 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00 }, // =
    { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04 }, // >
    { 0x0E, 0x11, 0x02, 0x04, 0x00, 0x04 }, // ?
    { 0x0E, 0x11, 0x17, 0x15, 0x17, 0x10 }, // @
    { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11 }, // A
    { 0x1E, 0x11, 0x1E, 0x11, 0x11, 0x1E }, // B
    { 0x0E, 0x11, 0x10, 0x10, 0x11, 0x0E }, // C
    { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x1E }, // D
    { 0x1F, 0x10, 0x1E, 0x10, 0x10, 0x1F }, // E
    { 0x1F, 0x10, 0x1E, 0x10, 0x10, 0x10 }, // F
    { 0x0E, 0x11, 0x10, 0x13, 0x11, 0x0F }, // G
    { 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, // H
    { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x0E }, // I
    { 0x01, 0x01, 0x01, 0x01, 0x11, 0x0E }, // J
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12 }, // K
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F }, // L
    { 0x11, 0x1B, 0x15, 0x11, 0x11, 0x11 }, // M
    { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 }, // N
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x0E }, // O
    { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10 }, // P
    { 0x0E, 0x11, 0x11, 0x15, 0x12, 0x0D }, // Q
    { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12 }, // R
    { 0x0E, 0x11, 0x10, 0x0E, 0x01, 0x1E }, // S
    { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04 }, // T
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, // U
    { 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04 }, // V
    { 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 }, // W
    { 0x11, 0x0A, 0x04, 0x04, 0x0A, 0x11 }, // X
    { 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 }, // Y
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x1F }, // Z
    { 0x0E, 0x08, 0x08, 0x08, 0x08, 0x0E }, // [
    { 0x10, 0x08, 0x04, 0x02, 0x01, 0x00 }, // backslash
    { 0x0E, 0x02, 0x02, 0x02, 0x02, 0x0E }, // ]
    { 0x04, 0x0A, 0x11, 0x00, 0x00, 0x00 }, // ^
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F }, // _
    { 0x08, 0x04, 0x00, 0x00, 0x00, 0x00 }, // `
    { 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F }, // a
    { 0x10, 0x1E, 0x11, 0x11, 0x11, 0x1E }, // b
    { 0x00, 0x0E, 0x10, 0x10, 0x11, 0x0E }, // c
    { 0x01, 0x0F, 0x11, 0x11, 0x11, 0x0F }, // d
    { 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E }, // e
    { 0x06, 0x08, 0x1C, 0x08, 0x08, 0x08 }, // f
    { 0x00, 0x0F, 0x11, 0x0F, 0x01, 0x0E }, // g
    { 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11 }, // h
    { 0x04, 0x00, 0x0C, 0x04, 0x04, 0x0E }, // i
    { 0x02, 0x00, 0x06, 0x02, 0x12, 0x0E }, // j
    { 0x10, 0x12, 0x14, 0x18, 0x14, 0x12 }, // k
    { 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }, // l
    { 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11 }, // m
    { 0x00, 0x1E, 0x11, 0x11, 0x11, 0x11 }, // n
    { 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E }, // o
    { 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10 }, // p
    { 0x00, 0x0F, 0x11, 0x0F, 0x01, 0x01 }, // q
    { 0x00, 0x16, 0x18, 0x10, 0x10, 0x10 }, // r
    { 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E }, // s
    { 0x08, 0x1C, 0x08, 0x08, 0x08, 0x06 }, // t
    { 0x00, 0x11, 0x11, 0x11, 0x11, 0x0F }, // u
    { 0x00, 0x11, 0x11, 0x0A, 0x0A, 0x04 }, // v
    { 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A }, // w
    { 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11 }, // x
    { 0x00, 0x11, 0x0A, 0x04, 0x08, 0x10 }, // y
    { 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F }, // z
    { 0x02, 0x04, 0x04, 0x08, 0x04, 0x02 }, // {
    { 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, // |
    { 0x08, 0x04, 0x04, 0x02, 0x04, 0x08 }, // }
    { 0x00, 0x08, 0x15, 0x02, 0x00, 0x00 }, // ~
};

#define FONT_W 6
#define FONT_H 8

static void write_cmd(uint8_t cmd)
{
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    gpio_set_level(PIN_NUM_DC, 0);
    spi_device_polling_transmit(spi, &t);
}

static void write_data(const uint8_t *data, size_t len)
{
    if (len == 0) return;
    gpio_set_level(PIN_NUM_DC, 1);
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(spi, &t);
}

static void write_u8(uint8_t v)
{
    write_data(&v, 1);
}

static void set_window(int x0, int y0, int w, int h)
{
    int x1 = x0 + w - 1;
    int y1 = y0 + h - 1;
    write_cmd(ST7735_CASET);
    write_u8(0); write_u8((uint8_t)(COLSTART + x0));
    write_u8(0); write_u8((uint8_t)(COLSTART + x1));
    write_cmd(ST7735_RASET);
    write_u8(0); write_u8((uint8_t)(ROWSTART + y0));
    write_u8(0); write_u8((uint8_t)(ROWSTART + y1));
    write_cmd(ST7735_RAMWR);
}

// Fill a rectangle with 16-bit color (RGB565). Uses a small buffer for speed.
static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    set_window(x, y, w, h);
    size_t n = (size_t)w * (size_t)h;
    size_t chunk = (n > 320) ? 320 : n;
    uint16_t *buf = (uint16_t *)heap_caps_malloc(chunk * 2, MALLOC_CAP_DMA);
    if (!buf) {
        uint8_t hi = (uint8_t)(color >> 8), lo = (uint8_t)(color & 0xFF);
        gpio_set_level(PIN_NUM_DC, 1);
        for (size_t i = 0; i < n; i++) {
            spi_transaction_t t = { .length = 16, .tx_data = { hi, lo } };
            spi_device_polling_transmit(spi, &t);
        }
        return;
    }
    for (size_t i = 0; i < chunk; i++) buf[i] = color;
    gpio_set_level(PIN_NUM_DC, 1);
    while (n) {
        size_t send = (n > chunk) ? chunk : n;
        spi_transaction_t t = { .length = send * 16, .tx_buffer = buf };
        spi_device_polling_transmit(spi, &t);
        n -= send;
    }
    heap_caps_free(buf);
}

// Full-screen or large fill using a DMA buffer (multiple chunks if needed).
static void fill_rect_fast(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    set_window(x, y, w, h);
    size_t total = (size_t)w * (size_t)h;
    size_t chunk = (total > 320) ? 320 : total;
    uint16_t *buf = (uint16_t *)heap_caps_malloc(chunk * 2, MALLOC_CAP_DMA);
    if (!buf) {
        fill_rect(x, y, w, h, color);
        return;
    }
    for (size_t i = 0; i < chunk; i++) buf[i] = color;
    gpio_set_level(PIN_NUM_DC, 1);
    while (total) {
        size_t send = (total > chunk) ? chunk : total;
        spi_transaction_t t = { .length = send * 16, .tx_buffer = buf };
        spi_device_polling_transmit(spi, &t);
        total -= send;
    }
    heap_caps_free(buf);
}

bool display_init(void)
{
    if (initialized) return true;

    esp_err_t ret;

    // GPIO: DC, CS, RST, BL
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_NUM_DC) | (1ULL << PIN_NUM_CS) | (1ULL << PIN_NUM_RST) | (1ULL << PIN_NUM_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed %d", ret);
        return false;
    }
    gpio_set_level(PIN_NUM_CS, 1);
    gpio_set_level(PIN_NUM_DC, 0);
    gpio_set_level(PIN_NUM_RST, 1);
#if CONFIG_DISPLAY_ST7735_BACKLIGHT_ACTIVE_LOW
    gpio_set_level(PIN_NUM_BL, 0);
#else
    gpio_set_level(PIN_NUM_BL, 1);
#endif

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_NUM_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 320 * 2,
    };
    ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed %d", ret);
        return false;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = SPI_CLOCK_HZ,
#if CONFIG_DISPLAY_ST7735_SPI_MODE_3
        .mode = 3,
#else
        .mode = 0,
#endif
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 1,
    };
    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed %d", ret);
        spi_bus_free(SPI2_HOST);
        return false;
    }

    // Hardware reset
    gpio_set_level(PIN_NUM_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_NUM_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    // Init sequence for 80x160 (green-tab style with offsets)
    write_cmd(ST7735_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(150));
    write_cmd(ST7735_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(255));

    write_cmd(ST7735_FRMCTR1);
    write_data((uint8_t[]){ 0x01, 0x2C, 0x2D }, 3);
    write_cmd(ST7735_FRMCTR2);
    write_data((uint8_t[]){ 0x01, 0x2C, 0x2D }, 3);
    write_cmd(ST7735_FRMCTR3);
    write_data((uint8_t[]){ 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D }, 6);
    write_cmd(ST7735_INVCTR);
    write_u8(0x07);
    write_cmd(ST7735_PWCTR1);
    write_data((uint8_t[]){ 0xA2, 0x02, 0x84 }, 3);
    write_cmd(ST7735_PWCTR2);
    write_u8(0xC5);
    write_cmd(ST7735_PWCTR3);
    write_data((uint8_t[]){ 0x0A, 0x00 }, 2);
    write_cmd(ST7735_PWCTR4);
    write_data((uint8_t[]){ 0x8A, 0x2A }, 2);
    write_cmd(ST7735_PWCTR5);
    write_data((uint8_t[]){ 0x8A, 0xEE }, 2);
    write_cmd(ST7735_VMCTR1);
    write_u8(0x0E);
    write_cmd(ST7735_GMCTRP1);
    write_data((uint8_t[]){ 0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10 }, 16);
    write_cmd(ST7735_GMCTRN1);
    write_data((uint8_t[]){ 0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10 }, 16);
#if CONFIG_DISPLAY_ST7735_PANEL_OFFSET_0_0
    write_cmd(ST7735_INVOFF);  // No inversion for 0,0 panels
#else
    write_cmd(ST7735_INVON);   // Inversion on for 80x160 (26,1) style
#endif
    write_cmd(ST7735_MADCTL);
    write_u8(0x00);  // (0,0) top-left, row-major so Latin text reads correctly
    write_cmd(ST7735_COLMOD);
    write_u8(0x05);  // 16-bit
    write_cmd(ST7735_CASET);
    write_data((uint8_t[]){ 0x00, COLSTART, 0x00, (uint8_t)(COLSTART + 79) }, 4);
    write_cmd(ST7735_RASET);
    write_data((uint8_t[]){ 0x00, ROWSTART, 0x00, (uint8_t)(ROWSTART + 159) }, 4);
    write_cmd(ST7735_NORON);
    vTaskDelay(pdMS_TO_TICKS(10));
    write_cmd(ST7735_DISPON);
    vTaskDelay(pdMS_TO_TICKS(100));

    initialized = true;

#if CONFIG_DISPLAY_ST7735_RAW_FILL_TEST
    // Raw full-controller fill: 132x162, no offset - verifies SPI/data path
    {
        write_cmd(ST7735_CASET);
        write_data((uint8_t[]){ 0x00, 0x00, 0x00, 131 }, 4);
        write_cmd(ST7735_RASET);
        write_data((uint8_t[]){ 0x00, 0x00, 0x00, 161 }, 4);
        write_cmd(ST7735_RAMWR);
        uint16_t green = 0x07E0;
        size_t total = 132 * 162;
        size_t chunk = (total > 320) ? 320 : total;
        uint16_t *buf = (uint16_t *)heap_caps_malloc(chunk * 2, MALLOC_CAP_DMA);
        if (buf) {
            for (size_t i = 0; i < chunk; i++) buf[i] = green;
            gpio_set_level(PIN_NUM_DC, 1);
            while (total) {
                size_t send = (total > chunk) ? chunk : total;
                spi_transaction_t t = { .length = send * 16, .tx_buffer = buf };
                spi_device_polling_transmit(spi, &t);
                total -= send;
            }
            heap_caps_free(buf);
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
#endif

    // Test: fill red in logical 80x160 window
    fill_rect_fast(0, 0, LCD_WIDTH, LCD_HEIGHT, 0xF800);
    vTaskDelay(pdMS_TO_TICKS(200));
    display_clear();
    ESP_LOGI(TAG, "ST7735 80x160 init OK | pins CS=%d DC=%d RST=%d MOSI=%d SCK=%d BL=%d | offset col=%d row=%d",
             (int)PIN_NUM_CS, (int)PIN_NUM_DC, (int)PIN_NUM_RST, (int)PIN_NUM_MOSI, (int)PIN_NUM_SCK, (int)PIN_NUM_BL,
             (int)COLSTART, (int)ROWSTART);
    return true;
}

void display_clear(void)
{
    if (!initialized) return;
    fill_rect_fast(0, 0, LCD_WIDTH, LCD_HEIGHT, 0x0000);
}

void display_draw_text(int x, int y, const char *str)
{
    if (!initialized || !str) return;

    // One-time log: ASCII only (first bytes in hex)
    static bool first_draw = true;
    if (first_draw) {
        first_draw = false;
        ESP_LOGI(TAG, "first draw (ASCII): \"%s\" (hex %02X %02X %02X %02X %02X %02X %02X %02X ...)",
                 str,
                 (unsigned)(uint8_t)str[0], (unsigned)(uint8_t)str[1], (unsigned)(uint8_t)str[2],
                 (unsigned)(uint8_t)str[3], (unsigned)(uint8_t)str[4], (unsigned)(uint8_t)str[5],
                 (unsigned)(uint8_t)str[6], (unsigned)(uint8_t)str[7]);
    }

    const uint16_t fg = 0xFFFF;
    const uint16_t bg = 0x0000;

#if CONFIG_DISPLAY_ST7735_TEXT_ROTATE_90
    // Rotated 90° CW: 6x8 glyph -> 8 wide x 6 tall. ASCII only.
    #define ROT_W 8
    #define ROT_H 6
    uint16_t buf[ROT_W * ROT_H];
    while (*str) {
        uint8_t ch = (uint8_t)*str++;
        if (ch < 0x20 || ch > 0x7F) ch = 0x20;
        const uint8_t *glyph = font6x8[ch - 0x20];
        int px = x, py = y;
        if (px + ROT_W <= 0 || px >= LCD_WIDTH || py + ROT_H <= 0 || py >= LCD_HEIGHT) {
            x += ROT_W;
            if (x >= LCD_WIDTH) break;
            continue;
        }
        for (int oy = 0; oy < ROT_H; oy++)
            for (int ox = 0; ox < ROT_W; ox++)
                buf[oy * ROT_W + ox] = ((glyph[oy] >> ox) & 1) ? fg : bg;
        set_window(px, py, ROT_W, ROT_H);
        gpio_set_level(PIN_NUM_DC, 1);
        spi_transaction_t t = { .length = ROT_W * ROT_H * 16, .tx_buffer = buf };
        spi_device_polling_transmit(spi, &t);
        x += ROT_W;
        if (x >= LCD_WIDTH) break;
    }
    #undef ROT_W
    #undef ROT_H
#else
    uint16_t buf[FONT_W * FONT_H];
    while (*str) {
        uint8_t c = (uint8_t)*str++;
        if (c < 0x20 || c > 0x7F) c = 0x20;
        const uint8_t *glyph = font6x8[c - 0x20];
        int px = x, py = y;
        if (px + FONT_W <= 0 || px >= LCD_WIDTH || py + FONT_H <= 0 || py >= LCD_HEIGHT) {
            x += FONT_W;
            if (x >= LCD_WIDTH) break;
            continue;
        }
        // ASCII only. Font LSB=top. Row-major; mirror glyph (glyph[5-col]) for panel.
        for (int row = 0; row < FONT_H; row++) {
            for (int col = 0; col < FONT_W; col++) {
                uint16_t color = ((glyph[5 - col] >> row) & 1) ? fg : bg;
                buf[row * FONT_W + col] = (uint16_t)((color << 8) | (color >> 8));
            }
        }
        set_window(px, py, FONT_W, FONT_H);
        gpio_set_level(PIN_NUM_DC, 1);
        spi_transaction_t t = { .length = FONT_W * FONT_H * 16, .tx_buffer = buf };
        spi_device_polling_transmit(spi, &t);
        x += FONT_W;
        if (x >= LCD_WIDTH) break;
    }
#endif
}

void display_set_backlight(bool on)
{
#if CONFIG_DISPLAY_ST7735_BACKLIGHT_ACTIVE_LOW
    gpio_set_level(PIN_NUM_BL, on ? 0 : 1);
#else
    gpio_set_level(PIN_NUM_BL, on ? 1 : 0);
#endif
}
