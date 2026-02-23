@echo off
REM Build and optionally flash bt2usb for Pocket-Dongle-S3.
REM Run this from "ESP-IDF 5.5 CMD" / "ESP-IDF 6.0 CMD" (or after running export.bat from your ESP-IDF install).

set SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.board.pocket_dongle_s3

if "%1"=="flash" (
    idf.py build flash
) else if "%1"=="monitor" (
    idf.py monitor
) else (
    idf.py build
    echo.
    echo To flash:  build_pocket_dongle_s3.bat flash
    echo To monitor: build_pocket_dongle_s3.bat monitor
)
