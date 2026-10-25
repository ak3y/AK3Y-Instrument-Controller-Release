/******************************************************************************
 * File: SPIManager.h
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

#pragma once

#include <stdint.h>

namespace AK3Y
{

class Console;

/******************************************************************************
 *
 *  SPIManager
 *
 *  Owns and manages the system SPI bus.
 *
 ******************************************************************************/

class SPIManager
{
public:

    /**************************************************************************
     *
     *  Constructor
     *
     **************************************************************************/

    SPIManager();

    /**************************************************************************
     *
     *  initialize()
     *
     *  Initializes the SPI peripheral.
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
     *  Executes SPI bus diagnostics.
     *
     *  Returns:
     *      true if diagnostics pass.
     *
     **************************************************************************/

    bool runDiagnostics(
        Console& console);

    /**************************************************************************
     *
     *  beginTransaction()
     *
     *  Begins an SPI transaction.
     *
     **************************************************************************/

    void beginTransaction();

    /**************************************************************************
     *
     *  endTransaction()
     *
     *  Ends the current SPI transaction.
     *
     **************************************************************************/

    void endTransaction();

    /**************************************************************************
     *
     *  transfer()
     *
     *  Transfers one byte over the SPI bus.
     *
     *  Parameters:
     *      value - Byte to transmit.
     *
     *  Returns:
     *      Byte received from the peripheral.
     *
     **************************************************************************/

    uint8_t transfer(
        uint8_t value);

private:

    //
    // Indicates whether the SPI peripheral has been
    // successfully initialized.
    //

    bool m_initialized;
};

}   // namespace AK3Y