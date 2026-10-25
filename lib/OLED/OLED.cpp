/******************************************************************************
 * File: OLED.cpp
 * Project: AK3Y Instrument Controller
 * Purpose: Local SSD1306 status display.
 * Responsibilities: Initializes the I2C display, reports network status and startup errors, and shows instrument-interface activity.
 * Dependencies: Adafruit SSD1306, Wire, Console, Configuration.
 * Notes: Documentation only; interface behavior is defined by the code below.
 ******************************************************************************/

#include "OLED.h"
#include <Arduino.h>
#include <Wire.h>
#include <string.h>
#include <Configuration.h>
#include <Console.h>
#include <Fonts/FreeSans9pt7b.h>

namespace AK3Y
{
namespace
{
constexpr int DisplayWidth = 128;
constexpr int DisplayHeight = 64;
constexpr int ResetPin = -1;
constexpr uint8_t Address1 = 0x3C;
constexpr uint8_t Address2 = 0x3D;
}

OLED::OLED(Console& console)
    : m_console(console),
      m_display(DisplayWidth, DisplayHeight, &Wire, ResetPin),
      m_address(0),
      m_ipAddress{0},
      m_rssi(0),
      m_gpibActive(false),
      m_rs232Active(false),
      m_lanActive(false)
{
}

bool OLED::probeAddress(uint8_t address)
{
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
}

bool OLED::initialize()
{
    Wire.begin(Configuration::Pins::I2C::SDA,
               Configuration::Pins::I2C::SCL);

    if (probeAddress(Address1))
        m_address = Address1;
    else if (probeAddress(Address2))
        m_address = Address2;
    else
    {
        m_console.println("  OLED     : SSD1306 not found at 0x3C or 0x3D");
        return false;
    }

    m_console.printf("  OLED     : SSD1306 found at 0x%02X\n",
                     static_cast<unsigned>(m_address));

    if (!m_display.begin(SSD1306_SWITCHCAPVCC, m_address, false, false))
    {
        m_console.println("  OLED     : SSD1306 initialization failed");
        return false;
    }

    m_display.clearDisplay();
    m_display.setTextColor(SSD1306_WHITE);
    m_display.setTextSize(1);
    m_display.setTextWrap(false);
    m_display.setCursor(0, 0);
    m_display.println("AK3Y Controller");
    m_display.println();
    m_display.println("Starting...");
    m_display.display();

    return true;
}

void OLED::showNetworkStatus(
    const char* ipAddress,
    int32_t rssi)
{
    if (ipAddress == nullptr)
        ipAddress = "";

    strncpy(m_ipAddress, ipAddress, sizeof(m_ipAddress) - 1);
    m_ipAddress[sizeof(m_ipAddress) - 1] = '\0';
    m_rssi = rssi;
    m_setupStatus = false;

    renderStatus();
}

void OLED::showSetupStatus()
{
    strncpy(m_ipAddress, "192.168.4.1", sizeof(m_ipAddress) - 1);
    m_ipAddress[sizeof(m_ipAddress) - 1] = '\0';
    m_setupStatus = true;
    renderStatus();
}

bool OLED::isSetupStatus() const
{
    return m_setupStatus;
}

void OLED::setGPIBActivity(
    bool active)
{
    if (active == m_gpibActive)
        return;

    m_gpibActive = active;
    renderStatus();
}

void OLED::setRS232Activity(
    bool active)
{
    if (active == m_rs232Active)
        return;

    m_rs232Active = active;
    renderStatus();
}

void OLED::setLANActivity(
    bool active)
{
    if (active == m_lanActive)
        return;

    m_lanActive = active;
    renderStatus();
}

void OLED::showStartupError(
    const char* message)
{
    if (message == nullptr)
        message = "";

    m_display.clearDisplay();
    m_display.setTextColor(SSD1306_WHITE);
    m_display.setTextSize(1);
    m_display.setTextWrap(false);

    m_display.setCursor(0, 0);
    m_display.println("START UP ERROR");
    m_display.drawLine(0, 12, 127, 12, SSD1306_WHITE);
    m_display.setCursor(0, 22);
    m_display.println(message);
    m_display.display();
}

void OLED::renderStatus()
{
    m_display.clearDisplay();
    m_display.setTextColor(SSD1306_WHITE);
    m_display.setTextWrap(false);
    m_display.setFont(nullptr);
    m_display.setTextSize(1);

    // Compact title leaves more vertical space for the network address.
    m_display.setCursor(19, 0);
    m_display.print("AK3Y Controller");
    m_display.drawLine(0, 11, 127, 11, SSD1306_WHITE);

    // Use a larger proportional font for IPv4 addresses whenever it fits.
    // Long addresses fall back to the built-in 6x8 font without clipping.
    m_display.setFont(&FreeSans9pt7b);
    m_display.setTextSize(1);
    int16_t bx, by;
    uint16_t bw, bh;
    m_display.getTextBounds(m_ipAddress, 0, 0, &bx, &by, &bw, &bh);
    if (bw <= 124 && m_ipAddress[0] != '\0')
    {
        m_display.setCursor((128 - static_cast<int>(bw)) / 2 - bx, 31);
        m_display.print(m_ipAddress);
    }
    else
    {
        m_display.setFont(nullptr);
        const size_t length = strlen(m_ipAddress);
        const int x = (128 - static_cast<int>(length) * 6) / 2;
        m_display.setCursor(x > 0 ? x : 0, 21);
        m_display.print(m_ipAddress);
    }

    m_display.setFont(nullptr);
    m_display.setTextSize(1);
    // Center the Wi-Fi RSSI line using the default 6-pixel-wide font.
    const String wifiStatus = m_setupStatus
        ? String("WiFi SETUP AP")
        : String("WiFi ") + String(m_rssi) + " dBm";
    const int wifiWidth = static_cast<int>(wifiStatus.length()) * 6;
    m_display.setCursor((DisplayWidth - wifiWidth) / 2, 39);
    m_display.print(wifiStatus);
    m_display.drawLine(0, 49, 127, 49, SSD1306_WHITE);

    // Retain all three independent activity indicators.
    m_display.setCursor(0, 54);
    m_display.print("GPIB");
    m_display.setCursor(42, 54);
    m_display.print("RS232");
    m_display.setCursor(94, 54);
    m_display.print("LAN");

    if (m_gpibActive)
        m_display.fillCircle(31, 57, 2, SSD1306_WHITE);
    if (m_rs232Active)
        m_display.fillCircle(76, 57, 2, SSD1306_WHITE);
    if (m_lanActive)
        m_display.fillCircle(116, 57, 2, SSD1306_WHITE);

    m_display.display();
}

} // namespace AK3Y
