/******************************************************************************
 * File: SPIDevice.h
 * Project: AK3Y Instrument Controller
 *
 * Description:
 *   Common foundation for SPI peripheral drivers.
 *
 * Responsibilities:
 *   Maintains the device chip-select pin and SPIManager reference; provides common device selection services. Bus initialization and device-specific register protocols remain outside this class.
 *
 * Dependencies:
 *   SPIManager; Arduino GPIO.
 *
 * Author: Bob Fontana, AK3Y
 ******************************************************************************/

#pragma once

#include <stdint.h>

namespace AK3Y
{

class SPIManager;

/******************************************************************************
 *
 *  SPIDevice
 *
 *  Common base class for all SPI peripheral drivers.
 *
 ******************************************************************************/

class SPIDevice
{
public:

    /**************************************************************************
     *
     *  Constructor
     *
     **************************************************************************/

    SPIDevice(
        SPIManager& spiManager,
        uint8_t chipSelectPin);

    /**************************************************************************
     *
     *  initialize()
     *
     *  Initializes the device chip-select signal.
     *
     **************************************************************************/

    bool initialize();

protected:

    /**************************************************************************
     *
     *  select()
     *
     *  Selects the SPI device.
     *
     **************************************************************************/

    void select();

    /**************************************************************************
     *
     *  deselect()
     *
     *  Deselects the SPI device.
     *
     **************************************************************************/

    void deselect();

    /**************************************************************************
     *
     *  transfer()
     *
     *  Transfers one byte to the selected SPI device.
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

    //
    // Reference to the system SPI manager.
    //

    SPIManager& m_spiManager;

private:

    //
    // Device chip-select GPIO.
    //

    uint8_t m_chipSelectPin;
};

}   // namespace AK3Y