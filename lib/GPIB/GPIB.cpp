/******************************************************************************
 * File: GPIB.cpp
 * Project: AK3Y Instrument Controller
 * Description: Implements the IEEE-488 electrical-interface abstraction.
 *
 * Responsibilities:
 *   - Drive and sample GPIB data, handshake, and management signals.
 *   - Configure talker/listener direction and interface buffers.
 *   - Map bus signals to the MCP23S17 and associated interface hardware.
 *
 * IEEE-488 protocol sequencing and instrument commands are handled by
 * higher-level modules, not by this electrical-interface implementation.
 *
 * Author: Bob Fontana, AK3Y
 ******************************************************************************/

#include "GPIB.h"

#include <Arduino.h>

#include <Configuration.h>
#include <Console.h>
#include <MCP23S17.h>

namespace AK3Y
{

namespace
{

/******************************************************************************
 *
 *  MCP23S17 Port A Signal Assignments
 *
 *  These assignments follow the PCB schematic directly:
 *
 *      GPA0    REN
 *      GPA1    SRQ
 *      GPA2    IFC
 *      GPA3    NDAC
 *      GPA4    NRFD
 *      GPA5    DAV
 *      GPA6    EOI
 *      GPA7    ATN
 *
 ******************************************************************************/

constexpr uint8_t REN_MASK  = 0x01;
constexpr uint8_t SRQ_MASK  = 0x02;
constexpr uint8_t IFC_MASK  = 0x04;
constexpr uint8_t NDAC_MASK = 0x08;
constexpr uint8_t NRFD_MASK = 0x10;
constexpr uint8_t DAV_MASK  = 0x20;
constexpr uint8_t EOI_MASK  = 0x40;
constexpr uint8_t ATN_MASK  = 0x80;

/******************************************************************************
 *
 *  Port A Direction Masks
 *
 *  MCP23S17 convention:
 *
 *      0 = Output
 *      1 = Input
 *
 *  Talker:
 *
 *      ATN     Output
 *      EOI     Output
 *      DAV     Output
 *      NRFD    Input
 *      NDAC    Input
 *      IFC     Output
 *      SRQ     Input
 *      REN     Output
 *
 ******************************************************************************/

constexpr uint8_t TALKER_DIRECTION_MASK =
    NRFD_MASK |
    NDAC_MASK |
    SRQ_MASK;

/******************************************************************************
 *
 *  Listener direction masks
 *
 *  With ATN released:
 *
 *      EOI     Input
 *      DAV     Input
 *      SRQ     Input
 *
 *  With ATN asserted:
 *
 *      EOI     Output
 *      DAV     Input
 *      SRQ     Input
 *
 *  The EOI behavior follows the SN75161B function table when
 *  DC = LOW and TE = LOW.
 *
 ******************************************************************************/

constexpr uint8_t LISTENER_ATN_RELEASED_DIRECTION_MASK =
    EOI_MASK |
    DAV_MASK |
    SRQ_MASK;

constexpr uint8_t LISTENER_ATN_ASSERTED_DIRECTION_MASK =
    DAV_MASK |
    SRQ_MASK;

/******************************************************************************
 *
 *  Port B Direction Masks
 *
 *  Port B contains DIO1 through DIO8.
 *
 ******************************************************************************/

constexpr uint8_t DATA_OUTPUT_DIRECTION_MASK = 0x00;
constexpr uint8_t DATA_INPUT_DIRECTION_MASK  = 0xFF;

/******************************************************************************
 *
 *  Released state
 *
 *  GPIB signals are active LOW.
 *
 ******************************************************************************/

constexpr uint8_t ALL_SIGNALS_RELEASED = 0xFF;

}   // anonymous namespace

/******************************************************************************
 *
 *  Constructor
 *
 *  IMPORTANT:
 *      No hardware is accessed by the constructor.
 *
 ******************************************************************************/

GPIB::GPIB(
    MCP23S17& mcp23s17,
    Console& console)
    :
    m_mcp23s17(mcp23s17),
    m_console(console),
    m_mode(Mode::Listener),
    m_controlOutputState(ALL_SIGNALS_RELEASED),
    m_atnAsserted(false),
    m_initialized(false)
{
}

/******************************************************************************
 *
 *  initialize()
 *
 *  Places the GPIB electrical interface into a known, safe state.
 *
 *  The 74HCT245 interface buffers are disabled before any direction
 *  changes are made.  They remain disabled until the MCP23S17 and
 *  GPIB transceiver control signals have been configured.
 *
 ******************************************************************************/

bool GPIB::initialize()
{
    //
    // Configure the buffer output-enable pin first.
    //
    // GPIB_BUF_OE is active LOW.  Setting it HIGH disables
    // U8-U11 and isolates the MCP23S17 from the GPIB transceivers.
    //

    pinMode(
        Configuration::Pins::GPIB::BufferEnable,
        OUTPUT);

    disableBuffers();

    //
    // Configure the direct ESP32 GPIB control signals.
    //

    pinMode(
        Configuration::Pins::GPIB::TE,
        OUTPUT);

    pinMode(
        Configuration::Pins::GPIB::DC,
        OUTPUT);

    pinMode(
        Configuration::Pins::GPIB::EOIDirection,
        OUTPUT);

    //
    // This design is the GPIB System Controller.
    //
    // DC = LOW causes the SN75161B to:
    //
    //      Transmit ATN
    //      Receive  SRQ
    //      Transmit REN
    //      Transmit IFC
    //

    digitalWrite(
        Configuration::Pins::GPIB::DC,
        LOW);

    //
    // Begin in Listener mode.
    //
    // TE = LOW:
    //
    //      SN75160B receives DIO1-DIO8.
    //
    //      SN75161B receives DAV and transmits
    //      NRFD and NDAC.
    //

    digitalWrite(
        Configuration::Pins::GPIB::TE,
        LOW);

    m_mode = Mode::Listener;

    //
    // Begin with all controllable GPIB signals released.
    //

    m_controlOutputState =
        ALL_SIGNALS_RELEASED;

    m_atnAsserted = false;

    //
    // Load the released state into both MCP23S17 output latches
    // BEFORE configuring any MCP pins as outputs.
    //
    // This prevents an output from momentarily asserting a GPIB
    // signal when its direction is changed.
    //

    if (!m_mcp23s17.writePort(
            MCP23S17::Port::A,
            m_controlOutputState))
    {
        return false;
    }

    //
    // The GPIB data bus is negative true.
    //
    // Loading 0xFF into the physical output latch corresponds to
    // all eight bus lines released.
    //

    if (!m_mcp23s17.writePort(
            MCP23S17::Port::B,
            0xFF))
    {
        return false;
    }

    //
    // Disable MCP23S17 internal pull-ups.
    //
    // The GPIB transceivers provide the required electrical
    // interface characteristics.
    //

    if (!m_mcp23s17.setPullups(
            MCP23S17::Port::A,
            0x00))
    {
        return false;
    }

    if (!m_mcp23s17.setPullups(
            MCP23S17::Port::B,
            0x00))
    {
        return false;
    }

    //
    // Configure the MCP23S17 for Listener operation.
    //

    if (!configureListener())
    {
        return false;
    }

    //
    // The complete interface is now configured.
    //
    // Enable U8-U11 only after all directions and output states
    // are known.
    //

    enableBuffers();

    m_initialized = true;

    return true;
}

/******************************************************************************
 *
 *  runDiagnostics()
 *
 ******************************************************************************/

bool GPIB::runDiagnostics()
{
    //
    // For the initial implementation, verify that initialization
    // completed successfully.
    //
    // More extensive electrical diagnostics will be added during
    // hardware bring-up.
    //

    return m_initialized;
}

/******************************************************************************
 *
 *  readControlPortDiagnostic()
 *
 *  Returns a diagnostic snapshot of MCP23S17 Port A without exposing
 *  MCP23S17 register addresses to higher software layers.
 *
 ******************************************************************************/

bool GPIB::readControlPortDiagnostic(
    uint8_t& direction,
    uint8_t& outputLatch,
    uint8_t& pinLevels)
{
    if (!m_initialized)
    {
        return false;
    }

    MCP23S17::PortState state;

    if (!m_mcp23s17.readPortState(
            MCP23S17::Port::A,
            state))
    {
        return false;
    }

    direction   = state.direction;
    outputLatch = state.outputLatch;
    pinLevels   = state.pinLevels;

    return true;
}

/******************************************************************************
 *
 *  setMode()
 *
 *  Changes the electrical data-transfer direction.
 *
 *  A break-before-make sequence is used:
 *
 *      1. Disable the 74HCT245 buffers.
 *      2. Configure MCP23S17 directions.
 *      3. Configure TE and EOI direction.
 *      4. Re-enable the buffers.
 *
 ******************************************************************************/

bool GPIB::setMode(
    Mode mode)
{
    if (!m_initialized)
    {
        return false;
    }

    if (mode == m_mode)
    {
        return true;
    }

    disableBuffers();

    bool success;

    if (mode == Mode::Talker)
    {
        success =
            configureTalker();
    }
    else
    {
        success =
            configureListener();
    }

    if (!success)
    {
        //
        // Leave the interface isolated if configuration fails.
        //

        return false;
    }

    m_mode = mode;

    enableBuffers();

    return true;
}

/******************************************************************************
 *
 *  mode()
 *
 ******************************************************************************/

GPIB::Mode GPIB::mode() const
{
    return m_mode;
}

/******************************************************************************
 *
 *  configureTalker()
 *
 ******************************************************************************/

bool GPIB::configureTalker()
{
    //
    // Establish MCP23S17 directions while the HCT245 buffers
    // remain disabled.
    //

    if (!m_mcp23s17.setPortDirection(
            MCP23S17::Port::B,
            DATA_OUTPUT_DIRECTION_MASK))
    {
        return false;
    }

    if (!m_mcp23s17.setPortDirection(
            MCP23S17::Port::A,
            TALKER_DIRECTION_MASK))
    {
        return false;
    }

    //
    // TE = HIGH places the SN75160B in transmit mode and selects
    // the talker direction for DAV, NRFD and NDAC.
    //

    digitalWrite(
        Configuration::Pins::GPIB::TE,
        HIGH);

    //
    // In Talker mode with DC LOW and TE HIGH, EOI is transmitted.
    //
    // U11 is wired with MCP23S17 GPA6 / EOI_2 on A1 and the
    // SN75161B terminal-side EOI signal on B1.
    //
    // 74HCT245 DIR HIGH transfers A -> B, so EOI transmission
    // requires DIR HIGH.
    //

    digitalWrite(
        Configuration::Pins::GPIB::EOIDirection,
        HIGH);

    return true;
}

/******************************************************************************
 *
 *  configureListener()
 *
 ******************************************************************************/

bool GPIB::configureListener()
{
    //
    // The data bus is received in Listener mode.
    //

    if (!m_mcp23s17.setPortDirection(
            MCP23S17::Port::B,
            DATA_INPUT_DIRECTION_MASK))
    {
        return false;
    }

    //
    // TE = LOW selects Listener operation.
    //

    digitalWrite(
        Configuration::Pins::GPIB::TE,
        LOW);

    //
    // Configure the Listener-side EOI direction directly here.
    //
    // IMPORTANT:
    //     configureListener() is called by setMode() before m_mode is
    //     changed to Mode::Listener.  Therefore it must not call
    //     updateEOIDirection(), because that routine uses m_mode to
    //     determine whether the interface is currently a Talker or
    //     Listener.  During a Talker -> Listener transition m_mode still
    //     contains Mode::Talker at this point.
    //
    //     Configure the hardware from the requested Listener state and
    //     the logical ATN state instead.
    //

    if (m_atnAsserted)
    {
        //
        // Listener with ATN asserted: EOI is transmitted.
        // U11 must transfer A -> B.
        //

        digitalWrite(
            Configuration::Pins::GPIB::EOIDirection,
            HIGH);

        return m_mcp23s17.setPortDirection(
            MCP23S17::Port::A,
            LISTENER_ATN_ASSERTED_DIRECTION_MASK);
    }

    //
    // Listener with ATN released: EOI is received.
    // U11 must transfer B -> A.
    //

    digitalWrite(
        Configuration::Pins::GPIB::EOIDirection,
        LOW);

    return m_mcp23s17.setPortDirection(
        MCP23S17::Port::A,
        LISTENER_ATN_RELEASED_DIRECTION_MASK);
}

/******************************************************************************
 *
 *  updateEOIDirection()
 *
 *  In Listener mode with DC = LOW and TE = LOW, the SN75161B uses
 *  ATN to determine EOI direction.
 *
 *  ATN released:
 *
 *      EOI is received.
 *
 *  ATN asserted:
 *
 *      EOI is transmitted.
 *
 ******************************************************************************/

bool GPIB::updateEOIDirection()
{
    if (m_mode == Mode::Talker)
    {
        //
        // Talker always transmits EOI.
        // U11 must transfer A -> B.
        //

        digitalWrite(
            Configuration::Pins::GPIB::EOIDirection,
            HIGH);

        return m_mcp23s17.setPortDirection(
            MCP23S17::Port::A,
            TALKER_DIRECTION_MASK);
    }

    //
    // Listener mode.
    //

    if (m_atnAsserted)
    {
        //
        // EOI transmitted.
        // U11 must transfer A -> B.
        //

        digitalWrite(
            Configuration::Pins::GPIB::EOIDirection,
            HIGH);

        return m_mcp23s17.setPortDirection(
            MCP23S17::Port::A,
            LISTENER_ATN_ASSERTED_DIRECTION_MASK);
    }

    //
    // ATN released: EOI received.
    // U11 must transfer B -> A.
    //

    digitalWrite(
        Configuration::Pins::GPIB::EOIDirection,
        LOW);

    return m_mcp23s17.setPortDirection(
        MCP23S17::Port::A,
        LISTENER_ATN_RELEASED_DIRECTION_MASK);
}

/******************************************************************************
 *
 *  writeData()
 *
 *  IEEE-488 data lines use negative logic.
 *
 *  A logical 1 is represented by an asserted LOW bus line.
 *  Therefore the logical byte is complemented before being written
 *  to the physical MCP23S17 output latch.
 *
 ******************************************************************************/

bool GPIB::writeData(
    uint8_t data)
{
    if (!m_initialized ||
        m_mode != Mode::Talker)
    {
        return false;
    }

    return m_mcp23s17.writePort(
        MCP23S17::Port::B,
        static_cast<uint8_t>(~data));
}

/******************************************************************************
 *
 *  readData()
 *
 ******************************************************************************/

bool GPIB::readData(
    uint8_t& data)
{
    if (!m_initialized ||
        m_mode != Mode::Listener)
    {
        return false;
    }

    uint8_t physicalData;

    if (!m_mcp23s17.readPort(
            MCP23S17::Port::B,
            physicalData))
    {
        return false;
    }

    data =
        static_cast<uint8_t>(~physicalData);

    return true;
}

/******************************************************************************
 *
 *  writeControlPort()
 *
 ******************************************************************************/

bool GPIB::writeControlPort()
{
    return m_mcp23s17.writePort(
        MCP23S17::Port::A,
        m_controlOutputState);
}

/******************************************************************************
 *
 *  writeControlSignal()
 *
 ******************************************************************************/

bool GPIB::writeControlSignal(
    uint8_t mask,
    bool asserted)
{
    if (!m_initialized)
    {
        return false;
    }

    //
    // GPIB signals are active LOW.
    //

    if (asserted)
    {
        m_controlOutputState &=
            static_cast<uint8_t>(~mask);
    }
    else
    {
        m_controlOutputState |=
            mask;
    }

    return writeControlPort();
}

/******************************************************************************
 *
 *  readControlSignal()
 *
 ******************************************************************************/

bool GPIB::readControlSignal(
    uint8_t mask,
    bool& asserted)
{
    if (!m_initialized)
    {
        return false;
    }

    uint8_t value;

    if (!m_mcp23s17.readPort(
            MCP23S17::Port::A,
            value))
    {
        return false;
    }

    //
    // Active LOW.
    //

    asserted =
        (value & mask) == 0;

    return true;
}

/******************************************************************************
 *
 *  DAV
 *
 ******************************************************************************/

bool GPIB::assertDAV()
{
    if (m_mode != Mode::Talker)
    {
        return false;
    }

    return writeControlSignal(
        DAV_MASK,
        true);
}

bool GPIB::releaseDAV()
{
    if (m_mode != Mode::Talker)
    {
        return false;
    }

    return writeControlSignal(
        DAV_MASK,
        false);
}

bool GPIB::isDAVAsserted()
{
    bool asserted = false;

    if (!readControlSignal(
            DAV_MASK,
            asserted))
    {
        return false;
    }

    return asserted;
}

/******************************************************************************
 *
 *  NRFD
 *
 ******************************************************************************/

bool GPIB::assertNRFD()
{
    if (m_mode != Mode::Listener)
    {
        return false;
    }

    return writeControlSignal(
        NRFD_MASK,
        true);
}

bool GPIB::releaseNRFD()
{
    if (m_mode != Mode::Listener)
    {
        return false;
    }

    return writeControlSignal(
        NRFD_MASK,
        false);
}

bool GPIB::isNRFDAsserted()
{
    bool asserted = false;

    if (!readControlSignal(
            NRFD_MASK,
            asserted))
    {
        return false;
    }

    return asserted;
}

bool GPIB::readNRFD(
    bool& asserted)
{
    return readControlSignal(
        NRFD_MASK,
        asserted);
}

/******************************************************************************
 *
 *  NDAC
 *
 ******************************************************************************/

bool GPIB::assertNDAC()
{
    if (m_mode != Mode::Listener)
    {
        return false;
    }

    return writeControlSignal(
        NDAC_MASK,
        true);
}

bool GPIB::releaseNDAC()
{
    if (m_mode != Mode::Listener)
    {
        return false;
    }

    return writeControlSignal(
        NDAC_MASK,
        false);
}

bool GPIB::isNDACAsserted()
{
    bool asserted = false;

    if (!readControlSignal(
            NDAC_MASK,
            asserted))
    {
        return false;
    }

    return asserted;
}

bool GPIB::readNDAC(
    bool& asserted)
{
    return readControlSignal(
        NDAC_MASK,
        asserted);
}

/******************************************************************************
 *
 *  ATN
 *
 ******************************************************************************/

bool GPIB::assertATN()
{
    if (!m_initialized)
    {
        return false;
    }

    //
    // In Listener mode EOI direction changes when ATN changes.
    // Isolate the interface while making the transition.
    //

    if (m_mode == Mode::Listener)
    {
        disableBuffers();
    }

    if (!writeControlSignal(
            ATN_MASK,
            true))
    {
        return false;
    }

    m_atnAsserted = true;

    if (m_mode == Mode::Listener)
    {
        if (!updateEOIDirection())
        {
            return false;
        }

        enableBuffers();
    }

    return true;
}

bool GPIB::releaseATN()
{
    if (!m_initialized)
    {
        return false;
    }

    if (m_mode == Mode::Listener)
    {
        disableBuffers();
    }

    if (!writeControlSignal(
            ATN_MASK,
            false))
    {
        return false;
    }

    m_atnAsserted = false;

    if (m_mode == Mode::Listener)
    {
        if (!updateEOIDirection())
        {
            return false;
        }

        enableBuffers();
    }

    return true;
}

bool GPIB::isATNAsserted()
{
    bool asserted = false;

    if (!readControlSignal(
            ATN_MASK,
            asserted))
    {
        return false;
    }

    return asserted;
}

/******************************************************************************
 *
 *  IFC
 *
 ******************************************************************************/

bool GPIB::assertIFC()
{
    return writeControlSignal(
        IFC_MASK,
        true);
}

bool GPIB::releaseIFC()
{
    return writeControlSignal(
        IFC_MASK,
        false);
}

bool GPIB::isIFCAsserted()
{
    bool asserted = false;

    if (!readControlSignal(
            IFC_MASK,
            asserted))
    {
        return false;
    }

    return asserted;
}

/******************************************************************************
 *
 *  REN
 *
 ******************************************************************************/

bool GPIB::assertREN()
{
    return writeControlSignal(
        REN_MASK,
        true);
}

bool GPIB::releaseREN()
{
    return writeControlSignal(
        REN_MASK,
        false);
}

bool GPIB::isRENAsserted()
{
    bool asserted = false;

    if (!readControlSignal(
            REN_MASK,
            asserted))
    {
        return false;
    }

    return asserted;
}

/******************************************************************************
 *
 *  SRQ
 *
 ******************************************************************************/

bool GPIB::isSRQAsserted()
{
    bool asserted = false;

    if (!readControlSignal(
            SRQ_MASK,
            asserted))
    {
        return false;
    }

    return asserted;
}

/******************************************************************************
 *
 *  EOI
 *
 ******************************************************************************/

bool GPIB::assertEOI()
{
    //
    // EOI may only be driven when the interface hardware is
    // configured to transmit EOI.
    //

    if (m_mode == Mode::Listener &&
        !m_atnAsserted)
    {
        return false;
    }

    return writeControlSignal(
        EOI_MASK,
        true);
}

bool GPIB::releaseEOI()
{
    if (m_mode == Mode::Listener &&
        !m_atnAsserted)
    {
        return false;
    }

    return writeControlSignal(
        EOI_MASK,
        false);
}

bool GPIB::isEOIAsserted()
{
    bool asserted = false;

    if (!readControlSignal(
            EOI_MASK,
            asserted))
    {
        return false;
    }

    return asserted;
}

/******************************************************************************
 *
 *  disableBuffers()
 *
 ******************************************************************************/

void GPIB::disableBuffers()
{
    digitalWrite(
        Configuration::Pins::GPIB::BufferEnable,
        HIGH);
}

/******************************************************************************
 *
 *  enableBuffers()
 *
 ******************************************************************************/

void GPIB::enableBuffers()
{
    digitalWrite(
        Configuration::Pins::GPIB::BufferEnable,
        LOW);
}

}   // namespace AK3Y