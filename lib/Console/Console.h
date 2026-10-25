/******************************************************************************
 * File: Console.h
 * Project: AK3Y Instrument Controller
 * Purpose: Serial console service interface.
 * Details: Declares console initialization, formatted output, input handling, and startup reporting used by application diagnostics.
 * Author: Bob Fontana, AK3Y
 *
 * Documentation only; firmware history is maintained in Git.
 ******************************************************************************/

#pragma once

#include <Arduino.h>

namespace AK3Y
{

class Console
{
public:

    /**************************************************************************
     *
     *  Constructor
     *
     **************************************************************************/

    Console();

    /**************************************************************************
     *
     *  begin()
     *
     **************************************************************************/

    bool begin();

    //
    // Report Formatting
    //

    void line();

    void printBanner();

    void printInitialization(
        const char* subsystem,
        bool success);

    void printInitialization(
        const char* subsystem,
        bool success,
        uint32_t elapsedMs);

    void printDiagnostic(
        const char* subsystem,
        bool success,
        uint32_t elapsedMs);

    void printStartupTime(
        uint32_t elapsedMs);

    void printSystemReady();

    //
    // Basic Output
    //

    void print(
        const char* text);

    void println();

    void println(
        const char* text);

    void printf(
        const char* format,
        ...);

    //
    // Non-Blocking Input
    //
    // available() reports whether at least one character is waiting.
    //
    // read() returns the next character, or -1 if no character
    // is available.
    //

    bool available();

    int read();

private:

    //
    // Report Helpers
    //

    void printReportPrefix(
        const char* label);
};

}   // namespace AK3Y