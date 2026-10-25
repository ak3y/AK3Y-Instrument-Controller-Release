/******************************************************************************
 * File: SDCard.h
 * Project: AK3Y Instrument Controller
 * Purpose: MicroSD storage service and diagnostic access.
 * Responsibilities: Initializes the SPI-connected SD card, exposes file access, and performs a non-destructive read/write diagnostic.
 * Dependencies: Arduino SD/FS, SPI, Console.
 * Notes: Documentation only; interface behavior is defined by the code below.
 ******************************************************************************/

#pragma once
#include <stdint.h>
#include <FS.h>

namespace AK3Y {
class Console;

class SDCard {
public:
    explicit SDCard(uint8_t chipSelectPin);

    bool initialize();
    bool ensureDirectories();
    bool runDiagnostics(Console& console);

    bool isInitialized() const;
    File open(const char* path, const char* mode = FILE_READ);

private:
    uint8_t m_chipSelectPin;
    bool m_initialized;
};
}
