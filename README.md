# AK3Y Instrument Controller

Firmware release **0.90(b)** for the ESP32-DevKitC / ESP32-WROOM-32E-based IEEE-488 (GPIB), RS-232 and TCP instrument controller.

This is the **release project**, separate from the development repository. The source code is provided for advanced users, but **you do not need to compile it** to install the firmware.

## Initial installation on a new ESP32

**Caution:** Initial programming overwrites existing firmware. Confirm that your hardware is compatible with this project. Back up any existing configuration before proceeding. The complete hardware design and microSD setup are documented in the User's Manual.

1. Download `firmware/AK3Y-Controller-v0.90b-factory.bin` from the release assets or this repository.
2. Install Espressif's `esptool` on your computer (for example, `python3 -m pip install esptool`; use a Python virtual environment if your system restricts package installation).
3. Connect the ESP32 board to your Mac/PC with a USB data cable. Determine its serial port (for example `/dev/cu.usbserial-310` on macOS).
4. From the directory containing the downloaded factory image, run the following command, replacing `PORT` with your actual serial port:

   ```sh
   python3 -m esptool --chip esp32 --port PORT --baud 460800 write_flash 0x0 AK3Y-Controller-v0.90b-factory.bin
   ```

5. After programming completes and the flash hash is verified, install the ESP32 in the controller hardware and power it on. Prepare the FAT32 microSD card as described in the User's Manual.

**Previously used ESP32:** For a clean initial installation, erase flash first, **only after backing up any needed settings or data**:

```sh
python3 -m esptool --chip esp32 --port PORT erase_flash
```

Erasing flash removes saved Wi-Fi credentials and other ESP32 nonvolatile settings.

### First-time Wi-Fi setup

If no valid Wi-Fi credentials are stored in ESP32 NVS, the controller starts a setup access point:

- **Wi-Fi network (SSID):** `AK3Y-Controller-Setup`
- **Setup page:** `http://192.168.4.1/`

Connect a computer or phone to that network, open the setup page, and enter the credentials for your normal Wi-Fi network. The controller will connect to that network and display its assigned IP address on the OLED.

Beginning with **0.90(b)**, Wi-Fi credentials are stored in ESP32 NVS; the firmware no longer loads legacy Wi-Fi credentials from the microSD card. Saved credentials are intended to be reused on subsequent power cycles.

**Factory-image contents:** bootloader at `0x1000`, partition table at `0x8000`, and application at `0x10000`. Unused bytes between these components are `0xFF`. The image does not contain personal Wi-Fi settings, instrument profiles, or microSD data. The OTA selection-data partition at `0xE000` remains erased (`0xFF`) in this factory image. The image is intended for initial programming of a blank or fully erased compatible ESP32.

**Validation status:** The **0.90(b) factory image** was programmed to a fully erased ESP32-D0WD-V3 (revision 3.1, 4 MB flash), with successful flash-hash verification. Installed in the assembled controller, it started the setup AP at `192.168.4.1`; Wi-Fi provisioning then successfully connected the controller to the local network. The controller's web interface and microSD-based instrument profiles were observed operating after setup.

## Updating an existing controller

Use the controller's **Firmware Update** web page with `firmware/AK3Y-Controller-v0.90b-ota.bin` (the application image only). **Do not upload the factory image through the browser.** Keep power connected throughout the update. An application-only update normally preserves NVS settings; this OTA image has not yet been independently verified through the browser update workflow.

## Source build

Open this project in Visual Studio Code with PlatformIO and build environment `esp32dev`. Project settings are in `platformio.ini`. The compiled application is ordinarily written to `.pio/build/esp32dev/firmware.bin`. The release repository was compiled successfully with firmware version 0.90(b).

## Documentation and examples

Consult the User's Manual for wiring, SD card preparation, instrument profiles, macros, workspace management, and troubleshooting. Example configuration files will be added after checking their exact release versions.

## Release integrity

See `firmware/SHA256SUMS.txt` for SHA-256 hashes of the distributed binaries. On macOS, calculate a file's SHA-256 hash with `shasum -a 256 FILENAME` and compare it with the corresponding entry.
