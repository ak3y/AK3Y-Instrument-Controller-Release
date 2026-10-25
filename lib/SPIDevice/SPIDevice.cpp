/******************************************************************************
 * File: SPIDevice.cpp
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

#include "SPIDevice.h"
#include "SPIManager.h"

#include <Arduino.h>

namespace AK3Y
{

/******************************************************************************
 *
 *  Constructor
 *
 ******************************************************************************/

SPIDevice::SPIDevice(
    SPIManager& spiManager,
    uint8_t chipSelectPin)
    :
    m_spiManager(spiManager),
    m_chipSelectPin(chipSelectPin)
{
}

/******************************************************************************
 *
 *  initialize()
 *
 ******************************************************************************/

bool SPIDevice::initialize()
{
    pinMode(
        m_chipSelectPin,
        OUTPUT);

    //
    // Leave the device deselected.
    //

    deselect();

    return true;
}

/******************************************************************************
 *
 *  select()
 *
 ******************************************************************************/

void SPIDevice::select()
{
    //
    // Begin the SPI transaction before asserting chip select.
    //

    m_spiManager.beginTransaction();

    digitalWrite(
        m_chipSelectPin,
        LOW);
}

/******************************************************************************
 *
 *  deselect()
 *
 ******************************************************************************/

void SPIDevice::deselect()
{
    //
    // Deassert chip select before ending the SPI transaction.
    //

    digitalWrite(
        m_chipSelectPin,
        HIGH);

    m_spiManager.endTransaction();
}

/******************************************************************************
 *
 *  transfer()
 *
 ******************************************************************************/

uint8_t SPIDevice::transfer(
    uint8_t value)
{
    return m_spiManager.transfer(
        value);
}

}   // namespace AK3Y