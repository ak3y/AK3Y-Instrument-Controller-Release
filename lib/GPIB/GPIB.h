/******************************************************************************
 * File: GPIB.h
 * Project: AK3Y Instrument Controller
 * Description: Declares low-level GPIB electrical-interface services.
 *
 * Responsibilities:
 *   - Control and monitor IEEE-488 bus signals and the eight-bit data bus.
 *   - Set transceiver and buffer direction for talker/listener operation.
 *   - Encapsulate MCP23S17 and interface-hardware details.
 *
 * IEEE-488 handshaking, addressing, SCPI, and instrument-specific commands
 * belong to higher layers. This interface does not access SPI directly.
 *
 * Author: Bob Fontana, AK3Y
 ******************************************************************************/

#pragma once

#include <stdint.h>

namespace AK3Y
{

class MCP23S17;
class Console;

class GPIB
{
public:
    enum class Mode
    {
        Listener,
        Talker
    };

    GPIB(
        MCP23S17& mcp23s17,
        Console& console);

    bool initialize();
    bool runDiagnostics();

    bool setMode(
        Mode mode);

    Mode mode() const;

    bool writeData(
        uint8_t data);

    bool readData(
        uint8_t& data);

    bool assertDAV();
    bool releaseDAV();
    bool isDAVAsserted();

    bool assertNRFD();
    bool releaseNRFD();
    bool isNRFDAsserted();
    bool readNRFD(bool& asserted);

    bool assertNDAC();
    bool releaseNDAC();
    bool isNDACAsserted();
    bool readNDAC(bool& asserted);

    bool assertATN();
    bool releaseATN();
    bool isATNAsserted();

    bool assertIFC();
    bool releaseIFC();
    bool isIFCAsserted();

    bool assertREN();
    bool releaseREN();
    bool isRENAsserted();

    bool isSRQAsserted();

    bool assertEOI();
    bool releaseEOI();
    bool isEOIAsserted();

    /**************************************************************************
     *
     *  readControlPortDiagnostic()
     *
     *  Returns a diagnostic snapshot of MCP23S17 Port A:
     *
     *      direction   = IODIRA
     *      outputLatch = OLATA
     *      pinLevels   = GPIOA
     *
     *  This preserves the GPIB abstraction by keeping MCP23S17 register
     *  addresses private to the MCP23S17 driver.
     *
     **************************************************************************/

    bool readControlPortDiagnostic(
        uint8_t& direction,
        uint8_t& outputLatch,
        uint8_t& pinLevels);

private:
    bool configureListener();
    bool configureTalker();
    bool updateEOIDirection();

    bool writeControlSignal(
        uint8_t mask,
        bool asserted);

    bool readControlSignal(
        uint8_t mask,
        bool& asserted);

    bool writeControlPort();

    void disableBuffers();
    void enableBuffers();

private:
    MCP23S17& m_mcp23s17;
    Console&  m_console;

    Mode m_mode;
    uint8_t m_controlOutputState;
    bool m_atnAsserted;
    bool m_initialized;
};

}   // namespace AK3Y
