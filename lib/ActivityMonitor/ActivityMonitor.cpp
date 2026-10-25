/******************************************************************************
 * File: ActivityMonitor.cpp
 * Project: AK3Y Instrument Controller
 * Purpose: Instrument-transport activity indication.
 * Responsibilities: Tracks IEEE-488, RS-232, and TCP instrument activity counters and updates the OLED activity indicators.
 * Dependencies: IEEE488, RS232, TCPInstrument, OLED.
 * Notes: Documentation only; interface behavior is defined by the code below.
 ******************************************************************************/

#include "ActivityMonitor.h"

#include <Arduino.h>
#include <IEEE488.h>
#include <OLED.h>
#include <RS232.h>
#include <TCPInstrument.h>

namespace AK3Y
{

namespace
{
constexpr uint32_t ActivityHoldMilliseconds = 500;
}

ActivityMonitor::ActivityMonitor(
    IEEE488& ieee488,
    RS232& rs232,
    TCPInstrument& tcpInstrument,
    OLED& oled)
    : m_ieee488(ieee488),
      m_rs232(rs232),
      m_tcpInstrument(tcpInstrument),
      m_oled(oled),
      m_lastGPIBCount(0),
      m_lastRS1Count(0),
      m_lastRS2Count(0),
      m_lastLANCount(0),
      m_gpibUntilMs(0),
      m_rs232UntilMs(0),
      m_lanUntilMs(0),
      m_gpibDisplayed(false),
      m_rs232Displayed(false),
      m_lanDisplayed(false)
{
}

void ActivityMonitor::initialize()
{
    // Establish the current counters as the baseline so initialization and
    // diagnostics do not appear later as stale activity.
    m_lastGPIBCount = m_ieee488.activityCount();
    m_lastRS1Count = m_rs232.activityCount(1);
    m_lastRS2Count = m_rs232.activityCount(2);
    m_lastLANCount = m_tcpInstrument.activityCount();

    m_gpibUntilMs = 0;
    m_rs232UntilMs = 0;
    m_lanUntilMs = 0;

    m_gpibDisplayed = false;
    m_rs232Displayed = false;
    m_lanDisplayed = false;

    m_oled.setGPIBActivity(false);
    m_oled.setRS232Activity(false);
    m_oled.setLANActivity(false);
}

void ActivityMonitor::update()
{
    const uint32_t now = millis();

    const uint32_t gpibCount = m_ieee488.activityCount();
    if (gpibCount != m_lastGPIBCount)
    {
        m_lastGPIBCount = gpibCount;
        m_gpibUntilMs = now + ActivityHoldMilliseconds;

        if (!m_gpibDisplayed)
        {
            m_oled.setGPIBActivity(true);
            m_gpibDisplayed = true;
        }
    }

    const uint32_t rs1Count = m_rs232.activityCount(1);
    const uint32_t rs2Count = m_rs232.activityCount(2);
    if (rs1Count != m_lastRS1Count || rs2Count != m_lastRS2Count)
    {
        m_lastRS1Count = rs1Count;
        m_lastRS2Count = rs2Count;
        m_rs232UntilMs = now + ActivityHoldMilliseconds;

        if (!m_rs232Displayed)
        {
            m_oled.setRS232Activity(true);
            m_rs232Displayed = true;
        }
    }

    const uint32_t lanCount = m_tcpInstrument.activityCount();
    if (lanCount != m_lastLANCount)
    {
        m_lastLANCount = lanCount;
        m_lanUntilMs = now + ActivityHoldMilliseconds;

        if (!m_lanDisplayed)
        {
            m_oled.setLANActivity(true);
            m_lanDisplayed = true;
        }
    }

    if (m_gpibDisplayed &&
        static_cast<int32_t>(now - m_gpibUntilMs) >= 0)
    {
        m_oled.setGPIBActivity(false);
        m_gpibDisplayed = false;
    }

    if (m_rs232Displayed &&
        static_cast<int32_t>(now - m_rs232UntilMs) >= 0)
    {
        m_oled.setRS232Activity(false);
        m_rs232Displayed = false;
    }

    if (m_lanDisplayed &&
        static_cast<int32_t>(now - m_lanUntilMs) >= 0)
    {
        m_oled.setLANActivity(false);
        m_lanDisplayed = false;
    }
}

} // namespace AK3Y
