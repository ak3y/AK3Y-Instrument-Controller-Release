/******************************************************************************
 * File: OLED.h
 * Project: AK3Y Instrument Controller
 * Purpose: Local SSD1306 status display.
 * Responsibilities: Initializes the I2C display, reports network status and startup errors, and shows instrument-interface activity.
 * Dependencies: Adafruit SSD1306, Wire, Console, Configuration.
 * Notes: Documentation only; interface behavior is defined by the code below.
 ******************************************************************************/

#pragma once
#include <Subsystem.h>
#include <Adafruit_SSD1306.h>

namespace AK3Y
{
class Console;

class OLED : public Subsystem
{
public:
    explicit OLED(Console& console);
    bool initialize() override;

    void showNetworkStatus(
        const char* ipAddress,
        int32_t rssi);

    void showSetupStatus();
    bool isSetupStatus() const;

    void setGPIBActivity(
        bool active);

    void setRS232Activity(
        bool active);

    void setLANActivity(
        bool active);

    void showStartupError(
        const char* message);

private:
    bool probeAddress(uint8_t address);

    Console& m_console;
    Adafruit_SSD1306 m_display;
    uint8_t m_address;
    char m_ipAddress[16];
    int32_t m_rssi;
    bool m_setupStatus = false;
    bool m_gpibActive;
    bool m_rs232Active;
    bool m_lanActive;

    void renderStatus();
};
} // namespace AK3Y
