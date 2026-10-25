/******************************************************************************
 * File: WiFiManager.h
 * Project: AK3Y Instrument Controller
 * Purpose: Wi-Fi connectivity and network configuration.
 * Responsibilities: Loads network settings from the SD-card configuration, connects to Wi-Fi, and exposes network status to other modules.
 * Dependencies: ConfigurationFile, Console, Arduino WiFi.
 * Notes: Documentation only; interface behavior is defined by the code below.
 ******************************************************************************/

#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <Subsystem.h>

namespace AK3Y
{

class ConfigurationFile;
class Console;

class WiFiManager : public Subsystem
{
public:
    WiFiManager(
        ConfigurationFile& configurationFile,
        Console& console);

    bool initialize() override;
    void update();

    // Called by the normal GUI in a later milestone, after user confirmation.
    bool enterUserProvisioning();

    bool isConnected() const;
    bool isProvisioning() const;
    const char* ssid() const;
    const char* hostname() const;
    String ipAddress() const;
    int32_t rssi() const;

private:
    ConfigurationFile& m_configurationFile;
    Console& m_console;

    bool m_provisioning = false;
    WebServer m_setupServer{80};
    bool m_rebootPending = false;
    uint32_t m_rebootAt = 0;
    bool m_userInitiatedSetup = false;
    uint32_t m_setupStartedAt = 0;
    void showSetupPage(const String& message = String());
    void handleSetupSubmit();
    void handleSetupCancel();
    void handleNetworkList();
    void handleRescan();
    bool startProvisioningAP(bool userInitiated = false);

    char m_ssid[64];
    char m_password[64];
    char m_hostname[64];

    static constexpr uint32_t ConnectionTimeoutMilliseconds = 15000;
    static constexpr uint32_t SetupTimeoutMilliseconds = 300000;
};

}   // namespace AK3Y
