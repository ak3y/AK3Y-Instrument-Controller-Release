/******************************************************************************
 * File: ActivityMonitor.h
 * Project: AK3Y Instrument Controller
 * Purpose: Instrument-transport activity indication.
 * Responsibilities: Tracks IEEE-488, RS-232, and TCP instrument activity counters and updates the OLED activity indicators.
 * Dependencies: IEEE488, RS232, TCPInstrument, OLED.
 * Notes: Documentation only; interface behavior is defined by the code below.
 ******************************************************************************/

#pragma once

#include <stdint.h>

namespace AK3Y
{
class IEEE488;
class OLED;
class RS232;
class TCPInstrument;

class ActivityMonitor
{
public:
    ActivityMonitor(
        IEEE488& ieee488,
        RS232& rs232,
        TCPInstrument& tcpInstrument,
        OLED& oled);

    void initialize();
    void update();

private:
    IEEE488& m_ieee488;
    RS232& m_rs232;
    TCPInstrument& m_tcpInstrument;
    OLED& m_oled;

    uint32_t m_lastGPIBCount;
    uint32_t m_lastRS1Count;
    uint32_t m_lastRS2Count;
    uint32_t m_lastLANCount;

    uint32_t m_gpibUntilMs;
    uint32_t m_rs232UntilMs;
    uint32_t m_lanUntilMs;

    bool m_gpibDisplayed;
    bool m_rs232Displayed;
    bool m_lanDisplayed;
};

} // namespace AK3Y
