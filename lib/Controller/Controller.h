/******************************************************************************
 * File: Controller.h
 * Project: AK3Y Instrument Controller
 * Purpose: Console-oriented controller interface
 * Description: Declares the controller subsystem, console command processing, and hardware test coordination.
 * Note: This file header describes responsibilities; release history is in Git.
 ******************************************************************************/

#pragma once

#include <Arduino.h>

#include "Subsystem.h"
#include "Console.h"
#include "GPIB.h"
#include "IEEE488.h"
#include "Instrument.h"
#include "ScriptExecutor.h"

namespace AK3Y
{

class Controller : public Subsystem
{
public:

    Controller(
        Console& console,
        GPIB& gpib,
        IEEE488& ieee488,
        ScriptExecutor& scriptExecutor);

    bool initialize() override;

    void update();

private:

    static constexpr size_t CommandBufferSize = 96;

    Console&  m_console;
    GPIB&    m_gpib;
    IEEE488& m_ieee488;
    Instrument m_testInstrument;
    ScriptExecutor& m_scriptExecutor;

    bool m_initialized;

    char   m_commandBuffer[CommandBufferSize];
    size_t m_commandLength;

    bool m_ignoreNextLF;

    bool    m_gpibTestActive;
    uint8_t m_gpibTestStep;

    bool    m_gpibControlTestActive;
    uint8_t m_gpibControlTestStep;

    bool    m_gpibHandshakeTestActive;
    uint8_t m_gpibHandshakeTestStep;

    bool    m_gpibEOITestActive;
    uint8_t m_gpibEOITestStep;

    bool    m_gpibSRQTestActive;
    uint8_t m_gpibSRQTestStep;

    bool    m_ieee488ByteTestActive;
    uint8_t m_ieee488ByteTestStep;

    void processConsole();
    void processCommand();

    bool commandEqualsIgnoreCase(
        const char* command) const;

    void printGPIBControlPortDiagnostic();

    void startGPIBTest();
    void advanceGPIBTest();
    void finishGPIBTest();

    void startGPIBControlTest();
    void advanceGPIBControlTest();
    void finishGPIBControlTest();

    void startGPIBHandshakeTest();
    void advanceGPIBHandshakeTest();
    void finishGPIBHandshakeTest();

    void startGPIBEOITest();
    void advanceGPIBEOITest();
    void finishGPIBEOITest();

    void startGPIBSRQTest();
    void advanceGPIBSRQTest();
    void finishGPIBSRQTest();

    void startIEEE488ByteTest();
    void advanceIEEE488ByteTest();
    void finishIEEE488ByteTest();
};

}   // namespace AK3Y
