/******************************************************************************
 * File: MCP23S17.cpp
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

#include "MCP23S17.h"

#include "Console.h"

#include <Arduino.h>

namespace AK3Y
{

namespace
{

//
// SPI opcodes
//

constexpr uint8_t WRITE_OPCODE_BASE = 0x40;
constexpr uint8_t READ_OPCODE_BASE  = 0x41;

//
// Dummy byte transmitted during SPI register reads.
//

constexpr uint8_t DummyByte = 0x00;

//
// Register addresses assume IOCON.BANK = 0,
// the default sequential register map.
//

constexpr uint8_t IODIRA   = 0x00;
constexpr uint8_t IODIRB   = 0x01;

constexpr uint8_t IPOLA    = 0x02;
constexpr uint8_t IPOLB    = 0x03;

constexpr uint8_t GPINTENA = 0x04;
constexpr uint8_t GPINTENB = 0x05;

constexpr uint8_t DEFVALA  = 0x06;
constexpr uint8_t DEFVALB  = 0x07;

constexpr uint8_t INTCONA  = 0x08;
constexpr uint8_t INTCONB  = 0x09;

constexpr uint8_t IOCON    = 0x0A;

constexpr uint8_t GPPUA    = 0x0C;
constexpr uint8_t GPPUB    = 0x0D;

constexpr uint8_t INTFA    = 0x0E;
constexpr uint8_t INTFB    = 0x0F;

constexpr uint8_t INTCAPA  = 0x10;
constexpr uint8_t INTCAPB  = 0x11;

constexpr uint8_t GPIOA    = 0x12;
constexpr uint8_t GPIOB    = 0x13;

constexpr uint8_t OLATA    = 0x14;
constexpr uint8_t OLATB    = 0x15;

}   // anonymous namespace

/******************************************************************************
 *
 *  Constructor
 *
 ******************************************************************************/

MCP23S17::MCP23S17(
    SPIManager& spiManager,
    uint8_t chipSelectPin,
    uint8_t hardwareAddress)
    :
    SPIDevice(
        spiManager,
        chipSelectPin),
    m_hardwareAddress(hardwareAddress)
{
}

/******************************************************************************
 *
 *  writeOpcode()
 *
 ******************************************************************************/

uint8_t MCP23S17::writeOpcode() const
{
    return
        WRITE_OPCODE_BASE |
        (m_hardwareAddress << 1);
}

/******************************************************************************
 *
 *  readOpcode()
 *
 ******************************************************************************/

uint8_t MCP23S17::readOpcode() const
{
    return
        READ_OPCODE_BASE |
        (m_hardwareAddress << 1);
}

/******************************************************************************
 *
 *  initialize()
 *
 ******************************************************************************/

bool MCP23S17::initialize()
{
    //
    // Initialize the SPIDevice base class.
    //

    if (!SPIDevice::initialize())
    {
        return false;
    }

    //
    // Verify communication with the MCP23S17 by reading
    // the IOCON register.
    //

    uint8_t registerValue;

    return readRegister(
        IOCON,
        registerValue);
}

/******************************************************************************
 *
 *  runDiagnostics()
 *
 ******************************************************************************/

bool MCP23S17::runDiagnostics(
    Console& console)
{
    uint32_t start =
        millis();

    uint8_t registerValue;

    bool success =
        readRegister(
            IOCON,
            registerValue);

    console.printDiagnostic(
        "MCP23S17",
        success,
        millis() - start);

    return success;
}

/******************************************************************************
 *
 *  setPortDirection()
 *
 *  Configures the direction of all eight GPIO pins on the selected port.
 *
 *  MCP23S17 IODIR convention:
 *
 *      0 = Output
 *      1 = Input
 *
 ******************************************************************************/

bool MCP23S17::setPortDirection(
    Port port,
    uint8_t directionMask)
{
    uint8_t registerAddress;

    if (port == Port::A)
    {
        registerAddress = IODIRA;
    }
    else
    {
        registerAddress = IODIRB;
    }

    return writeRegister(
        registerAddress,
        directionMask);
}

/******************************************************************************
 *
 *  writePort()
 *
 *  Writes an 8-bit value to the output latch of the selected port.
 *
 *  OLAT is used rather than GPIO so that this operation explicitly
 *  represents modification of the MCP23S17 output latch.
 *
 ******************************************************************************/

bool MCP23S17::writePort(
    Port port,
    uint8_t value)
{
    uint8_t registerAddress;

    if (port == Port::A)
    {
        registerAddress = OLATA;
    }
    else
    {
        registerAddress = OLATB;
    }

    return writeRegister(
        registerAddress,
        value);
}

/******************************************************************************
 *
 *  readPort()
 *
 *  Reads the actual logic levels present on the selected GPIO port.
 *
 ******************************************************************************/

bool MCP23S17::readPort(
    Port port,
    uint8_t& value)
{
    uint8_t registerAddress;

    if (port == Port::A)
    {
        registerAddress = GPIOA;
    }
    else
    {
        registerAddress = GPIOB;
    }

    return readRegister(
        registerAddress,
        value);
}

/******************************************************************************
 *
 *  setPullups()
 *
 *  Configures the internal pull-up resistors on the selected GPIO port.
 *
 *  MCP23S17 GPPU convention:
 *
 *      0 = Pull-up disabled
 *      1 = Pull-up enabled
 *
 ******************************************************************************/

bool MCP23S17::setPullups(
    Port port,
    uint8_t pullupMask)
{
    uint8_t registerAddress;

    if (port == Port::A)
    {
        registerAddress = GPPUA;
    }
    else
    {
        registerAddress = GPPUB;
    }

    return writeRegister(
        registerAddress,
        pullupMask);
}

/******************************************************************************
 *
 *  readPortState()
 *
 *  Reads the direction register, output latch, and actual GPIO levels
 *  for the selected port.
 *
 *  This method is intended primarily for diagnostics and hardware
 *  bring-up.  It deliberately exposes the meaning of the registers
 *  without exposing their physical addresses to higher software layers.
 *
 ******************************************************************************/

bool MCP23S17::readPortState(
    Port port,
    PortState& state)
{
    uint8_t directionRegister;
    uint8_t outputLatchRegister;
    uint8_t gpioRegister;

    if (port == Port::A)
    {
        directionRegister   = IODIRA;
        outputLatchRegister = OLATA;
        gpioRegister        = GPIOA;
    }
    else
    {
        directionRegister   = IODIRB;
        outputLatchRegister = OLATB;
        gpioRegister        = GPIOB;
    }

    if (!readRegister(
            directionRegister,
            state.direction))
    {
        return false;
    }

    if (!readRegister(
            outputLatchRegister,
            state.outputLatch))
    {
        return false;
    }

    if (!readRegister(
            gpioRegister,
            state.pinLevels))
    {
        return false;
    }

    return true;
}

/******************************************************************************
 *
 *  writeRegister()
 *
 ******************************************************************************/

bool MCP23S17::writeRegister(
    uint8_t registerAddress,
    uint8_t value)
{
    //
    // Begin an SPI transaction with the MCP23S17.
    //

    select();

    //
    // Send the write opcode.
    //

    transfer(
        writeOpcode());

    //
    // Send the register address.
    //

    transfer(
        registerAddress);

    //
    // Send the register value.
    //

    transfer(
        value);

    //
    // Complete the SPI transaction.
    //

    deselect();

    return true;
}

/******************************************************************************
 *
 *  readRegister()
 *
 ******************************************************************************/

bool MCP23S17::readRegister(
    uint8_t registerAddress,
    uint8_t& value)
{
    //
    // Begin an SPI transaction with the MCP23S17.
    //

    select();

    //
    // Send the read opcode.
    //

    transfer(
        readOpcode());

    //
    // Send the register address.
    //

    transfer(
        registerAddress);

    //
    // Read the register value.
    //

    value = transfer(
        DummyByte);

    //
    // Complete the SPI transaction.
    //

    deselect();

    return true;
}

}   // namespace AK3Y