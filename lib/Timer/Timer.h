/******************************************************************************
 * File: Timer.h
 * Project: AK3Y Instrument Controller
 * Purpose: Timing services interface
 * Description: Declares elapsed-time and timeout utilities and timer diagnostics.
 * Note: This file header describes responsibilities; release history is in Git.
 ******************************************************************************/

#pragma once

#include <stdint.h>

namespace AK3Y
{

class Console;

/******************************************************************************
 *
 *  Timer
 *
 *  Provides centralized timing services for the firmware.
 *
 ******************************************************************************/

class Timer
{
public:

    /**************************************************************************
     *
     *  Constructor
     *
     **************************************************************************/

    Timer();

    /**************************************************************************
     *
     *  initialize()
     *
     *  Initializes the Timer library.
     *
     *  Returns:
     *      true if initialization succeeds.
     *
     **************************************************************************/

    bool initialize();

    /**************************************************************************
     *
     *  runDiagnostics()
     *
     *  Executes Timer library diagnostics.
     *
     *  Returns:
     *      true if all diagnostics pass.
     *
     **************************************************************************/

    bool runDiagnostics(
        Console& console);

    /**************************************************************************
     *
     *  milliseconds()
     *
     *  Returns the number of milliseconds since system startup.
     *
     **************************************************************************/

    uint32_t milliseconds() const;

    /**************************************************************************
     *
     *  microseconds()
     *
     *  Returns the number of microseconds since system startup.
     *
     **************************************************************************/

    uint32_t microseconds() const;

    /**************************************************************************
     *
     *  elapsed()
     *
     *  Returns the elapsed time in milliseconds since the specified
     *  starting time.
     *
     **************************************************************************/

    uint32_t elapsed(
        uint32_t startMilliseconds) const;

    /**************************************************************************
     *
     *  delay()
     *
     *  Delays execution for the specified number of milliseconds.
     *
     **************************************************************************/

    void delay(
        uint32_t milliseconds) const;

    /**************************************************************************
     *
     *  expired()
     *
     *  Determines whether the specified timeout interval has expired.
     *
     **************************************************************************/

    bool expired(
        uint32_t startMilliseconds,
        uint32_t timeoutMilliseconds) const;
};

}   // namespace AK3Y