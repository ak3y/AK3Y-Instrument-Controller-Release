/******************************************************************************
 * File: SPIManager.cpp
 * Project: AK3Y Instrument Controller
 *
 * Description:
 *   Shared SPI bus initialization and transaction management.
 *
 * Responsibilities:
 *   Initializes the ESP32 SPI peripheral; provides shared transaction services and SPI self-diagnostics. Device chip-select ownership and peripheral protocols belong to individual device drivers.
 *
 * Dependencies:
 *   Arduino SPI library; Console for diagnostic output.
 *
 * Author: Bob Fontana, AK3Y
 ******************************************************************************/

#include "SPIManager.h"

#include "Console.h"

#include <SPI.h>

namespace AK3Y
{

/******************************************************************************
 *
 *  Constructor
 *
 ******************************************************************************/

SPIManager::SPIManager()
    : m_initialized(false)
{
}

/******************************************************************************
 *
 *  initialize()
 *
 ******************************************************************************/

bool SPIManager::initialize()
{
    //
    // Initialize the system SPI peripheral.
    //

    SPI.begin();

    m_initialized = true;

    return true;
}

/******************************************************************************
 *
 *  runDiagnostics()
 *
 ******************************************************************************/

bool SPIManager::runDiagnostics(
    Console& console)
{
    //
    // At present, diagnostics verify only that the SPI manager
    // has successfully initialized the SPI peripheral.
    //
    // More comprehensive diagnostics will be added as SPI
    // peripheral devices are integrated.
    //

    console.printDiagnostic(
        "SPI",
        m_initialized,
        0);

    return m_initialized;
}

/******************************************************************************
 *
 *  beginTransaction()
 *
 ******************************************************************************/

void SPIManager::beginTransaction()
{
    //
    // Transaction management will be implemented when the first
    // SPI peripheral driver (MCP23S17) is integrated.
    //
}

/******************************************************************************
 *
 *  endTransaction()
 *
 ******************************************************************************/

void SPIManager::endTransaction()
{
    //
    // Transaction management will be implemented when the first
    // SPI peripheral driver (MCP23S17) is integrated.
    //
}

/******************************************************************************
 *
 *  transfer()
 *
 ******************************************************************************/

uint8_t SPIManager::transfer(
    uint8_t value)
{
    return SPI.transfer(
        value);
}

}   // namespace AK3Y