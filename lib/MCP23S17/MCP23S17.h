/******************************************************************************
 * File: MCP23S17.h
 * Project: AK3Y Instrument Controller
 *
 * Description:
 *   SPI driver for the Microchip MCP23S17 GPIO expander.
 *
 * Responsibilities:
 *   Initializes the expander; manages GPIO port operations and register access; supports device diagnostics and port-state inspection. SPI bus management is delegated to the shared SPI infrastructure.
 *
 * Dependencies:
 *   SPIDevice/SPIManager; Console; Arduino.
 *
 * Author: Bob Fontana, AK3Y
 ******************************************************************************/

#pragma once

#include <stdint.h>

#include <SPIDevice.h>

namespace AK3Y
{

class Console;

/******************************************************************************
 *
 *  MCP23S17
 *
 *  Driver for the Microchip MCP23S17 SPI GPIO Expander.
 *
 ******************************************************************************/

class MCP23S17 : public SPIDevice
{
public:

    /**************************************************************************
     *
     *  Port
     *
     *  Identifies one of the two 8-bit GPIO ports.
     *
     **************************************************************************/

    enum class Port
    {
        A,
        B
    };

    /**************************************************************************
     *
     *  PortState
     *
     *  Diagnostic snapshot of one MCP23S17 GPIO port.
     *
     *  direction:
     *      Contents of IODIR.
     *      0 = Output
     *      1 = Input
     *
     *  outputLatch:
     *      Contents of OLAT.
     *
     *  pinLevels:
     *      Actual logic levels read from GPIO.
     *
     **************************************************************************/

    struct PortState
    {
        uint8_t direction;
        uint8_t outputLatch;
        uint8_t pinLevels;
    };

    /**************************************************************************
     *
     *  Constructor
     *
     **************************************************************************/

    MCP23S17(
        SPIManager& spiManager,
        uint8_t chipSelectPin,
        uint8_t hardwareAddress = 0);

    /**************************************************************************
     *
     *  initialize()
     *
     **************************************************************************/

    bool initialize();

    /**************************************************************************
     *
     *  runDiagnostics()
     *
     **************************************************************************/

    bool runDiagnostics(
        Console& console);

    /**************************************************************************
     *
     *  setPortDirection()
     *
     *  Configures the direction of all eight pins of the selected port.
     *
     *  Each bit in directionMask corresponds to one GPIO pin:
     *
     *      0 = Output
     *      1 = Input
     *
     **************************************************************************/

    bool setPortDirection(
        Port port,
        uint8_t directionMask);

    /**************************************************************************
     *
     *  writePort()
     *
     *  Writes an 8-bit value to the output latch of the selected port.
     *
     **************************************************************************/

    bool writePort(
        Port port,
        uint8_t value);

    /**************************************************************************
     *
     *  readPort()
     *
     *  Reads the current logic levels of all eight pins of the selected port.
     *
     **************************************************************************/

    bool readPort(
        Port port,
        uint8_t& value);

    /**************************************************************************
     *
     *  setPullups()
     *
     *  Configures the internal pull-up resistors for the selected port.
     *
     *  Each bit in pullupMask corresponds to one GPIO pin:
     *
     *      0 = Pull-up disabled
     *      1 = Pull-up enabled
     *
     **************************************************************************/

    bool setPullups(
        Port port,
        uint8_t pullupMask);

    /**************************************************************************
     *
     *  readPortState()
     *
     *  Reads a diagnostic snapshot of the selected port.
     *
     *  This provides visibility into:
     *
     *      • I/O direction
     *      • Output latch contents
     *      • Actual GPIO pin levels
     *
     *  Register addresses remain private to the MCP23S17 driver.
     *
     **************************************************************************/

    bool readPortState(
        Port port,
        PortState& state);

private:

    /**************************************************************************
     *
     *  writeOpcode()
     *
     *  Returns the SPI write opcode for the configured hardware address.
     *
     **************************************************************************/

    uint8_t writeOpcode() const;

    /**************************************************************************
     *
     *  readOpcode()
     *
     *  Returns the SPI read opcode for the configured hardware address.
     *
     **************************************************************************/

    uint8_t readOpcode() const;

    /**************************************************************************
     *
     *  writeRegister()
     *
     *  Writes one MCP23S17 register.
     *
     **************************************************************************/

    bool writeRegister(
        uint8_t registerAddress,
        uint8_t value);

    /**************************************************************************
     *
     *  readRegister()
     *
     *  Reads one MCP23S17 register.
     *
     **************************************************************************/

    bool readRegister(
        uint8_t registerAddress,
        uint8_t& value);

private:

    //
    // MCP23S17 hardware address (A2:A0).
    //

    uint8_t m_hardwareAddress;
};

}   // namespace AK3Y