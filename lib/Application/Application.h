/******************************************************************************
 * File: Application.h
 * Project: AK3Y Instrument Controller
 * Purpose: Top-level application composition and lifecycle interface.
 * Details: Declares ownership of the controller subsystems and the initialization, diagnostic, and update entry points.
 * Author: Bob Fontana, AK3Y
 *
 * Documentation only; firmware history is maintained in Git.
 ******************************************************************************/

#pragma once

#include <ActivityMonitor.h>
#include <Console.h>
#include <ConfigurationFile.h>
#include <CommandExecutor.h>
#include <Controller.h>
#include <GPIB.h>
#include <IEEE488.h>
#include <MCP23S17.h>
#include <MacroConfiguration.h>
#include <MacroExecutor.h>
#include <OLED.h>
#include <RS232.h>
#include <SDCard.h>
#include <ScriptExecutor.h>
#include <SPIManager.h>
#include <Subsystem.h>
#include <Timer.h>
#include <TCPInstrument.h>
#include <WiFiManager.h>
#include <WebServerManager.h>

namespace AK3Y
{

class Application
{
public:
    Application();
    bool initialize();
    void update();

private:
    bool initializeSubsystem(
        const char* name,
        Subsystem& subsystem);

    bool runDiagnostics();

    uint32_t m_startupTimeMs;

    Console     m_console;
    Timer       m_timer;
    SPIManager  m_spiManager;

    MCP23S17    m_mcp23s17;
    SDCard      m_sdCard;
    OLED        m_oled;

    ConfigurationFile m_configurationFile;
    MacroConfiguration m_macroConfiguration;

    WiFiManager m_wifiManager;

    GPIB        m_gpib;
    RS232       m_rs232;
    TCPInstrument m_tcpInstrument;

    IEEE488     m_ieee488;
    ActivityMonitor m_activityMonitor;

    CommandExecutor   m_commandExecutor;
    MacroExecutor     m_macroExecutor;
    WebServerManager  m_webServerManager;
    ScriptExecutor    m_scriptExecutor;

    Controller  m_controller;
};

}   // namespace AK3Y
