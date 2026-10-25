/******************************************************************************
 * File: Application.cpp
 * Project: AK3Y Instrument Controller
 * Purpose: Top-level application initialization and update coordination.
 * Details: Constructs and connects subsystems, sequences startup, runs diagnostics, and coordinates ongoing controller service.
 * Author: Bob Fontana, AK3Y
 *
 * Documentation only; firmware history is maintained in Git.
 ******************************************************************************/

#include "Application.h"

#include <Arduino.h>
#include <SD.h>

#include <Configuration.h>
#include <InstrumentConfiguration.h>

namespace AK3Y
{

/******************************************************************************
 *
 *  Constructor
 *
 ******************************************************************************/

Application::Application()
    :
    m_startupTimeMs(0),
    m_console(),
    m_timer(),
    m_spiManager(),
    m_mcp23s17(
        m_spiManager,
        Configuration::Pins::SPI::MCP23S17ChipSelect,
        0),
    m_sdCard(
        Configuration::Pins::SPI::SDCardChipSelect),
    m_oled(
        m_console),
    m_configurationFile(
        m_sdCard),
    m_macroConfiguration(
        m_sdCard),
    m_wifiManager(
        m_configurationFile,
        m_console),
    m_gpib(
        m_mcp23s17,
        m_console),
    m_rs232(
        m_console),
    m_tcpInstrument(),
    m_ieee488(
        m_gpib),
    m_activityMonitor(
        m_ieee488,
        m_rs232,
        m_tcpInstrument,
        m_oled),
    m_commandExecutor(
        m_configurationFile,
        m_sdCard,
        m_ieee488,
        m_rs232,
        m_tcpInstrument),
    m_macroExecutor(
        m_macroConfiguration,
        m_commandExecutor,
        m_sdCard,
        m_console),
    m_webServerManager(
        m_wifiManager,
        m_configurationFile,
        m_commandExecutor,
        m_macroConfiguration,
        m_macroExecutor,
        m_activityMonitor,
        m_console),
    m_scriptExecutor(
        m_sdCard,
        m_commandExecutor,
        m_console),
    m_controller(
        m_console,
        m_gpib,
        m_ieee488,
        m_scriptExecutor)
{
}

/******************************************************************************
 *
 *  initializeSubsystem()
 *
 *  Initialize a subsystem, measure its initialization time,
 *  report PASS/FAIL status, and accumulate total startup time.
 *
 ******************************************************************************/

bool Application::initializeSubsystem(
    const char* name,
    Subsystem& subsystem)
{
    uint32_t start = m_timer.milliseconds();

    bool success = subsystem.initialize();

    uint32_t elapsed = m_timer.elapsed(start);

    m_startupTimeMs += elapsed;

    m_console.printInitialization(
        name,
        success,
        elapsed);

    return success;
}

/******************************************************************************
 *
 *  runDiagnostics()
 *
 ******************************************************************************/

bool Application::runDiagnostics()
{
    m_console.println();
    m_console.println("Running Diagnostics...");
    m_console.println();

    if (!m_timer.runDiagnostics(m_console))
    {
        return false;
    }

    if (!m_spiManager.runDiagnostics(m_console))
    {
        return false;
    }

    if (!m_mcp23s17.runDiagnostics(m_console))
    {
        return false;
    }

    if (!m_sdCard.runDiagnostics(m_console))
    {
        return false;
    }

    //
    // Configuration-file parser diagnostic.
    //
    // This test deliberately uses configuration that is not tied to an
    // installed instrument instance name.  Instrument instance sections
    // (RFGEN, GENERATOR, etc.) are user configuration and must not be
    // compiled into Application diagnostics.
    //

    {
        char value[80];

        m_console.println();
        m_console.println("Configuration File Test - v0.63(f)");
        m_console.println("----------------------------------------");

        if (!m_configurationFile.getValue(
                "/config/network.cfg",
                "wifi",
                "hostname",
                value,
                sizeof(value)))
        {
            m_console.println("FAIL: wifi hostname");
            return false;
        }

        m_console.printf("hostname     = %s\n", value);

        if (!m_configurationFile.getValue(
                "/instruments/marconi_2024.cfg",
                "instrument",
                "manufacturer",
                value,
                sizeof(value)))
        {
            m_console.println("FAIL: instrument manufacturer");
            return false;
        }

        m_console.printf("manufacturer = %s\n", value);

        if (!m_configurationFile.getValue(
                "/instruments/marconi_2024.cfg",
                "instrument",
                "model",
                value,
                sizeof(value)))
        {
            m_console.println("FAIL: instrument model");
            return false;
        }

        m_console.printf("model        = %s\n", value);

        if (!m_configurationFile.getValue(
                "/instruments/marconi_2024.cfg",
                "instrument",
                "description",
                value,
                sizeof(value)))
        {
            m_console.println("FAIL: instrument description");
            return false;
        }

        m_console.printf("description  = %s\n", value);

        m_console.println("----------------------------------------");
        m_console.println("PASS: Configuration files read successfully.");
    }

    //
    // GPIB electrical-interface diagnostics.
    //

    {
        uint32_t start = m_timer.milliseconds();

        bool success = m_gpib.runDiagnostics();

        uint32_t elapsed = m_timer.elapsed(start);

        m_console.printDiagnostic(
            "GPIB",
            success,
            elapsed);

        if (!success)
        {
            return false;
        }
    }

    //
    // RS-232 external loopback diagnostics.
    //

    {
        uint32_t start = m_timer.milliseconds();
        bool success = m_rs232.runDiagnostics();
        uint32_t elapsed = m_timer.elapsed(start);

        m_console.printDiagnostic("RS-232", success, elapsed);

        if (!success)
            return false;
    }


    return true;
}

/******************************************************************************
 *
 *  initialize()
 *
 ******************************************************************************/

bool Application::initialize()
{
    //
    // Reset startup diagnostics.
    //

    m_startupTimeMs = 0;

    //
    // Initialize the system console.
    //

    if (!m_console.begin())
    {
        return false;
    }

    //
    // Display startup banner.
    //

    m_console.printBanner();

    m_console.println();
    m_console.println("Initializing...");
    m_console.println();

    //
    // Application
    //

    m_console.printInitialization(
        "Application",
        true,
        0);

    //
    // Timer (Framework Service)
    //

    {
        uint32_t start = m_timer.milliseconds();

        bool success = m_timer.initialize();

        uint32_t elapsed = m_timer.elapsed(start);

        m_startupTimeMs += elapsed;

        m_console.printInitialization(
            "Timer",
            success,
            elapsed);

        if (!success)
        {
            return false;
        }
    }

    //
    // SPI Manager (Framework Service)
    //

    {
        uint32_t start = m_timer.milliseconds();

        bool success = m_spiManager.initialize();

        uint32_t elapsed = m_timer.elapsed(start);

        m_startupTimeMs += elapsed;

        m_console.printInitialization(
            "SPI Manager",
            success,
            elapsed);

        if (!success)
        {
            return false;
        }
    }

    //
    // MCP23S17 (Hardware Device)
    //

    {
        uint32_t start = m_timer.milliseconds();

        bool success = m_mcp23s17.initialize();

        uint32_t elapsed = m_timer.elapsed(start);

        m_startupTimeMs += elapsed;

        m_console.printInitialization(
            "MCP23S17",
            success,
            elapsed);

        if (!success)
        {
            return false;
        }
    }

    //
    // OLED Display (Hardware Device) 
    //
    // Initialize before the SD card so a fatal storage failure can be
    // reported locally to the user.
    //
    {
        uint32_t start = m_timer.milliseconds();
        bool success = m_oled.initialize();
        uint32_t elapsed = m_timer.elapsed(start);
        m_startupTimeMs += elapsed;
        m_console.printInitialization("OLED", success, elapsed);
        if (!success)
            return false;
    }

    //
    // SD Card (Hardware Device)
    //
    // The system SPI peripheral must already be initialized.
    //

    {
        uint32_t start = m_timer.milliseconds();

        bool success = m_sdCard.initialize();

        uint32_t elapsed = m_timer.elapsed(start);

        m_startupTimeMs += elapsed;

        m_console.printInitialization(
            "SD Card",
            success,
            elapsed);

        if (!success)
        {
            m_oled.showStartupError("SD Card Missing\nor Damaged");
            return false;
        }
    }

    //
    // Macro configuration parser bench test 
    //
    // This is deliberately read-only.  It parses and reports the macro
    // definition but does not execute any macro step or instrument command.
    //

    if (Configuration::Diagnostics::RunStartupTests)
    {
        // Macro is deliberately static for this diagnostic.  The complete
        // parsed definition is several kilobytes and must not be allocated
        // on the ESP32 Arduino loopTask stack.
        static MacroConfiguration::Macro macro;

        const MacroConfiguration::Result result =
            m_macroConfiguration.load(
                "/macros/find_signal.cfg",
                macro);

        m_console.println();
        m_console.println("Macro Configuration Test - v0.74(b)");
        m_console.println("----------------------------------------");
        m_console.printf(
            "parse result = %s\n",
            MacroConfiguration::resultText(result));

        if (result != MacroConfiguration::Result::Success)
        {
            m_console.println("FAIL: unable to parse /macros/find_signal.cfg");
            return false;
        }

        m_console.printf("name         = %s\n", macro.name);
        m_console.printf("description  = %s\n", macro.description);
        m_console.printf(
            "parameters   = %u\n",
            static_cast<unsigned int>(macro.parameterCount));

        for (size_t i = 0; i < macro.parameterCount; ++i)
        {
            const MacroConfiguration::Parameter& parameter =
                macro.parameters[i];

            m_console.printf(
                "  parameter %u: %s\n",
                static_cast<unsigned int>(i + 1),
                parameter.name);
            m_console.printf("    type      = %s\n", parameter.type);
            m_console.printf("    label     = %s\n", parameter.label);

            if (parameter.quantity[0] != '\0')
                m_console.printf("    quantity  = %s\n", parameter.quantity);

            if (parameter.baseUnit[0] != '\0')
                m_console.printf("    base_unit = %s\n", parameter.baseUnit);

            if (parameter.defaultValue[0] != '\0')
                m_console.printf("    default   = %s\n", parameter.defaultValue);

            if (parameter.options[0] != '\0')
                m_console.printf("    options   = %s\n", parameter.options);
        }

        m_console.printf(
            "steps        = %u\n",
            static_cast<unsigned int>(macro.stepCount));

        for (size_t i = 0; i < macro.stepCount; ++i)
        {
            m_console.printf(
                "  %u = %s\n",
                static_cast<unsigned int>(i + 1),
                macro.steps[i]);
        }

        m_console.println("----------------------------------------");
        m_console.println("PASS: Macro file parsed successfully.");
        m_console.println("      No macro steps were executed.");

        m_console.println();
        m_console.println("Macro Expansion Test - v0.74(c)-1");
        m_console.println("----------------------------------------");

        for (size_t i = 0; i < macro.stepCount; ++i)
        {
            char expanded[224];

            const MacroConfiguration::Result expansionResult =
                m_macroConfiguration.expandStepDefaults(
                    macro,
                    i,
                    expanded,
                    sizeof(expanded));

            if (expansionResult != MacroConfiguration::Result::Success)
            {
                m_console.printf(
                    "FAIL step %u: %s\n",
                    static_cast<unsigned int>(i + 1),
                    MacroConfiguration::resultText(expansionResult));
                return false;
            }

            m_console.printf(
                "  %u = %s\n",
                static_cast<unsigned int>(i + 1),
                expanded);
        }

        m_console.println("----------------------------------------");
        m_console.println("PASS: Default parameters expanded successfully.");
        m_console.println("      No macro steps were executed.");

    }

    //
    // Wi-Fi (Network Service)
    //
    // Network configuration is read from /config/network.cfg
    // after the SD card has been initialized.
    //

    {
        uint32_t start = m_timer.milliseconds();

        bool success = m_wifiManager.initialize();

        uint32_t elapsed = m_timer.elapsed(start);

        m_startupTimeMs += elapsed;

        m_console.printInitialization(
            "WiFi",
            success,
            elapsed);

        if (!success)
        {
            return false;
        }

        if (m_wifiManager.isProvisioning())
        {
            m_oled.showSetupStatus();
            m_console.println("Provisioning AP active; normal web server not started.");
            return true;
        }

        m_console.printf(
            "  SSID     : %s\n",
            m_wifiManager.ssid());

        m_console.printf(
            "  Hostname : %s\n",
            m_wifiManager.hostname());

        const String ipAddress =
            m_wifiManager.ipAddress();

        m_console.printf(
            "  IP       : %s\n",
            ipAddress.c_str());

        m_console.printf(
            "  RSSI     : %ld dBm\n",
            static_cast<long>(
                m_wifiManager.rssi()));

        //
        // Update the local OLED with live network status.
        //

        m_oled.showNetworkStatus(
            ipAddress.c_str(),
            m_wifiManager.rssi());
    }

    //
    // Web Server (Network Service)
    //
    // Wi-Fi must already be connected before the HTTP server starts.
    //

    {
        uint32_t start = m_timer.milliseconds();

        bool success = m_webServerManager.initialize();

        uint32_t elapsed = m_timer.elapsed(start);

        m_startupTimeMs += elapsed;

        m_console.printInitialization(
            "Web Server",
            success,
            elapsed);

        if (!success)
        {
            return false;
        }
    }

    //
    // GPIB (Hardware Interface Service)
    //
    // The MCP23S17 must already be initialized before the
    // GPIB interface is initialized.
    //

    {
        uint32_t start = m_timer.milliseconds();

        bool success = m_gpib.initialize();

        uint32_t elapsed = m_timer.elapsed(start);

        m_startupTimeMs += elapsed;

        m_console.printInitialization(
            "GPIB",
            success,
            elapsed);

        if (!success)
        {
            return false;
        }
    }

    //
    // RS-232 (Hardware Interface Service) 
    //

    {
        uint32_t start = m_timer.milliseconds();
        bool success = m_rs232.initialize();
        uint32_t elapsed = m_timer.elapsed(start);
        m_startupTimeMs += elapsed;

        m_console.printInitialization("RS-232", success, elapsed);

        if (!success)
            return false;
    }

    //
    // Installed-instrument configuration 
    //
    // Enumerate /config/instruments.cfg generically.  GPIB definitions are
    // validated here but remain configured transaction-by-transaction by the
    // existing CommandExecutor.  RS-232 definitions configure their selected
    // UART port.  No instrument data is transmitted.
    //
    {
        InstrumentConfiguration instrumentConfiguration(
            m_sdCard, m_configurationFile);

        bool rs232PortConfigured[2]={false,false};
        size_t instrumentIndex=0;

        // A new controller may have no installed instruments. A missing
        // configuration file is normal; malformed existing files still fail.
        const bool hasInstrumentConfig = SD.exists("/config/instruments.cfg");
        if (!hasInstrumentConfig)
            m_console.println("  No instruments configured (instruments.cfg absent).");

        while (hasInstrumentConfig)
        {
            char section[32];
            bool fileOK=false;

            if (!instrumentConfiguration.sectionName(
                    instrumentIndex,section,sizeof(section),fileOK))
            {
                if (!fileOK)
                {
                    m_console.println("Instrument Config...............FAIL");
                    m_console.println("  Unable to enumerate /config/instruments.cfg");
                    return false;
                }
                break;
            }

            ++instrumentIndex;

            InstrumentConfiguration::Definition definition;

            if (!instrumentConfiguration.load(section,definition))
            {
                m_console.println("Instrument Config...............FAIL");
                m_console.printf("  Invalid instrument section: [%s]\n",section);
                return false;
            }

            if (definition.interfaceType==
                InstrumentConfiguration::Interface::GPIB)
            {
                m_console.printf(
                    "  Instrument %-12s GPIB address %u\n",
                    definition.section,
                    static_cast<unsigned int>(definition.address));
                continue;
            }

            if (definition.interfaceType==
                InstrumentConfiguration::Interface::TCP)
            {
                m_console.printf(
                    "  Instrument %-12s TCP %s:%u\n",
                    definition.section,
                    definition.host,
                    static_cast<unsigned int>(definition.tcpPort));
                continue;
            }

            const uint8_t portIndex=
                static_cast<uint8_t>(definition.port-1);

            if (rs232PortConfigured[portIndex])
            {
                m_console.println("Instrument Config...............FAIL");
                m_console.printf(
                    "  RS-232 port %u is assigned more than once\n",
                    static_cast<unsigned int>(definition.port));
                return false;
            }

            RS232::Parity parity;
            switch(definition.parity)
            {
                case InstrumentConfiguration::Parity::None:
                    parity=RS232::Parity::None; break;
                case InstrumentConfiguration::Parity::Even:
                    parity=RS232::Parity::Even; break;
                case InstrumentConfiguration::Parity::Odd:
                    parity=RS232::Parity::Odd; break;
                default:
                    m_console.println("Instrument Config...............FAIL");
                    return false;
            }

            RS232::PortConfiguration serialConfiguration={
                definition.baudRate,
                definition.dataBits,
                parity,
                definition.stopBits
            };

            if (!m_rs232.configurePort(definition.port,serialConfiguration))
            {
                m_console.println("Instrument Config...............FAIL");
                m_console.printf(
                    "  RS232::configurePort() rejected [%s]\n",
                    definition.section);
                return false;
            }

            rs232PortConfigured[portIndex]=true;

            m_console.printf(
                "  Instrument %-12s RS232 port %u  %lu  %u-%s-%u\n",
                definition.section,
                static_cast<unsigned int>(definition.port),
                static_cast<unsigned long>(definition.baudRate),
                static_cast<unsigned int>(definition.dataBits),
                InstrumentConfiguration::parityText(definition.parity),
                static_cast<unsigned int>(definition.stopBits));
        }

        if (hasInstrumentConfig && instrumentIndex==0)
            m_console.println("  No instruments configured (empty instruments.cfg).");
    }

    //
    // IEEE-488 (Protocol Service)
    //
    // The GPIB electrical interface must already be initialized.
    //

    {
        uint32_t start = m_timer.milliseconds();

        bool success = m_ieee488.initialize();

        uint32_t elapsed = m_timer.elapsed(start);

        m_startupTimeMs += elapsed;

        m_console.printInitialization(
            "IEEE-488",
            success,
            elapsed);

        if (!success)
        {
            return false;
        }
    }

    //
    // Activity monitor
    //
    // Baseline the transport counters after all interfaces are initialized.
    //
    m_activityMonitor.initialize();

    //
    // Controller (Top-Level Subsystem)
    //

    if (!initializeSubsystem(
            "Controller",
            m_controller))
    {
        return false;
    }

    //
    // Run diagnostics.
    //

    if (Configuration::Diagnostics::RunStartupTests && !runDiagnostics())
    {
        return false;
    }

    //
    // Startup summary.
    //

    m_console.printStartupTime(
        m_startupTimeMs);

    m_console.printSystemReady();

    return true;
}

/******************************************************************************
 *
 *  update()
 *
 ******************************************************************************/

void Application::update()
{
    if (m_wifiManager.isProvisioning())
    {
        // Update the OLED once on transition from normal Wi-Fi to setup AP.
        // This also covers entry via the normal web interface.
        if (!m_oled.isSetupStatus())
            m_oled.showSetupStatus();
        m_wifiManager.update();  // Serve setup-only HTTP; no instrument endpoints.
        return;
    }
    m_controller.update();
    m_webServerManager.update();
    // The web handler can enter provisioning during this iteration.
    // Update the display immediately rather than retaining the station IP.
    if (m_wifiManager.isProvisioning())
    {
        m_oled.showSetupStatus();
        return;
    }
    m_activityMonitor.update();
}

}   // namespace AK3Y