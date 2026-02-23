# ESP32-S3 Support

Joypad OS supports ESP32-S3 as a build target for the **BT2USB** app. The ESP32-S3 has both BLE and USB OTG, making it a compact standalone BLE-to-USB HID gamepad adapter.

## Overview

| Feature | Pico W / Pico 2 W | ESP32-S3 |
|---|---|---|
| Bluetooth | Classic BT + BLE | BLE only |
| USB Output | USB Device (RP2040) | USB OTG Device |
| Controllers | All BT controllers | BLE controllers only |
| Build System | pico-sdk / CMake | ESP-IDF / CMake |
| Firmware Update | Drag-and-drop (.uf2) | Drag-and-drop (.uf2) via TinyUF2 |

**BLE-only limitation:** ESP32-S3 only supports BLE controllers. Classic Bluetooth controllers (like DualShock 3 in BT mode) will not work. Most modern controllers support BLE.

**WiFi (JOCP):** The ESP32-S3 bt2usb firmware also accepts controller input over WiFi using the same JOCP protocol as the Pico W wifi2usb app. The dongle runs a WiFi access point (SSID `JOYPAD-XXXX`, password derived from MAC); connect your phone or a JOCP-capable controller app and send input to the dongle’s IP (192.168.4.1) on UDP port 30100. **Click** the BOOT button to make the SSID visible for 30 seconds (pairing mode). **Hold** the button to restart the WiFi AP. BLE and WiFi inputs are merged to the same USB output.

### Supported Controllers (BLE)

| Controller | Status |
|---|---|
| Xbox One / Series (BLE mode) | Supported |
| 8BitDo controllers (BLE mode) | Supported |
| Switch 2 Pro (BLE) | Supported |
| Generic BLE HID gamepads | Supported |

### Not Supported (Classic BT only)

| Controller | Why |
|---|---|
| DualShock 3 | Classic BT only |
| DualShock 4 | Classic BT only |
| DualSense | Classic BT only |
| Switch Pro | Classic BT only |
| Wii U Pro | Classic BT only |
| Wiimote | Classic BT only |

These controllers pair over Classic BT and require the Pico W build.

## Hardware

### Tested Boards

| Board | Flash | Status | Notes |
|---|---|---|---|
| Seeed XIAO ESP32-S3 | 8MB | Tested | User LED on GPIO 21 (active low) |
| ESP32-S3-DevKitC | Varies | Should work | Untested |
| **Pocket-Dongle-S3 / T-Dongle S3** (N16R8) | 16MB | Supported | 0.96" ST7735, 8MB PSRAM, Boot on GPIO 0. Use `BOARD=pocket_dongle_s3` or `make bt2usb_esp32s3_pocket_dongle_s3`. |

Any ESP32-S3 board with USB OTG should work. Board-specific pin configurations (LED GPIO, button GPIO) can be overridden via sdkconfig.

### Why ESP32-S3?

ESP32-S3 is the only ESP32 variant with both BLE and USB OTG:

| Chip | BLE | USB OTG | bt2usb? |
|---|---|---|---|
| ESP32 | Yes (+ Classic) | No | No |
| ESP32-S2 | No | Yes | No |
| **ESP32-S3** | **Yes** | **Yes** | **Yes** |
| ESP32-C3/C6/H2 | Yes | No | No |

## Firmware Updates (TinyUF2)

The ESP32-S3 build uses [Adafruit TinyUF2](https://github.com/adafruit/tinyuf2) as a bootloader, enabling the same drag-and-drop `.uf2` firmware updates as RP2040 boards.

### How It Works

TinyUF2 lives in a factory partition on flash. When activated, it presents a USB mass storage drive. Drop a `.uf2` file on the drive and the firmware updates automatically.

```
8MB Flash Layout
────────────────────────────────────────
0x000000  Bootloader (TinyUF2's custom 2nd-stage)
0x008000  Partition table
0x009000  NVS (20KB)
0x00E000  OTA data (8KB) — tells bootloader which app to run
0x010000  ota_0 (2MB) — Joypad firmware lives here
0x210000  ota_1 (2MB) — unused
0x410000  factory (256KB) — TinyUF2 mass storage app
0x450000  ffat (3.7MB) — TinyUF2 virtual FAT filesystem
```

Normal boot: bootloader reads OTA data, boots ota_0 (Joypad app).
TinyUF2 mode: bootloader boots factory partition (TinyUF2), USB drive appears.
Recovery: if ota_0 is corrupted/empty, bootloader falls back to factory (TinyUF2) automatically.

### First-Time Setup

Flash the official TinyUF2 release for your board once. This installs the bootloader, partition table, and TinyUF2:

1. Download the TinyUF2 release for your board from [Adafruit TinyUF2 releases](https://github.com/adafruit/tinyuf2/releases)
   - For XIAO ESP32-S3: `tinyuf2-seeed_xiao_esp32s3-X.XX.X.zip`
2. Extract the zip and flash `combined.bin`:
   ```bash
   esptool.py --chip esp32s3 -b 921600 write_flash 0x0 combined.bin
   ```
3. The device reboots into TinyUF2 — a USB drive named `XIAOS3BOOT` appears
4. Drop a `.uf2` firmware file onto the drive (or run `make flash-uf2-bt2usb_esp32s3`)

After this one-time setup, all future updates are drag-and-drop.

### Entering TinyUF2 Mode

Three ways to enter TinyUF2 from a running app:

| Method | How |
|---|---|
| Double-tap reset | Press the reset button twice within 500ms |
| CDC command | Send `BOOTSEL` over the USB serial port |
| Recovery | If the app is corrupted, TinyUF2 boots automatically |

### Updating Firmware

**Drag-and-drop (recommended for end users):**

1. Enter TinyUF2 mode (double-tap reset or send `BOOTSEL`)
2. A USB drive appears (`XIAOS3BOOT` on XIAO)
3. Copy the `.uf2` file to the drive
4. Device reboots into the new firmware

**Via make (recommended for development):**

```bash
# Build + flash via TinyUF2 USB drive (device must be in TinyUF2 mode)
make flash-uf2-bt2usb_esp32s3

# Build + flash via esptool USB serial (device must be in download mode)
make flash-bt2usb_esp32s3
```

**Generating .uf2 files:**

```bash
make uf2-bt2usb_esp32s3
# Output: releases/joypad_<version>_bt2usb_esp32s3.uf2
```

## Build & Flash

### Prerequisites

**Option 1 (Unix):** From repo root, `make init-esp` clones ESP-IDF v6.0 to `~/esp-idf` and installs the ESP32-S3 toolchain. If you already have ESP-IDF installed, it will skip the clone and just install tools.

**Option 2 (manual):** Install ESP-IDF (v5.5.x or v6.x). Releases: [ESP-IDF GitHub Releases](https://github.com/espressif/esp-idf/releases) (e.g. [v5.5.3](https://github.com/espressif/esp-idf/releases/tag/v5.5.3)). Clone with `git clone -b v5.5.3 --recursive https://github.com/espressif/esp-idf.git`, then run `install.sh esp32s3` and use the environment’s `export.sh` / `export.bat`. On Windows with the official installer, open **"ESP-IDF 5.5 CMD"** or **"ESP-IDF 6.0 CMD"** so that `idf.py` is in your PATH.

### Build Commands

From the repo root:

```bash
# Default board: devkit
make bt2usb_esp32s3                 # Build
make uf2-bt2usb_esp32s3             # Build + generate .uf2
make flash-uf2-bt2usb_esp32s3       # Build + flash .uf2 via TinyUF2 drive
make flash-bt2usb_esp32s3           # Build + flash via esptool
make monitor-bt2usb_esp32s3         # UART serial monitor (Ctrl+] to exit)
```

**Pocket-Dongle-S3 / LilyGo T-Dongle S3** (0.96" display, 16MB Flash, N16R8):

From a shell where ESP-IDF is loaded (e.g. Git Bash after `source ~/esp-idf/export.sh`, or WSL):

```bash
make bt2usb_esp32s3_pocket_dongle_s3     # Build for Pocket-Dongle-S3
make flash-bt2usb_esp32s3_pocket_dongle_s3
make monitor-bt2usb_esp32s3_pocket_dongle_s3
# Or with BOARD= for any target:
make bt2usb_esp32s3 BOARD=pocket_dongle_s3
make flash-bt2usb_esp32s3 BOARD=pocket_dongle_s3
```

**Windows (ESP-IDF CMD):** Open **"ESP-IDF 5.5 CMD"** or **"ESP-IDF 6.0 CMD"** (or run `export.bat` from your ESP-IDF install), then:

```cmd
cd path\to\joypad-os\esp
build_pocket_dongle_s3.bat
```
Then plug the dongle and run `build_pocket_dongle_s3.bat flash`. For serial monitor: `build_pocket_dongle_s3.bat monitor`.

Or from the `esp/` directory:

```bash
cd esp
make init                          # Install ESP-IDF (if not done from root)
source env.sh                      # Activate ESP-IDF environment
make build                         # default BOARD=devkit
make pocket_dongle_s3              # build for Pocket-Dongle-S3
make uf2                           # Build + generate .uf2
make flash [BOARD=pocket_dongle_s3]
make monitor
```

### Board Configurations

Board-specific sdkconfig overrides go in `esp/sdkconfig.board.<name>`:

```bash
echo 'CONFIG_SOME_OPTION=y' > esp/sdkconfig.board.myboard
cd esp && make BOARD=myboard build
```

## Usage

### Pairing

1. Flash the firmware
2. The adapter starts scanning for BLE devices automatically on boot
3. Put your controller into BLE pairing mode
4. Once paired, the controller reconnects automatically on subsequent use

### Button Controls

| Action | Function |
|---|---|
| Click | Start 60-second BLE scan |
| Double-click | Cycle USB output mode |
| Triple-click | Reset to default HID mode |
| Hold | Disconnect all devices and clear bonds |

### Status: LCD vs LED

**Pocket-Dongle-S3 / T-Dongle S3** (and other boards with the 0.96" ST7735) have **no status LED** — they use the **LCD screen** only. The display shows "Joypad OS", the current **USB output mode** (e.g. HID, XInput), and a **status line**: **"Scanning..."** (animated dots) when looking for controllers, **"1 controller"** / **"2 controllers"** when connected, or **"Ready"** when idle. Put your controller in BLE pairing mode while the screen shows "Scanning..." to pair.

**Other boards** (e.g. XIAO ESP32-S3, DevKit) may have an LED: blinking = scanning/idle, solid = device connected.

### USB Output Modes

Double-click the button to cycle through output modes: XInput, DInput, Switch, PS3, PS4/PS5.

### Reset config when stuck (e.g. XInput, no serial)

In XInput (and some other USB modes) the device does not expose a CDC serial port, so you cannot change settings or send the `BOOTLOADER` command over USB. To get back to a configurable state:

1. **Erase full flash then reflash** (wipes NVS: USB mode, bonds, all settings):
   - **From esp/ (Unix):** `make flash-wipe PORT=COM78` (use your port).
   - **Windows / any:**  
     `idf.py -p COM78 erase-flash`  
     then  
     `idf.py -p COM78 flash`

2. After flashing, the device starts with default config (e.g. HID mode with serial), so you can use the serial monitor and the button again.

**Mode switch:** USB output mode is changed by **double-clicking** the BOOT button (two quick presses within ~450 ms). Single click starts BLE scan; double-click cycles XInput → DInput → Switch → PS3 → PS4/PS5.

**Button not responding?** The LCD shows a debug line `GPxx: H` or `GPxx: L` (e.g. `GP00: H`). **H** = pin high (released), **L** = pin low (pressed). If pressing the button never changes it to **L**, the button is likely on a different GPIO. Run `idf.py menuconfig` → **Component config** → **Button** → set **User button GPIO** to the correct pin (e.g. 1 or 9 on some boards), then rebuild and reflash.

### Viewing logs in config.joypad.ai

[config.joypad.ai](https://config.joypad.ai/) talks to the device over the CDC serial interface. The **Log** section shows debug output (e.g. `[button] Event: CLICK`) only when **Debug Stream** is enabled:

1. Connect the device in the web app (Connect → choose the Joypad serial port).
2. Open the **Log** section (bottom of the page).
3. Click **Start Stream** (or enable **Debug Stream**) so the device starts sending log events.
4. Button presses and other `printf` output will then appear in the log.

The device must be in a USB mode that exposes CDC (e.g. **SInput** or **HID**). In **XInput** mode the device often does not expose a serial port, so the config tool cannot connect.

### CDC Serial Commands

Connect to the USB serial port for device management:

| Command | Function |
|---|---|
| `VERSION` | Show firmware version |
| `DEVICE` | Show connected controller info |
| `BOOTSEL` | Reboot into TinyUF2 mode |
| `HELP` | List all commands |

## Architecture

### Threading Model

ESP32-S3 uses FreeRTOS with two tasks:

```
Main Task                    BTstack Task
  USB device polling           BLE scanning/pairing
  LED status updates           HID report processing
  Button input                 Controller data -> router
  Storage persistence
```

BTstack runs in its own FreeRTOS task. All BLE operations (scanning, pairing, data) happen in that task. The main task handles USB output, LED, and button input.

### Shared Code

The ESP32-S3 build compiles the same shared source files as the Pico W build:

- Core services (router, players, profiles, hotkeys, storage, LEDs)
- All USB device output modes (HID, XInput, PS3, PS4, Switch, etc.)
- All BT HID drivers (vendor-specific and generic)
- BTstack host integration
- CDC serial command interface

Platform-specific code is abstracted through `src/platform/platform.h`.

### ESP32-Specific Files

| Location | Purpose |
|---|---|
| `esp/main/main.c` | FreeRTOS entry point |
| `esp/main/flash_esp32.c` | NVS-based settings storage |
| `esp/main/button_esp32.c` | GPIO button driver |
| `esp/main/btstack_config.h` | BLE-only BTstack configuration |
| `esp/main/tusb_config_esp32.h` | TinyUSB device configuration |
| `esp/partitions_uf2.csv` | Partition table (matches TinyUF2 layout) |
| `esp/tools/ota_data_ota0.bin` | Pre-built OTA data (boots ota_0) |
| `esp/tools/uf2conv.py` | UF2 file conversion tool |
| `src/platform/esp32/platform_esp32.c` | Platform HAL (time, reboot, double-tap reset) |
| `src/bt/transport/bt_transport_esp32.c` | BLE transport layer |
