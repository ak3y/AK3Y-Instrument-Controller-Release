# AK3Y Instrument Controller — Firmware v0.90(b)

## Firmware Images

This directory contains two firmware images for the AK3Y Instrument Controller.

**AK3Y-Controller-v0.90b-factory.bin**

Complete factory image for initial programming of an ESP32. Includes the bootloader, partition table, and application firmware.

Program this image at flash address `0x00000000`.

**AK3Y-Controller-v0.90b-ota.bin**

Application firmware image for updating an existing controller through its web interface.

Use the **Firmware Update** page in the controller's web interface. Do not program this image at address `0x00000000`.

## Initial Installation

Connect the ESP32 controller to a computer using a USB data cable.

Install esptool if necessary:

```bash
python3 -m pip install esptool
```

Identify the serial port assigned to the ESP32.

For example, on macOS:

```bash
ls /dev/cu.usb*
```

To erase the ESP32 flash completely:

```bash
python3 -m esptool --chip esp32 --port PORT erase_flash
```

Replace `PORT` with the appropriate serial port.

**Warning:** Erasing flash removes existing Wi-Fi credentials and other settings stored in ESP32 nonvolatile storage (NVS).

Program the factory image:

```bash
python3 -m esptool --chip esp32 --port PORT --baud 460800 write_flash 0x0 AK3Y-Controller-v0.90b-factory.bin
```

Wait for the message:

`Hash of data verified.`

Disconnect USB and install the programmed ESP32 in the controller hardware.

## First-Time Wi-Fi Configuration

When no valid Wi-Fi credentials are stored in NVS, firmware v0.90(b) starts a Wi-Fi access point:

**SSID:** `AK3Y-Controller-Setup`

Connect a computer, tablet, or smartphone to this network.

Open a web browser and navigate to:

`http://192.168.4.1/`

Select the desired Wi-Fi network, enter its password, and save the configuration.

The controller will restart and connect to the selected network.

The assigned IP address is displayed on the controller's OLED screen.

Wi-Fi credentials are stored in ESP32 NVS. They are not retrieved from the microSD card.

On subsequent power cycles, the controller automatically reconnects using the saved credentials.

## Updating Existing Controllers

For controllers already running compatible firmware:

1. Open the controller's web interface.
2. Select **Firmware Update**.
3. Select `AK3Y-Controller-v0.90b-ota.bin`.
4. Start the firmware update.
5. Allow the controller to restart.

An OTA update normally preserves Wi-Fi credentials and other NVS settings.

Do not disconnect power while the firmware update is in progress.

## File Verification

The file `SHA256SUMS.txt` contains SHA-256 checksums for the distributed firmware images.

On macOS, verify an image using:

```bash
shasum -a 256 AK3Y-Controller-v0.90b-factory.bin
```

Compare the result with the corresponding entry in `SHA256SUMS.txt`.

Repeat for the OTA image if desired.

## Compatibility

Firmware v0.90(b) is intended for the ESP32-based AK3Y Instrument Controller hardware using the project's PlatformIO `esp32dev` configuration.

The factory image was successfully programmed and tested on an ESP32-D0WD-V3 (revision 3.1) with 4 MB flash.

The factory installation, first-time Wi-Fi provisioning, microSD initialization, and web interface startup were verified on the assembled controller.

## Release Notes — v0.90(b)

- Removed legacy Wi-Fi credential loading from the microSD card.
- Wi-Fi credentials are stored and retrieved using ESP32 NVS.
- New installations start the `AK3Y-Controller-Setup` provisioning access point.
- Confirmed factory image programming and first-time provisioning.
- Confirmed operation of the controller web interface with existing instrument profiles.