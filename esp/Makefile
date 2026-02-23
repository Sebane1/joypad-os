# Joypad ESP32-S3 bt2usb Build System
# Board selection via BOARD variable (default: devkit)

.PHONY: init build flash flash-wipe monitor clean menuconfig help uf2

# Default board
BOARD ?= devkit

# Auto-source ESP-IDF environment
IDF_ENV := . ./env.sh >/dev/null 2>&1 &&

# Board configurations
COMMON_SDKCONFIG := sdkconfig.defaults

SDKCONFIG_BOARD := sdkconfig.board.$(BOARD)
SDKCONFIG_FILES := $(COMMON_SDKCONFIG)
ifneq ($(wildcard $(SDKCONFIG_BOARD)),)
    SDKCONFIG_FILES := $(COMMON_SDKCONFIG);$(SDKCONFIG_BOARD)
endif

init:
	@if [ ! -d "$(HOME)/esp-idf" ]; then \
		echo "Cloning ESP-IDF v6.0..."; \
		git clone --branch v6.0 --depth 1 --recursive https://github.com/espressif/esp-idf.git $(HOME)/esp-idf; \
	else \
		echo "ESP-IDF already installed at ~/esp-idf"; \
	fi
	@cd $(HOME)/esp-idf && ./install.sh esp32s3
	@echo "ESP-IDF setup complete. Run 'source env.sh' to activate."

build:
	@echo "Building bt2usb for ESP32-S3 ($(BOARD))"
	$(IDF_ENV) SDKCONFIG_DEFAULTS="$(SDKCONFIG_FILES)" idf.py build

# --- UF2 / Flash variables ---

UF2CONV := tools/uf2conv.py
UF2_FAMILY := 0xc47e5767
OTA_DATA := tools/ota_data_ota0.bin
BUILD_APP := build/joypad_bt2usb.bin
BUILD_PARTITION := build/partition_table/partition-table.bin

# Flash app to ota_0 with otadata pointing to ota_0.
# Assumes TinyUF2 bootloader release is already flashed (one-time setup).
# Only writes partition table, otadata, and app — does not touch bootloader
# or factory partition.
flash: build
	@echo "Flashing bt2usb to ESP32-S3 ($(BOARD))"
	$(IDF_ENV) esptool.py --chip esp32s3 write_flash \
		--flash_mode dio --flash_freq 80m --flash_size 8MB \
		0x8000 $(BUILD_PARTITION) \
		0xe000 $(OTA_DATA) \
		0x10000 $(BUILD_APP)

# Wipe all flash (including NVS config) then flash. Use when stuck in XInput (no serial).
# Example: make flash-wipe PORT=COM78
flash-wipe:
	@echo "Erasing full flash (wipes saved config: USB mode, bonds, etc.)..."
	$(IDF_ENV) idf.py $(if $(PORT),-p $(PORT),) erase-flash
	@echo "Flashing firmware..."
	$(IDF_ENV) SDKCONFIG_DEFAULTS="$(SDKCONFIG_FILES)" idf.py $(if $(PORT),-p $(PORT),) flash

monitor:
	@echo "Opening serial monitor"
	@echo "NOTE: Use Ctrl+] to exit"
	$(IDF_ENV) idf.py monitor

clean:
	$(IDF_ENV) idf.py fullclean

menuconfig:
	$(IDF_ENV) SDKCONFIG_DEFAULTS="$(SDKCONFIG_FILES)" idf.py menuconfig

# Generate .uf2 update file from built app binary.
# Base address 0 = offset within ota_0 partition (TinyUF2 convention).
uf2: build
	@echo "Generating UF2 file..."
	python3 $(UF2CONV) $(BUILD_APP) \
		--base 0 \
		--family $(UF2_FAMILY) \
		--output build/joypad_bt2usb.uf2 \
		--convert
	@echo "UF2 file: build/joypad_bt2usb.uf2"

# Board shortcuts
devkit:
	@$(MAKE) BOARD=devkit build

pocket_dongle_s3:
	@$(MAKE) BOARD=pocket_dongle_s3 build

# Help
help:
	@echo "Joypad ESP32-S3 Build Commands:"
	@echo ""
	@echo "  make [BOARD=devkit]  - Build for specified board (default: devkit)"
	@echo "  make devkit          - Build for ESP32-S3 DevKit"
	@echo "  make pocket_dongle_s3 - Build for Pocket-Dongle-S3 / T-Dongle S3 (16MB flash)"
	@echo ""
	@echo "  make flash [BOARD=...] - Flash app via esptool (requires TinyUF2 bootloader)"
	@echo "  make flash-wipe [PORT=COMx] - Erase full flash then flash (resets USB mode/config)"
	@echo "  make monitor           - Monitor serial output"
	@echo "  make uf2               - Build + generate .uf2 for drag-and-drop update"
	@echo "  make clean             - Clean build files"
	@echo "  make menuconfig        - Open ESP-IDF config menu"
	@echo ""
	@echo "First-time setup:"
	@echo "  1. Flash TinyUF2 release for your board (from github.com/adafruit/tinyuf2)"
	@echo "  2. Run: make flash"
	@echo ""
	@echo "Firmware updates:"
	@echo "  - Drag-and-drop: make uf2, copy .uf2 to TinyUF2 USB drive"
	@echo "  - Esptool: make flash"
	@echo ""
	@echo "Add new boards:"
	@echo "  1. Create sdkconfig.board.<name> with board-specific overrides"
	@echo "  2. Run: make BOARD=<name>"
	@echo ""
	@echo "Setup:"
	@echo "  make init            - Install ESP-IDF v6.0 and tools"
	@echo ""
