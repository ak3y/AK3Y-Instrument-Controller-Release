/******************************************************************************
 * File: SDCard.cpp
 * Project: AK3Y Instrument Controller
 * Purpose: MicroSD storage service and diagnostic access.
 * Responsibilities: Initializes the SPI-connected SD card, exposes file access, and performs a non-destructive read/write diagnostic.
 * Dependencies: Arduino SD/FS, SPI, Console.
 * Notes: Documentation only; interface behavior is defined by the code below.
 ******************************************************************************/

#include "SDCard.h"
#include "Console.h"
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

namespace AK3Y {
namespace {
constexpr char DiagnosticFileName[] = "/.ak3y_sd_diagnostic.tmp";
constexpr char DiagnosticPattern[] = "AK3Y SD diagnostic 2026\n";
}

SDCard::SDCard(uint8_t chipSelectPin)
    : m_chipSelectPin(chipSelectPin), m_initialized(false) {}

bool SDCard::initialize()
{
    if (!SD.begin(m_chipSelectPin, SPI)) {
        m_initialized = false;
        return false;
    }

    if (SD.cardType() == CARD_NONE) {
        SD.end();
        m_initialized = false;
        return false;
    }

    m_initialized = true;
    if (!ensureDirectories()) {
        SD.end();
        m_initialized = false;
        return false;
    }
    return true;
}

bool SDCard::ensureDirectories()
{
    if (!m_initialized) return false;
    constexpr const char* directories[] = {
        "/config", "/instruments", "/macros", "/logs"
    };
    for (const char* path : directories) {
        if (!SD.exists(path) && !SD.mkdir(path)) return false;
        File directory = SD.open(path, FILE_READ);
        const bool valid = directory && directory.isDirectory();
        if (directory) directory.close();
        if (!valid) return false;
    }
    return true;
}

bool SDCard::runDiagnostics(Console& console)
{
    if (!m_initialized) {
        console.printDiagnostic("SD Card", false, 0);
        return false;
    }

    const uint32_t start = millis();
    bool passed = false;
    const char* failure = nullptr;

    // Do not overwrite an existing file left by a previous interrupted test.
    if (SD.exists(DiagnosticFileName)) {
        failure = "Diagnostic temporary file already exists; not overwriting it.";
    } else {
        File file = SD.open(DiagnosticFileName, FILE_WRITE);
        if (!file) {
            failure = "Unable to create diagnostic temporary file.";
        } else {
            const size_t expected = sizeof(DiagnosticPattern) - 1;
            const size_t written = file.write(
                reinterpret_cast<const uint8_t*>(DiagnosticPattern), expected);
            file.flush();
            file.close();

            if (written != expected) {
                failure = "Unable to write complete diagnostic pattern.";
            } else {
                file = SD.open(DiagnosticFileName, FILE_READ);
                if (!file) {
                    failure = "Unable to reopen diagnostic temporary file.";
                } else {
                    passed = file.size() == expected;
                    for (size_t i = 0; passed && i < expected; ++i) {
                        if (file.read() != static_cast<uint8_t>(DiagnosticPattern[i])) {
                            passed = false;
                        }
                    }
                    file.close();
                    if (!passed) {
                        failure = "Diagnostic readback did not match written data.";
                    }
                }
            }
        }

        // Remove the file even if writing or readback failed.
        if (!SD.remove(DiagnosticFileName)) {
            passed = false;
            failure = "Unable to remove diagnostic temporary file.";
        }
    }

    console.printDiagnostic("SD Card", passed, millis() - start);
    if (passed) {
        console.println("  Read/write/readback/delete verified.");
    } else if (failure != nullptr) {
        console.print("  ");
        console.println(failure);
    }
    return passed;
}

bool SDCard::isInitialized() const
{
    return m_initialized;
}

File SDCard::open(const char* path, const char* mode)
{
    if (!m_initialized || path == nullptr) {
        return File();
    }

    return SD.open(path, mode);
}
}
