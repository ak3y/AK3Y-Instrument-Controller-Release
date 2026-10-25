# AK3Y Instrument Controller

Firmware release **0.90(a)** for the ESP32-DevKitC / ESP32-WROOM-32E-based IEEE-488 (GPIB), RS-232 and TCP instrument controller.

This is the **clean public release** project, separate from the development repository. The source code is provided for advanced users, but **you do not need to compile it** to install the firmware.

## Initial installation on a new ESP32

**Caution:** Initial programming overwrites existing firmware. Confirm that your hardware is compatible with this project. Back up any existing configuration before proceeding. The complete hardware design and microSD setup are documented in the User's Manual.

1. Download `firmware/AK3Y-Controller-v0.90a-factory.bin` from the release assets or this repository.
2. Install Espressif's `esptool` on your computer (`python3 -m pip install esptool`).
3. Connect the ESP32 board to your Mac/PC with a USB data cable. Determine its serial port (for example `/dev/cu.usbserial-0001` on macOS).
4. Run the following command, replacing `PORT` with your actual serial port:

   ```sh
   python3 -m esptool --chip esp32 --port PORT --baud 460800 write_flash 0x0 AK3Y-Controller-v0.90a-factory.bin
   ```

5. Restart the ESP32 and follow the User's Manual to prepare the FAT32 microSD card and configure Wi-Fi.

**Factory-image contents:** bootloader at `0x1000`, partition table at `0x8000`, and application at `0x10000`. Unused bytes between these components are `0xFF`. The image does not contain personal Wi-Fi settings, instrument profiles, or microSD data. The OTA selection-data partition at `0xE000` remains erased (`0xFF`) for a fresh device. This image is intended for **initial programming of a blank or fully erased compatible ESP32**. For a previously used ESP32, erase flash first only after backing up any needed data (`python3 -m esptool --chip esp32 --port PORT erase_flash`).

**Validation status:** binaries were extracted from the supplied PlatformIO build and the merged image was checked for correct byte placement; **hardware flashing and boot operation have not yet been tested**. Please verify on a spare or newly assembled controller before publication.

## Updating an existing controller

Use the controller's **Firmware Update** web page with `firmware/AK3Y-Controller-v0.90a-ota.bin` (the application image only). **Do not upload the factory image through the browser.**

## Source build

Open this project in Visual Studio Code with PlatformIO and build environment `esp32dev`. Project settings are in `platformio.ini`. The compiled application is ordinarily written to `.pio/build/esp32dev/firmware.bin`.

## Documentation and examples

Consult the User's Manual for wiring, SD card preparation, instrument profiles, macros, workspace management, and troubleshooting. Example configuration files will be added after checking their exact release versions.

## Release integrity

See `firmware/SHA256SUMS.txt` for SHA-256 hashes of all distributed binaries.
