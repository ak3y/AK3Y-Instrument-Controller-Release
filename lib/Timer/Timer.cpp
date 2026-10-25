/******************************************************************************
 * File: Timer.cpp
 * Project: AK3Y Instrument Controller
 * Purpose: Timing services
 * Description: Implements elapsed-time and timeout utilities and timer diagnostics.
 * Note: This file header describes responsibilities; release history is in Git.
 ******************************************************************************/

#include "Timer.h"

#include "Console.h"

#include <Configuration.h>

#include <Arduino.h>

namespace AK3Y
{

/******************************************************************************
 *
 *  Constructor
 *
 ******************************************************************************/

Timer::Timer()
{
}

/******************************************************************************
 *
 *  initialize()
 *
 ******************************************************************************/

bool Timer::initialize()
{
    //
    // No initialization is currently required.
    //

    return true;
}

/******************************************************************************
 *
 *  runDiagnostics()
 *
 ******************************************************************************/

bool Timer::runDiagnostics(
    Console& console)
{
    const uint32_t diagnosticDelayMs =
        Configuration::Diagnostics::TimerDiagnosticDelayMs;

    uint32_t start = milliseconds();

    delay(diagnosticDelayMs);

    uint32_t elapsedTime = elapsed(start);

   const uint32_t timingToleranceMs = 1;

    bool success =
        (elapsedTime + timingToleranceMs >= diagnosticDelayMs) &&
        expired(
            start,
            diagnosticDelayMs / 2);

    console.printDiagnostic(
        "Timer",
        success,
        elapsedTime);

    return success;
}

/******************************************************************************
 *
 *  milliseconds()
 *
 ******************************************************************************/

uint32_t Timer::milliseconds() const
{
    return ::millis();
}

/******************************************************************************
 *
 *  microseconds()
 *
 ******************************************************************************/

uint32_t Timer::microseconds() const
{
    return ::micros();
}

/******************************************************************************
 *
 *  elapsed()
 *
 ******************************************************************************/

uint32_t Timer::elapsed(
    uint32_t startMilliseconds) const
{
    //
    // Unsigned subtraction correctly handles rollover.
    //

    return milliseconds() - startMilliseconds;
}

/******************************************************************************
 *
 *  delay()
 *
 ******************************************************************************/

void Timer::delay(
    uint32_t milliseconds) const
{
    ::delay(milliseconds);
}

/******************************************************************************
 *
 *  expired()
 *
 ******************************************************************************/

bool Timer::expired(
    uint32_t startMilliseconds,
    uint32_t timeoutMilliseconds) const
{
    return elapsed(startMilliseconds) >= timeoutMilliseconds;
}

}   // namespace AK3Y