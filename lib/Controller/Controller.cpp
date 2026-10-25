/******************************************************************************
 * File: Controller.cpp
 * Project: AK3Y Instrument Controller
 * Purpose: Console-oriented controller
 * Description: Implements the controller command interface and coordinates GPIB diagnostics and script execution.
 * Note: This file header describes responsibilities; release history is in Git.
 ******************************************************************************/

#include "Controller.h"

#include <ctype.h>
#include <string.h>

namespace AK3Y
{

namespace
{

/******************************************************************************
 *
 *  IEEE-488 Connector Data-Line Mapping
 *
 *      DIO1    J1 pin 1
 *      DIO2    J1 pin 2
 *      DIO3    J1 pin 3
 *      DIO4    J1 pin 4
 *      DIO5    J1 pin 13
 *      DIO6    J1 pin 14
 *      DIO7    J1 pin 15
 *      DIO8    J1 pin 16
 *
 ******************************************************************************/

constexpr uint8_t J1DataPins[8] =
{
    1,
    2,
    3,
    4,
    13,
    14,
    15,
    16
};

}   // anonymous namespace

/******************************************************************************
 *
 *  Constructor
 *
 ******************************************************************************/

Controller::Controller(
    Console& console,
    GPIB& gpib,
    IEEE488& ieee488,
    ScriptExecutor& scriptExecutor)
    :
    m_console(console),
    m_gpib(gpib),
    m_ieee488(ieee488),
    m_testInstrument(ieee488, 20),
    m_scriptExecutor(scriptExecutor),
    m_initialized(false),
    m_commandBuffer{},
    m_commandLength(0),
    m_ignoreNextLF(false),
    m_gpibTestActive(false),
    m_gpibTestStep(0),
    m_gpibControlTestActive(false),
    m_gpibControlTestStep(0),
    m_gpibHandshakeTestActive(false),
    m_gpibHandshakeTestStep(0),
    m_gpibEOITestActive(false),
    m_gpibEOITestStep(0),
    m_gpibSRQTestActive(false),
    m_gpibSRQTestStep(0),
    m_ieee488ByteTestActive(false),
    m_ieee488ByteTestStep(0)
{
}

/******************************************************************************
 *
 *  initialize()
 *
 ******************************************************************************/

bool Controller::initialize()
{
    m_commandLength = 0;
    m_commandBuffer[0] = '\0';

    m_ignoreNextLF = false;

    m_gpibTestActive = false;
    m_gpibTestStep = 0;

    m_gpibControlTestActive = false;
    m_gpibControlTestStep = 0;

    m_gpibHandshakeTestActive = false;
    m_gpibHandshakeTestStep = 0;

    m_gpibEOITestActive = false;
    m_gpibEOITestStep = 0;

    m_gpibSRQTestActive = false;
    m_gpibSRQTestStep = 0;

    m_ieee488ByteTestActive = false;
    m_ieee488ByteTestStep = 0;

    m_initialized = true;

    return true;
}

/******************************************************************************
 *
 *  update()
 *
 ******************************************************************************/

void Controller::update()
{
    if (!m_initialized)
    {
        return;
    }

    processConsole();
}

/******************************************************************************
 *
 *  processConsole()
 *
 ******************************************************************************/

void Controller::processConsole()
{
    while (m_console.available())
    {
        int value = m_console.read();

        if (value < 0)
        {
            return;
        }

        char character =
            static_cast<char>(value);

        if (character == '\n' &&
            m_ignoreNextLF)
        {
            m_ignoreNextLF = false;
            continue;
        }

        if (character == '\r' ||
            character == '\n')
        {
            m_ignoreNextLF =
                (character == '\r');

            if (m_gpibTestActive)
            {
                advanceGPIBTest();
            }
            else if (m_gpibControlTestActive)
            {
                advanceGPIBControlTest();
            }
            else if (m_gpibHandshakeTestActive)
            {
                advanceGPIBHandshakeTest();
            }
            else if (m_gpibEOITestActive)
            {
                advanceGPIBEOITest();
            }
            else if (m_gpibSRQTestActive)
            {
                advanceGPIBSRQTest();
            }
            else if (m_ieee488ByteTestActive)
            {
                advanceIEEE488ByteTest();
            }
            else
            {
                processCommand();
            }

            continue;
        }

        m_ignoreNextLF = false;

        if (m_gpibTestActive ||
            m_gpibControlTestActive ||
            m_gpibHandshakeTestActive ||
            m_gpibEOITestActive ||
            m_gpibSRQTestActive ||
            m_ieee488ByteTestActive)
        {
            continue;
        }

        if (character == '\b' ||
            character == 0x7F)
        {
            if (m_commandLength > 0)
            {
                --m_commandLength;
                m_commandBuffer[m_commandLength] = '\0';
            }

            continue;
        }

        if (isprint(
                static_cast<unsigned char>(character)))
        {
            if (m_commandLength <
                (CommandBufferSize - 1))
            {
                m_commandBuffer[m_commandLength] =
                    character;

                ++m_commandLength;

                m_commandBuffer[m_commandLength] =
                    '\0';
            }
        }
    }
}

/******************************************************************************
 *
 *  processCommand()
 *
 ******************************************************************************/

void Controller::processCommand()
{
    if (m_commandLength == 0)
    {
        return;
    }

    if (commandEqualsIgnoreCase(
            "GPIB Test"))
    {
        startGPIBTest();
    }
    else if (commandEqualsIgnoreCase(
                 "GPIB Control Test"))
    {
        startGPIBControlTest();
    }
    else if (commandEqualsIgnoreCase(
                 "GPIB Handshake Test"))
    {
        startGPIBHandshakeTest();
    }
    else if (commandEqualsIgnoreCase(
                 "GPIB EOI Test"))
    {
        startGPIBEOITest();
    }
    else if (commandEqualsIgnoreCase(
                 "GPIB SRQ Test"))
    {
        startGPIBSRQTest();
    }
    else if (commandEqualsIgnoreCase(
                 "IEEE488 Byte Test"))
    {
        startIEEE488ByteTest();
    }
    else if (commandEqualsIgnoreCase(
                 "HELP"))
    {
        m_console.println();
        m_console.println("AK3Y Instrument Controller Commands");
        m_console.println("----------------------------------------");
        m_console.println("HELP");
        m_console.println("  Show this command summary.");
        m_console.println();
        m_console.println("SCRIPTS");
        m_console.println("  List files in /scripts on the SD card.");
        m_console.println();
        m_console.println("RUN <script-path>");
        m_console.println("  Execute an SD-card script.");
        m_console.println("  Example: RUN /scripts/test.txt");
        m_console.println();
        m_console.println("Diagnostic commands:");
        m_console.println("  GPIB Test");
        m_console.println("  GPIB Control Test");
        m_console.println("  GPIB Handshake Test");
        m_console.println("  GPIB EOI Test");
        m_console.println("  GPIB SRQ Test");
        m_console.println("  IEEE488 Byte Test");
        m_console.println("----------------------------------------");
    }
    else if (commandEqualsIgnoreCase(
                 "SCRIPTS"))
    {
        m_console.println();

        if (!m_scriptExecutor.listScripts())
        {
            m_console.println(
                "Unable to open /scripts directory.");
        }
    }
    else if (m_commandLength > 4 &&
             tolower(static_cast<unsigned char>(m_commandBuffer[0])) == 'r' &&
             tolower(static_cast<unsigned char>(m_commandBuffer[1])) == 'u' &&
             tolower(static_cast<unsigned char>(m_commandBuffer[2])) == 'n' &&
             m_commandBuffer[3] == ' ')
    {
        const char* scriptPath = m_commandBuffer + 4;

        m_console.print("Running script: ");
        m_console.println(scriptPath);

        const ScriptExecutor::Result result =
            m_scriptExecutor.execute(
                scriptPath,
                5000);

        m_console.print("Script result: ");
        m_console.println(
            ScriptExecutor::resultText(result));
    }
    else
    {
        m_console.print("Unknown command: ");
        m_console.println(m_commandBuffer);
    }

    m_commandLength = 0;
    m_commandBuffer[0] = '\0';
}

/******************************************************************************
 *
 *  commandEqualsIgnoreCase()
 *
 ******************************************************************************/

bool Controller::commandEqualsIgnoreCase(
    const char* command) const
{
    if (command == nullptr)
    {
        return false;
    }

    size_t commandLength =
        strlen(command);

    if (commandLength != m_commandLength)
    {
        return false;
    }

    for (size_t index = 0;
         index < commandLength;
         ++index)
    {
        unsigned char left =
            static_cast<unsigned char>(
                m_commandBuffer[index]);

        unsigned char right =
            static_cast<unsigned char>(
                command[index]);

        if (tolower(left) !=
            tolower(right))
        {
            return false;
        }
    }

    return true;
}

/******************************************************************************
 *
 *  printGPIBControlPortDiagnostic()
 *
 *  Prints the MCP23S17 Port A direction register, output latch, and
 *  actual GPIO pin levels.
 *
 ******************************************************************************/

void Controller::printGPIBControlPortDiagnostic()
{
    uint8_t direction;
    uint8_t outputLatch;
    uint8_t pinLevels;

    if (!m_gpib.readControlPortDiagnostic(
            direction,
            outputLatch,
            pinLevels))
    {
        m_console.println(
            "ERROR: Unable to read MCP23S17 Port A diagnostic state.");

        return;
    }

    m_console.println();
    m_console.println(
        "MCP23S17 Port A Diagnostic");

    m_console.printf(
        "  IODIRA = 0x%02X\n",
        static_cast<unsigned int>(direction));

    m_console.printf(
        "  OLATA  = 0x%02X\n",
        static_cast<unsigned int>(outputLatch));

    m_console.printf(
        "  GPIOA  = 0x%02X\n",
        static_cast<unsigned int>(pinLevels));
}

/******************************************************************************
 *
 *  GPIB DATA-LINE TEST
 *
 ******************************************************************************/

void Controller::startGPIBTest()
{
    m_console.println();
    m_console.line();
    m_console.println("GPIB Data-Line Test");
    m_console.line();

    m_console.println();
    m_console.println(
        "WARNING: J1 must be disconnected from all instruments.");
    m_console.println();

    if (!m_gpib.setMode(
            GPIB::Mode::Listener))
    {
        m_console.println(
            "ERROR: Unable to establish GPIB Listener mode.");

        m_console.line();
        return;
    }

    m_gpibTestActive = true;
    m_gpibTestStep = 0;

    m_console.println(
        "Press ENTER to begin.");
}

void Controller::advanceGPIBTest()
{
    if (!m_gpibTestActive)
    {
        return;
    }

    if (m_gpibTestStep == 0)
    {
        if (!m_gpib.setMode(
                GPIB::Mode::Talker))
        {
            m_console.println();
            m_console.println(
                "ERROR: Unable to enter GPIB Talker mode.");

            finishGPIBTest();
            return;
        }
    }

    if (m_gpibTestStep < 8)
    {
        uint8_t logicalData =
            static_cast<uint8_t>(
                1U << m_gpibTestStep);

        if (!m_gpib.writeData(
                logicalData))
        {
            m_console.println();
            m_console.println(
                "ERROR: Unable to write GPIB data.");

            finishGPIBTest();
            return;
        }

        uint8_t dataLineIndex =
            m_gpibTestStep;

        ++m_gpibTestStep;

        m_console.println();

        m_console.printf(
            "DIO%u asserted - J1 pin %u - logical data 0x%02X\n",
            static_cast<unsigned int>(
                dataLineIndex + 1),
            static_cast<unsigned int>(
                J1DataPins[dataLineIndex]),
            static_cast<unsigned int>(
                logicalData));

        m_console.println(
            "Press ENTER for next line.");

        return;
    }

    finishGPIBTest();
}

void Controller::finishGPIBTest()
{
    bool dataReleased =
        m_gpib.writeData(0x00);

    bool listenerMode =
        m_gpib.setMode(
            GPIB::Mode::Listener);

    m_gpibTestActive = false;
    m_gpibTestStep = 0;

    m_console.println();

    if (!dataReleased)
    {
        m_console.println(
            "WARNING: Unable to verify data-line release.");
    }

    if (!listenerMode)
    {
        m_console.println(
            "ERROR: Unable to return to GPIB Listener mode.");
    }
    else
    {
        m_console.println(
            "GPIB returned to Listener mode.");
    }

    m_console.println(
        "GPIB Test Complete.");

    m_console.line();
}

/******************************************************************************
 *
 *  GPIB CONTROL-LINE TEST
 *
 *      ATN     J1 pin 11
 *      IFC     J1 pin 9
 *      REN     J1 pin 17
 *
 ******************************************************************************/

void Controller::startGPIBControlTest()
{
    m_console.println();
    m_console.line();
    m_console.println("GPIB Control-Line Test");
    m_console.line();

    m_console.println();
    m_console.println(
        "WARNING: J1 must be disconnected from all instruments.");
    m_console.println();

    if (!m_gpib.setMode(
            GPIB::Mode::Listener))
    {
        m_console.println(
            "ERROR: Unable to establish GPIB Listener mode.");

        m_console.line();
        return;
    }

    bool success = true;

    success =
        m_gpib.releaseATN() &&
        success;

    success =
        m_gpib.releaseIFC() &&
        success;

    success =
        m_gpib.releaseREN() &&
        success;

    if (!success)
    {
        m_console.println(
            "ERROR: Unable to release GPIB control lines.");

        m_console.line();
        return;
    }

    m_gpibControlTestActive = true;
    m_gpibControlTestStep = 0;

    m_console.println(
        "ATN, IFC and REN are released.");

    m_console.println(
        "Press ENTER to assert ATN.");
}

void Controller::advanceGPIBControlTest()
{
    if (!m_gpibControlTestActive)
    {
        return;
    }

    bool success = true;

    switch (m_gpibControlTestStep)
    {
        case 0:
        {
            success =
                m_gpib.releaseIFC() &&
                success;

            success =
                m_gpib.releaseREN() &&
                success;

            success =
                m_gpib.assertATN() &&
                success;

            if (!success)
            {
                m_console.println();
                m_console.println(
                    "ERROR: Unable to assert ATN.");

                finishGPIBControlTest();
                return;
            }

            ++m_gpibControlTestStep;

            m_console.println();
            m_console.println(
                "ATN asserted - J1 pin 11 should be LOW.");

            m_console.println(
                "IFC J1 pin 9 and REN J1 pin 17 should be released.");

            m_console.println(
                "Press ENTER to assert IFC.");

            return;
        }

        case 1:
        {
            success =
                m_gpib.releaseATN() &&
                success;

            success =
                m_gpib.releaseREN() &&
                success;

            success =
                m_gpib.assertIFC() &&
                success;

            if (!success)
            {
                m_console.println();
                m_console.println(
                    "ERROR: Unable to assert IFC.");

                finishGPIBControlTest();
                return;
            }

            ++m_gpibControlTestStep;

            m_console.println();
            m_console.println(
                "IFC asserted - J1 pin 9 should be LOW.");

            m_console.println(
                "ATN J1 pin 11 and REN J1 pin 17 should be released.");

            m_console.println(
                "Press ENTER to assert REN.");

            return;
        }

        case 2:
        {
            success =
                m_gpib.releaseATN() &&
                success;

            success =
                m_gpib.releaseIFC() &&
                success;

            success =
                m_gpib.assertREN() &&
                success;

            if (!success)
            {
                m_console.println();
                m_console.println(
                    "ERROR: Unable to assert REN.");

                finishGPIBControlTest();
                return;
            }

            ++m_gpibControlTestStep;

            m_console.println();
            m_console.println(
                "REN asserted - J1 pin 17 should be LOW.");

            m_console.println(
                "ATN J1 pin 11 and IFC J1 pin 9 should be released.");

            m_console.println(
                "Press ENTER to finish.");

            return;
        }

        default:
        {
            finishGPIBControlTest();
            return;
        }
    }
}

void Controller::finishGPIBControlTest()
{
    bool success = true;

    success =
        m_gpib.releaseATN() &&
        success;

    success =
        m_gpib.releaseIFC() &&
        success;

    success =
        m_gpib.releaseREN() &&
        success;

    bool listenerMode =
        m_gpib.setMode(
            GPIB::Mode::Listener);

    m_gpibControlTestActive = false;
    m_gpibControlTestStep = 0;

    m_console.println();

    if (!success)
    {
        m_console.println(
            "WARNING: Unable to release all GPIB control lines.");
    }

    if (!listenerMode)
    {
        m_console.println(
            "ERROR: Unable to restore GPIB Listener mode.");
    }
    else
    {
        m_console.println(
            "ATN, IFC and REN released.");
    }

    m_console.println(
        "GPIB Control-Line Test Complete.");

    m_console.line();
}

/******************************************************************************
 *
 *  GPIB THREE-WIRE HANDSHAKE TEST
 *
 *  Talker:
 *
 *      DAV     Output      J1 pin 6
 *      NRFD    Input       J1 pin 7
 *      NDAC    Input       J1 pin 8
 *
 *  Listener:
 *
 *      DAV     Input       J1 pin 6
 *      NRFD    Output      J1 pin 7
 *      NDAC    Output      J1 pin 8
 *
 ******************************************************************************/

void Controller::startGPIBHandshakeTest()
{
    m_console.println();
    m_console.line();
    m_console.println("GPIB Handshake Test");
    m_console.line();

    m_console.println();
    m_console.println(
        "WARNING: J1 must be disconnected from all instruments.");
    m_console.println();

    //
    // Establish Listener mode first.
    //

    if (!m_gpib.setMode(
            GPIB::Mode::Listener))
    {
        m_console.println(
            "ERROR: Unable to establish GPIB Listener mode.");

        m_console.line();
        return;
    }

    //
    // NRFD and NDAC are outputs in Listener mode.
    // Begin with both released.
    //

    bool success = true;

    success =
        m_gpib.releaseNRFD() &&
        success;

    success =
        m_gpib.releaseNDAC() &&
        success;

    if (!success)
    {
        m_console.println(
            "ERROR: Unable to establish released handshake state.");

        m_console.line();
        return;
    }

    m_gpibHandshakeTestActive = true;
    m_gpibHandshakeTestStep = 0;

    m_console.println(
        "Handshake lines initialized.");

    m_console.println(
        "Press ENTER to test DAV in Talker mode.");
}

/******************************************************************************
 *
 *  advanceGPIBHandshakeTest()
 *
 ******************************************************************************/

void Controller::advanceGPIBHandshakeTest()
{
    if (!m_gpibHandshakeTestActive)
    {
        return;
    }

    switch (m_gpibHandshakeTestStep)
    {
        //
        // ENTER 1:
        //
        // Change to Talker mode and assert DAV.
        //

        case 0:
        {
            if (!m_gpib.setMode(
                    GPIB::Mode::Talker))
            {
                m_console.println();
                m_console.println(
                    "ERROR: Unable to enter GPIB Talker mode.");

                finishGPIBHandshakeTest();
                return;
            }

            //
            // DAV is now an output.
            //

            if (!m_gpib.releaseDAV())
            {
                m_console.println();
                m_console.println(
                    "ERROR: Unable to release DAV.");

                finishGPIBHandshakeTest();
                return;
            }

            if (!m_gpib.assertDAV())
            {
                m_console.println();
                m_console.println(
                    "ERROR: Unable to assert DAV.");

                finishGPIBHandshakeTest();
                return;
            }

            ++m_gpibHandshakeTestStep;

            m_console.println();
            m_console.println(
                "Talker mode.");

            m_console.println(
                "DAV asserted - J1 pin 6 should be LOW.");

            printGPIBControlPortDiagnostic();

            m_console.println(
                "Press ENTER to test NRFD in Listener mode.");

            return;
        }

        //
        // ENTER 2:
        //
        // Release DAV while it is still an output.
        // Then change to Listener mode and assert NRFD.
        //

        case 1:
        {
            if (!m_gpib.releaseDAV())
            {
                m_console.println();
                m_console.println(
                    "ERROR: Unable to release DAV.");

                finishGPIBHandshakeTest();
                return;
            }

            if (!m_gpib.setMode(
                    GPIB::Mode::Listener))
            {
                m_console.println();
                m_console.println(
                    "ERROR: Unable to enter GPIB Listener mode.");

                finishGPIBHandshakeTest();
                return;
            }

            //
            // NRFD and NDAC are now outputs.
            // Release both before selecting NRFD.
            //

            bool success = true;

            success =
                m_gpib.releaseNRFD() &&
                success;

            success =
                m_gpib.releaseNDAC() &&
                success;

            success =
                m_gpib.assertNRFD() &&
                success;

            if (!success)
            {
                m_console.println();
                m_console.println(
                    "ERROR: Unable to assert NRFD.");

                finishGPIBHandshakeTest();
                return;
            }

            ++m_gpibHandshakeTestStep;

            m_console.println();
            m_console.println(
                "Listener mode.");

            m_console.println(
                "NRFD asserted - J1 pin 7 should be LOW.");

            m_console.println(
                "NDAC J1 pin 8 should be released.");

            printGPIBControlPortDiagnostic();

            m_console.println(
                "Press ENTER to test NDAC.");

            return;
        }

        //
        // ENTER 3:
        //
        // Remain in Listener mode.
        // Release NRFD and assert NDAC.
        //

        case 2:
        {
            bool success = true;

            success =
                m_gpib.releaseNRFD() &&
                success;

            success =
                m_gpib.releaseNDAC() &&
                success;

            success =
                m_gpib.assertNDAC() &&
                success;

            if (!success)
            {
                m_console.println();
                m_console.println(
                    "ERROR: Unable to assert NDAC.");

                finishGPIBHandshakeTest();
                return;
            }

            ++m_gpibHandshakeTestStep;

            m_console.println();
            m_console.println(
                "Listener mode.");

            m_console.println(
                "NDAC asserted - J1 pin 8 should be LOW.");

            m_console.println(
                "NRFD J1 pin 7 should be released.");

            printGPIBControlPortDiagnostic();

            m_console.println(
                "Press ENTER to finish.");

            return;
        }

        //
        // ENTER 4:
        //
        // Release handshake outputs and finish.
        //

        default:
        {
            finishGPIBHandshakeTest();
            return;
        }
    }
}

/******************************************************************************
 *
 *  finishGPIBHandshakeTest()
 *
 ******************************************************************************/

void Controller::finishGPIBHandshakeTest()
{
    bool success = true;

    //
    // Release whichever handshake output is currently under our
    // control before restoring the normal Listener state.
    //

    if (m_gpib.mode() == GPIB::Mode::Talker)
    {
        success =
            m_gpib.releaseDAV() &&
            success;

        if (!m_gpib.setMode(
                GPIB::Mode::Listener))
        {
            success = false;
        }
    }

    //
    // In Listener mode NRFD and NDAC are outputs.
    //

    if (m_gpib.mode() == GPIB::Mode::Listener)
    {
        success =
            m_gpib.releaseNRFD() &&
            success;

        success =
            m_gpib.releaseNDAC() &&
            success;
    }

    m_gpibHandshakeTestActive = false;
    m_gpibHandshakeTestStep = 0;

    m_console.println();

    if (!success)
    {
        m_console.println(
            "WARNING: Unable to restore all handshake lines.");
    }
    else
    {
        m_console.println(
            "Handshake lines released.");
    }

    m_console.println(
        "GPIB returned to Listener mode.");

    m_console.println(
        "GPIB Handshake Test Complete.");

    m_console.line();
}


/******************************************************************************
 *  GPIB EOI TEST
 ******************************************************************************/
void Controller::startGPIBEOITest()
{
    m_console.println(); m_console.line(); m_console.println("GPIB EOI Test"); m_console.line();
    m_console.println(); m_console.println("WARNING: J1 must be disconnected from all instruments."); m_console.println();
    if (!m_gpib.setMode(GPIB::Mode::Listener) || !m_gpib.releaseATN()) { m_console.println("ERROR: Unable to establish Listener mode with ATN released."); m_console.line(); return; }
    m_gpibEOITestActive = true; m_gpibEOITestStep = 0;
    m_console.println("Listener mode, ATN released. EOI is an input."); printGPIBControlPortDiagnostic();
    m_console.println("Press ENTER to test EOI in Talker mode.");
}

void Controller::advanceGPIBEOITest()
{
    if (!m_gpibEOITestActive) return;
    switch (m_gpibEOITestStep)
    {
        case 0:
            if (!m_gpib.setMode(GPIB::Mode::Talker) || !m_gpib.releaseEOI() || !m_gpib.assertEOI()) { m_console.println(); m_console.println("ERROR: Unable to assert EOI in Talker mode."); finishGPIBEOITest(); return; }
            ++m_gpibEOITestStep; m_console.println(); m_console.println("Talker mode, ATN released."); m_console.println("EOI asserted - J1 pin 5 should be LOW."); printGPIBControlPortDiagnostic(); m_console.println("Press ENTER to test EOI as a Listener input."); return;
        case 1:
            if (!m_gpib.releaseEOI() || !m_gpib.setMode(GPIB::Mode::Listener) || !m_gpib.releaseATN()) { m_console.println(); m_console.println("ERROR: Unable to establish Listener EOI-input state."); finishGPIBEOITest(); return; }
            ++m_gpibEOITestStep; m_console.println(); m_console.println("Listener mode, ATN released."); m_console.println("EOI is an input - J1 pin 5 should be released HIGH."); printGPIBControlPortDiagnostic(); m_console.println("Press ENTER to assert ATN and test EOI as an output."); return;
        case 2:
            if (!m_gpib.assertATN() || !m_gpib.releaseEOI() || !m_gpib.assertEOI()) { m_console.println(); m_console.println("ERROR: Unable to assert EOI with ATN asserted."); finishGPIBEOITest(); return; }
            ++m_gpibEOITestStep; m_console.println(); m_console.println("Listener mode, ATN asserted."); m_console.println("EOI asserted - J1 pin 5 should be LOW."); m_console.println("ATN J1 pin 11 should also be LOW."); printGPIBControlPortDiagnostic(); m_console.println("Press ENTER to finish."); return;
        default: finishGPIBEOITest(); return;
    }
}

void Controller::finishGPIBEOITest()
{
    bool success = true;
    if (m_gpib.mode() == GPIB::Mode::Talker) { success = m_gpib.releaseEOI() && success; success = m_gpib.setMode(GPIB::Mode::Listener) && success; }
    else { success = m_gpib.releaseEOI() && success; }
    success = m_gpib.releaseATN() && success;
    m_gpibEOITestActive = false; m_gpibEOITestStep = 0;
    m_console.println();
    if (!success) m_console.println("WARNING: Unable to restore the normal EOI state.");
    else m_console.println("EOI released; ATN released; Listener mode restored.");
    m_console.println("GPIB EOI Test Complete."); m_console.line();
}


/******************************************************************************
 *
 *  GPIB SRQ TEST
 *
 *  SRQ is an input to the controller:
 *
 *      SRQ     J1 pin 10     MCP23S17 GPA1
 *
 *  The test verifies the complete receive path by observing SRQ first
 *  released, then externally asserted by grounding J1 pin 10, and finally
 *  released again.
 *
 ******************************************************************************/

void Controller::startGPIBSRQTest()
{
    m_console.println();
    m_console.line();
    m_console.println("GPIB SRQ Test");
    m_console.line();

    m_console.println();
    m_console.println(
        "WARNING: J1 must be disconnected from all instruments.");
    m_console.println();

    if (!m_gpib.setMode(
            GPIB::Mode::Listener) ||
        !m_gpib.releaseATN())
    {
        m_console.println(
            "ERROR: Unable to establish Listener mode with ATN released.");

        m_console.line();
        return;
    }

    bool asserted = false;

    if (!m_gpib.isSRQAsserted())
    {
        //
        // isSRQAsserted() returns the logical SRQ state directly.
        // A false result is the expected initial released state.
        //
    }
    else
    {
        asserted = true;
    }

    m_gpibSRQTestActive = true;
    m_gpibSRQTestStep = 0;

    m_console.println(
        "Listener mode, ATN released. SRQ is an input.");

    printGPIBControlPortDiagnostic();

    m_console.println();

    if (asserted)
    {
        m_console.println(
            "WARNING: SRQ is already reported ASSERTED.");
        m_console.println(
            "Verify that J1 pin 10 is not connected to ground.");
    }
    else
    {
        m_console.println(
            "SRQ is reported RELEASED.");
    }

    m_console.println();
    m_console.println(
        "Temporarily connect J1 pin 10 (SRQ) to GND.");
    m_console.println(
        "Leave that connection in place, then press ENTER.");
}

/******************************************************************************
 *
 *  advanceGPIBSRQTest()
 *
 ******************************************************************************/

void Controller::advanceGPIBSRQTest()
{
    if (!m_gpibSRQTestActive)
    {
        return;
    }

    switch (m_gpibSRQTestStep)
    {
        //
        // ENTER 1:
        // J1 pin 10 should be externally grounded.
        //

        case 0:
        {
            bool asserted =
                m_gpib.isSRQAsserted();

            ++m_gpibSRQTestStep;

            m_console.println();
            m_console.println(
                "Reading SRQ with J1 pin 10 grounded:");

            printGPIBControlPortDiagnostic();

            m_console.println();

            if (asserted)
            {
                m_console.println(
                    "PASS: SRQ is reported ASSERTED.");
            }
            else
            {
                m_console.println(
                    "FAIL: SRQ is still reported RELEASED.");
            }

            m_console.println();
            m_console.println(
                "Now remove the connection between J1 pin 10 and GND.");
            m_console.println(
                "Then press ENTER to verify SRQ release.");

            return;
        }

        //
        // ENTER 2:
        // Ground has been removed; SRQ should again be released.
        //

        case 1:
        {
            bool asserted =
                m_gpib.isSRQAsserted();

            ++m_gpibSRQTestStep;

            m_console.println();
            m_console.println(
                "Reading SRQ after removing the ground:");

            printGPIBControlPortDiagnostic();

            m_console.println();

            if (!asserted)
            {
                m_console.println(
                    "PASS: SRQ is reported RELEASED.");
            }
            else
            {
                m_console.println(
                    "FAIL: SRQ is still reported ASSERTED.");
            }

            m_console.println(
                "Press ENTER to finish.");

            return;
        }

        default:
        {
            finishGPIBSRQTest();
            return;
        }
    }
}

/******************************************************************************
 *
 *  finishGPIBSRQTest()
 *
 ******************************************************************************/

void Controller::finishGPIBSRQTest()
{
    bool listenerMode =
        m_gpib.setMode(
            GPIB::Mode::Listener);

    bool atnReleased =
        m_gpib.releaseATN();

    m_gpibSRQTestActive = false;
    m_gpibSRQTestStep = 0;

    m_console.println();

    if (!listenerMode ||
        !atnReleased)
    {
        m_console.println(
            "WARNING: Unable to restore the normal Listener state.");
    }
    else
    {
        m_console.println(
            "Listener mode restored; ATN released.");
    }

    m_console.println(
        "GPIB SRQ Test Complete.");

    m_console.line();
}


/******************************************************************************
 *
 *  INSTRUMENT-ABSTRACTION QUERY TEST
 *
 *
 *  The generic Instrument object is configured for primary address 20.
 *  The Controller supplies the Marconi-specific CFRQ? command only as test
 *  data; Instrument itself contains no model-specific command knowledge.
 *
 ******************************************************************************/

void Controller::startIEEE488ByteTest()
{
    m_console.println();
    m_console.line();
    m_console.println("Instrument-Abstraction Query Test - v0.61(a)");
    m_console.line();

    m_console.println();
    m_console.println(
        "Test instrument: Marconi 2024, GPIB primary address 20.");
    m_console.println(
        "Query: CFRQ?");
    m_console.println(
        "The returned value should agree with the Marconi front-panel frequency.");
    m_console.println();

    m_console.println(
        "This test calls Instrument::query(); Controller does not call");
    m_console.println(
        "IEEE488::query() directly.");
    m_console.println();

    m_ieee488ByteTestActive = true;
    m_ieee488ByteTestStep = 0;

    m_console.println(
        "When ready, press ENTER to perform the Instrument-layer query.");
}

void Controller::advanceIEEE488ByteTest()
{
    if (!m_ieee488ByteTestActive)
    {
        return;
    }

    if (m_ieee488ByteTestStep == 0)
    {
        ++m_ieee488ByteTestStep;

        constexpr uint32_t TransferTimeoutMs = 5000;

        char response[256];
        size_t responseLength = 0;

        m_console.println();
        m_console.printf(
            "Instrument address: %u\n",
            static_cast<unsigned int>(
                m_testInstrument.address()));

        m_console.println(
            "Calling Instrument::query(\"CFRQ?\", ...)...");

        const IEEE488::TransferResult result =
            m_testInstrument.query(
                "CFRQ?",
                response,
                sizeof(response),
                responseLength,
                TransferTimeoutMs);

        m_console.print("Query result: ");
        m_console.println(
            IEEE488::transferResultText(result));

        if (result != IEEE488::TransferResult::Success)
        {
            m_console.println(
                "FAIL: Instrument-layer query did not complete.");

            if (responseLength > 0)
            {
                m_console.print("Partial response: ");
                m_console.println(response);

                m_console.printf(
                    "Partial response length: %u\n",
                    static_cast<unsigned int>(
                        responseLength));
            }

            m_console.println();
            m_console.println(
                "Press ENTER to restore Listener mode and finish.");
            return;
        }

        m_console.println(
            "PASS: Instrument-layer query completed.");

        m_console.printf(
            "Response length: %u\n",
            static_cast<unsigned int>(
                responseLength));

        m_console.print("Instrument response: ");
        m_console.println(response);

        m_console.println();
        m_console.println(
            "Verify that the returned CFRQ value agrees with the Marconi display.");
        m_console.println();
        m_console.println(
            "Press ENTER to restore Listener mode and finish.");

        return;
    }

    finishIEEE488ByteTest();
}

void Controller::finishIEEE488ByteTest()
{
    bool success = true;

    if (m_gpib.mode() == GPIB::Mode::Talker)
    {
        success =
            m_gpib.releaseDAV() &&
            success;

        success =
            m_gpib.releaseEOI() &&
            success;
    }

    success =
        m_gpib.releaseATN() &&
        success;

    success =
        m_gpib.setMode(
            GPIB::Mode::Listener) &&
        success;

    if (m_gpib.mode() == GPIB::Mode::Listener)
    {
        success =
            m_gpib.releaseNRFD() &&
            success;

        success =
            m_gpib.releaseNDAC() &&
            success;
    }

    m_ieee488ByteTestActive = false;
    m_ieee488ByteTestStep = 0;

    m_console.println();

    if (!success)
    {
        m_console.println(
            "WARNING: Unable to restore the normal GPIB Listener state.");
    }
    else
    {
        m_console.println(
            "Listener mode restored; handshake lines, EOI and ATN released.");
    }

    m_console.println(
        "Instrument-Abstraction Query Test Complete.");

    m_console.line();
}

}   // namespace AK3Y