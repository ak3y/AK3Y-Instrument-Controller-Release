/******************************************************************************
 * File: Console.cpp
 * Project: AK3Y Instrument Controller
 * Purpose: Serial console service implementation.
 * Details: Implements console initialization, formatted output, nonblocking input, and status reporting through Arduino Serial.
 * Author: Bob Fontana, AK3Y
 *
 * Documentation only; firmware history is maintained in Git.
 ******************************************************************************/

#include "Console.h"

#include <Configuration.h>

#include <stdarg.h>
#include <string.h>

namespace AK3Y
{

namespace
{
    //
    // Column at which report status begins.
    //

    constexpr int ReportColumn = 30;
}

/******************************************************************************
 *
 *  Constructor
 *
 ******************************************************************************/

Console::Console()
{
}

/******************************************************************************
 *
 *  begin()
 *
 ******************************************************************************/

bool Console::begin()
{
    Serial.begin(
        Configuration::Console::BaudRate);

    while (!Serial)
    {
        ;
    }

    return true;
}

/******************************************************************************
 *
 *  line()
 *
 ******************************************************************************/

void Console::line()
{
    Serial.println(
        "============================================================");
}

/******************************************************************************
 *
 *  printBanner()
 *
 ******************************************************************************/

void Console::printBanner()
{
    line();

    println("AK3Y Instrument Controller");
    println("Wi-Fi Instrument Controller");

    println();

    print("Firmware : ");
    println(
        Configuration::Firmware::Version);

    print("Hardware : ");
    println(
        Configuration::Firmware::HardwareRevision);

    print("Build    : ");
    print(__DATE__);
    print(" ");
    println(__TIME__);

    print("Author   : ");
    println("Bob Fontana, AK3Y");

    line();
}

/******************************************************************************
 *
 *  printReportPrefix()
 *
 ******************************************************************************/

void Console::printReportPrefix(
    const char* label)
{
    Serial.print(label);

    int length = strlen(label);

    while (length < ReportColumn)
    {
        Serial.print('.');
        ++length;
    }
}

/******************************************************************************
 *
 *  printInitialization()
 *
 ******************************************************************************/

void Console::printInitialization(
    const char* subsystem,
    bool success)
{
    printReportPrefix(subsystem);

    if (success)
    {
        Serial.println("PASS");
    }
    else
    {
        Serial.println("FAIL");
    }
}

/******************************************************************************
 *
 *  printInitialization()
 *
 ******************************************************************************/

void Console::printInitialization(
    const char* subsystem,
    bool success,
    uint32_t elapsedMs)
{
    printReportPrefix(subsystem);

    if (success)
    {
        Serial.print("PASS");
    }
    else
    {
        Serial.print("FAIL");
    }

    Serial.printf(
        " %5lu ms\n",
        static_cast<unsigned long>(elapsedMs));
}

/******************************************************************************
 *
 *  printDiagnostic()
 *
 ******************************************************************************/

void Console::printDiagnostic(
    const char* subsystem,
    bool success,
    uint32_t elapsedMs)
{
    //
    // Diagnostics currently use the same presentation format as
    // initialization reporting. A separate public interface is
    // provided to allow future diagnostic formatting without
    // affecting callers.
    //

    printInitialization(
        subsystem,
        success,
        elapsedMs);
}

/******************************************************************************
 *
 *  printStartupTime()
 *
 ******************************************************************************/

void Console::printStartupTime(
    uint32_t elapsedMs)
{
    println();

    printReportPrefix("Startup Time");

    Serial.printf(
        " %5lu ms\n",
        static_cast<unsigned long>(elapsedMs));
}

/******************************************************************************
 *
 *  printSystemReady()
 *
 ******************************************************************************/

void Console::printSystemReady()
{
    println();
    println("System Ready");

    line();
}

/******************************************************************************
 *
 *  print()
 *
 ******************************************************************************/

void Console::print(
    const char* text)
{
    Serial.print(text);
}

/******************************************************************************
 *
 *  println()
 *
 ******************************************************************************/

void Console::println()
{
    Serial.println();
}

/******************************************************************************
 *
 *  println()
 *
 ******************************************************************************/

void Console::println(
    const char* text)
{
    Serial.println(text);
}

/******************************************************************************
 *
 *  printf()
 *
 ******************************************************************************/

void Console::printf(
    const char* format,
    ...)
{
    char buffer[256];

    va_list args;

    va_start(args, format);

    vsnprintf(
        buffer,
        sizeof(buffer),
        format,
        args);

    va_end(args);

    Serial.print(buffer);
}

/******************************************************************************
 *
 *  available()
 *
 ******************************************************************************/

bool Console::available()
{
    return Serial.available() > 0;
}

/******************************************************************************
 *
 *  read()
 *
 ******************************************************************************/

int Console::read()
{
    if (!available())
    {
        return -1;
    }

    return Serial.read();
}

}   // namespace AK3Y