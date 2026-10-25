/******************************************************************************
 * File: WiFiManager.cpp
 * Project: AK3Y Instrument Controller
 * Purpose: Wi-Fi connectivity and network configuration.
 * Responsibilities: Loads Wi-Fi credentials from NVS, connects to Wi-Fi, and exposes network status.
 * Dependencies: Console, Arduino WiFi, Preferences.
 * Notes: Documentation only; interface behavior is defined by the code below.
 ******************************************************************************/

#include "WiFiManager.h"

#include <Console.h>
#include <WiFi.h>
#include <Preferences.h>
#include <string.h>
#include <time.h>

namespace AK3Y
{

WiFiManager::WiFiManager(
    ConfigurationFile& configurationFile,
    Console& console)
    :
    m_configurationFile(configurationFile),
    m_console(console),
    m_ssid(),
    m_password(),
    m_hostname()
{
}

bool WiFiManager::initialize()
{
    // Wi-Fi credentials are stored exclusively in NVS.
    // If unavailable, start first-time browser provisioning.
    bool haveCredentials = false;
    Preferences preferences;

    if (preferences.begin("ak3y-wifi", true))
    {
        const String storedSSID = preferences.getString("ssid", "");
        const String storedPassword = preferences.getString("password", "");
        const String storedHostname = preferences.getString("hostname", "");

        if (preferences.getBool("valid", false) &&
            storedSSID.length() > 0 &&
            storedSSID.length() < sizeof(m_ssid) &&
            storedPassword.length() < sizeof(m_password) &&
            storedHostname.length() < sizeof(m_hostname))
        {
            storedSSID.toCharArray(m_ssid, sizeof(m_ssid));
            storedPassword.toCharArray(m_password, sizeof(m_password));
            storedHostname.toCharArray(m_hostname, sizeof(m_hostname));
            haveCredentials = true;
        }
        preferences.end();
    }

    if (!haveCredentials)
    {
        m_console.println("Wi-Fi: No valid NVS credentials; starting setup.");
        return startProvisioningAP();
    }
    m_console.println("Wi-Fi: Credentials loaded from NVS.");

    WiFi.mode(WIFI_STA);
    if (m_hostname[0] != '\0')
        WiFi.setHostname(m_hostname);

    WiFi.begin(m_ssid, m_password);

    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED)
    {
        if ((millis() - start) >= ConnectionTimeoutMilliseconds)
        {
            WiFi.disconnect();
            m_console.println("Wi-Fi: Connection timed out; NVS not changed.");
            return startProvisioningAP();
        }
        delay(100);
    }

    // Configure SNTP in the background; do not wait for synchronization.
    configTzTime("EST5EDT,M3.2.0/2,M11.1.0/2",
                 "pool.ntp.org", "time.nist.gov");
    return true;
}

// Open setup AP: no password required.
bool WiFiManager::startProvisioningAP(bool userInitiated)
{
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP_STA); // Station radio is needed for nearby-network scans.
    const IPAddress address(192, 168, 4, 1);
    if (!WiFi.softAPConfig(address, address, IPAddress(255, 255, 255, 0)) ||
        !WiFi.softAP("AK3Y-Controller-Setup"))
    {
        m_console.println("Wi-Fi: Unable to start setup access point.");
        return false;
    }
    m_provisioning = true;
    m_userInitiatedSetup = userInitiated;
    m_setupStartedAt = millis();
    m_rebootPending = false;
    m_console.println("Wi-Fi: SETUP MODE - connect to AK3Y-Controller-Setup");
    m_console.println("Wi-Fi: Setup AP is open (no password).");
    m_console.println("Wi-Fi: Setup AP address: 192.168.4.1");
    m_setupServer.on("/", HTTP_GET, [this]() { showSetupPage(); });
    m_setupServer.on("/save", HTTP_POST, [this]() { handleSetupSubmit(); });
    m_setupServer.on("/networks", HTTP_GET, [this]() { handleNetworkList(); });
    m_setupServer.on("/rescan", HTTP_POST, [this]() { handleRescan(); });
    m_setupServer.on("/cancel", HTTP_POST, [this]() { handleSetupCancel(); });
    m_setupServer.onNotFound([this]() {
        m_setupServer.sendHeader("Location", "/", true);
        m_setupServer.send(302, "text/plain", "");
    });
    m_setupServer.begin();
    WiFi.scanDelete();
    WiFi.scanNetworks(true, false); // Async; setup HTTP remains responsive.
    if (m_userInitiatedSetup)
        m_console.println("Wi-Fi: User setup timeout is five minutes.");
    else
        m_console.println("Wi-Fi: Automatic fallback; no setup timeout.");
    m_console.println("Wi-Fi: Setup webpage ready at http://192.168.4.1/");
    return true;
}


void WiFiManager::showSetupPage(const String& message)
{
    String html = F(
        "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>AK3Y Wi-Fi Setup</title><style>"
        "body{font:16px system-ui,sans-serif;background:#f2f5f8;color:#243446;margin:0;padding:24px}"
        "main{max-width:480px;margin:35px auto;background:white;padding:28px;border-radius:12px;"
        "box-shadow:0 3px 18px #0002}h1{margin-top:0;font-size:23px}"
        "label{display:block;margin:18px 0 6px;font-weight:600}"
        "input,select{width:100%;box-sizing:border-box;padding:12px;border:1px solid #aab7c5;border-radius:6px;font-size:16px}"
        "button{margin-top:24px;padding:12px 20px;background:#155a95;color:white;border:0;border-radius:6px;font-size:16px}"
        ".note{font-size:14px;color:#556}.msg{padding:12px;background:#e9f2fb;border-radius:6px}"
        ".password-row{display:flex;gap:8px;align-items:center}"
        ".password-row input{flex:1;min-width:0}"
        ".password-row button{margin-top:0;white-space:nowrap;background:#687582}"
        "</style></head><body><main><h1>AK3Y Instrument Controller</h1>"
        "<p>Wi-Fi network setup</p>");
    if (message.length()) {
        html += F("<p class='msg'>");
        html += message; // Only fixed server-authored messages are passed here.
        html += F("</p>");
    }
    // Offer cancellation before the credential form, while the setup AP is active.
    if (m_userInitiatedSetup)
        html += F("<form method='POST' action='/cancel'>"
                  "<button type='submit' style='background:#687582'>Cancel Setup</button></form>"
                  "<p class='note'>Discard changes and restart on the previously saved network. "
                  "Setup also expires automatically after five minutes.</p>");
    html += F(
        "<form method='POST' action='/save'>"
        "<label for='ssid'>Network name (SSID)</label>"
        "<select id='networks' aria-label='Available Wi-Fi networks'><option value=''>Scanning nearby networks...</option></select>"
        "<button type='button' id='rescan' style='margin-top:8px;background:#687582'>Rescan Networks</button>"
        "<p class='note' id='scanstatus'>Scanning for nearby networks...</p>"
        "<label for='ssid'>Selected network (or enter manually)</label>"
        "<input id='ssid' name='ssid' maxlength='63' required autocomplete='off'>"
        "<label for='password'>Wi-Fi password</label>"
        "<div class='password-row'>"
        "<input id='password' name='password' type='password' maxlength='63' autocomplete='off'>"
        "<button type='button' id='togglePassword' aria-label='Show password' aria-pressed='false' title='Show password'>&#128065;</button>"
        "</div>"
        "<button type='submit'>Test and save network</button></form>"
        "<p class='note'>The controller will test the new network before saving it. "
        "Existing saved settings are retained if the test fails. "
        "The test can take up to 15 seconds.</p>");
    html += F(R"AK3Y(<script>
(function(){
 const select=document.getElementById('networks');
 const ssid=document.getElementById('ssid');
 const status=document.getElementById('scanstatus');
 const rescan=document.getElementById('rescan');
 const password=document.getElementById('password');
 const togglePassword=document.getElementById('togglePassword');
 togglePassword.addEventListener('click',()=>{
  const showing=password.type==='password';
  password.type=showing?'text':'password';
  togglePassword.setAttribute('aria-label',showing?'Hide password':'Show password');
  togglePassword.setAttribute('title',showing?'Hide password':'Show password');
  togglePassword.setAttribute('aria-pressed',String(showing));
 });
 let timer=null;
 function refresh(){
  fetch('/networks',{cache:'no-store'}).then(r=>r.json()).then(data=>{
   if(data.scanning){status.textContent='Scanning for nearby networks...';
    timer=setTimeout(refresh,1000);return;}
   select.replaceChildren();
   const manual=new Option('Other / enter manually','');
   select.add(manual);
   (data.networks||[]).forEach(n=>{
    const label=n.ssid+' ('+n.rssi+' dBm, '+(n.secure?'secured':'open')+')';
    select.add(new Option(label,n.ssid));
   });
   status.textContent=data.networks.length+' network(s) found. Hidden networks can be entered manually.';
   if(!ssid.value && data.networks.length){select.selectedIndex=1;ssid.value=select.value;}
   else {select.value=ssid.value;if(select.selectedIndex<0)select.value='';}
  }).catch(()=>{status.textContent='Scan unavailable; enter network name manually.';});
 }
 select.addEventListener('change',()=>{ssid.value=select.value;ssid.focus();});
 ssid.addEventListener('input',()=>{select.value=ssid.value;if(select.selectedIndex<0)select.value='';});
 rescan.addEventListener('click',()=>{
  if(timer)clearTimeout(timer);
  status.textContent='Starting scan...';
  fetch('/rescan',{method:'POST',cache:'no-store'}).then(()=>refresh())
   .catch(()=>{status.textContent='Rescan failed; enter network name manually.';});
 });
 refresh();
})();
</script></main></body></html>)AK3Y");
    m_setupServer.sendHeader("Cache-Control", "no-store");
    m_setupServer.send(200, "text/html; charset=utf-8", html);
}

// Escape untrusted SSIDs for JSON; the browser inserts them as text via Option().
static String escapeNetworkJSON(const String& value)
{
    String result;
    result.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i) {
        const uint8_t c = static_cast<uint8_t>(value[i]);
        if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
        else if (c < 0x20) { // JSON control characters
            const char digits[] = "0123456789abcdef";
            result += "\\u00";
            result += digits[c >> 4];
            result += digits[c & 15];
        } else result += char(c);
    }
    return result;
}

void WiFiManager::handleNetworkList()
{
    const int count = WiFi.scanComplete();
    String json = "{\"scanning\":";
    json += (count == WIFI_SCAN_RUNNING || count == WIFI_SCAN_FAILED) ? "true" : "false";
    json += ",\"networks\":[";
    if (count >= 0) {
        bool first = true;
        // scanNetworks returns strongest signals first on ESP32; deduplicate SSIDs.
        for (int i = 0; i < count; ++i) {
            const String name = WiFi.SSID(i);
            if (name.isEmpty()) continue;
            bool duplicate = false;
            for (int j = 0; j < i; ++j)
                if (WiFi.SSID(j) == name) { duplicate = true; break; }
            if (duplicate) continue;
            if (!first) json += ',';
            first = false;
            json += "{\"ssid\":\"";
            json += escapeNetworkJSON(name);
            json += "\",\"rssi\":";
            json += String(WiFi.RSSI(i));
            json += ",\"secure\":";
            json += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "false" : "true";
            json += '}';
        }
    }
    json += "]}";
    m_setupServer.sendHeader("Cache-Control", "no-store");
    m_setupServer.send(200, "application/json", json);
}

void WiFiManager::handleRescan()
{
    if (m_rebootPending) { m_setupServer.send(409, "text/plain", "Restart pending"); return; }
    WiFi.scanDelete();
    WiFi.scanNetworks(true, false);
    m_setupServer.sendHeader("Cache-Control", "no-store");
    m_setupServer.send(202, "text/plain", "Scan started");
}

void WiFiManager::handleSetupSubmit()
{
    const String candidateSSID = m_setupServer.arg("ssid");
    const String candidatePassword = m_setupServer.arg("password");
    if (candidateSSID.isEmpty() || candidateSSID.length() >= sizeof(m_ssid) ||
        candidatePassword.length() >= sizeof(m_password))
    {
        showSetupPage("Invalid SSID or password length.");
        return;
    }

    m_console.println("Wi-Fi: Testing candidate credentials (NVS unchanged).");
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(candidateSSID.c_str(), candidatePassword.c_str());
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - start < ConnectionTimeoutMilliseconds)
    {
        delay(100);
    }
    if (WiFi.status() != WL_CONNECTED)
    {
        WiFi.disconnect(false);
        m_console.println("Wi-Fi: Candidate connection failed; NVS unchanged.");
        showSetupPage("Connection failed. Check the network name and password, then try again.");
        return;
    }

    // Retain a copy for rollback if any NVS write fails.
    Preferences backup;
    String oldSSID, oldPassword, oldHostname;
    bool oldValid = false;
    if (backup.begin("ak3y-wifi", true))
    {
        oldSSID = backup.getString("ssid", "");
        oldPassword = backup.getString("password", "");
        oldHostname = backup.getString("hostname", "");
        oldValid = backup.getBool("valid", false);
        backup.end();
    }
    Preferences writer;
    bool committed = false;
    if (writer.begin("ak3y-wifi", false))
    {
        // Invalidate before writing so partial updates cannot be considered valid.
        const bool invalidated = writer.putBool("valid", false) == 1;
        const bool saved = invalidated &&
            writer.putString("ssid", candidateSSID) == candidateSSID.length() &&
            writer.putString("password", candidatePassword) == candidatePassword.length() &&
            writer.putString("hostname", m_hostname) == strlen(m_hostname);
        committed = saved && writer.putBool("valid", true) == 1;
        writer.end();
    }
    if (!committed)
    {
        Preferences rollback;
        if (rollback.begin("ak3y-wifi", false))
        {
            rollback.putBool("valid", false);
            rollback.putString("ssid", oldSSID);
            rollback.putString("password", oldPassword);
            rollback.putString("hostname", oldHostname);
            rollback.putBool("valid", oldValid);
            rollback.end();
        }
        m_console.println("Wi-Fi: NVS save failed; rollback attempted.");
        showSetupPage("Network connected, but saving failed. Please retry.");
        return;
    }
    m_console.println("Wi-Fi: New credentials verified and saved; restarting.");
    showSetupPage("Connection successful. Settings saved. Controller restarting; reconnect to your normal Wi-Fi.");
    m_rebootPending = true;
    m_rebootAt = millis() + 2500;
}

bool WiFiManager::enterUserProvisioning()
{
    if (m_provisioning || m_rebootPending || WiFi.status() != WL_CONNECTED)
        return false;
    return startProvisioningAP(true);
}

void WiFiManager::handleSetupCancel()
{
    if (!m_userInitiatedSetup || m_rebootPending)
    {
        m_setupServer.send(403, "text/plain", "Cancel unavailable in this mode.");
        return;
    }
    m_console.println("Wi-Fi: Setup cancelled; restarting with saved credentials.");
    m_setupServer.sendHeader("Cache-Control", "no-store");
    m_setupServer.send(200, "text/html; charset=utf-8",
        "<!doctype html><html><body><h2>Setup cancelled</h2>"
        "<p>Saved credentials were not changed. Controller restarting.</p>"
        "<p>Reconnect to your previous Wi-Fi network and use the IP address on the OLED.</p>"
        "</body></html>");
    m_rebootPending = true;
    m_rebootAt = millis() + 2500;
}

void WiFiManager::update()
{
    if (!m_provisioning) return;
    m_setupServer.handleClient();
    if (m_userInitiatedSetup && !m_rebootPending &&
        static_cast<uint32_t>(millis() - m_setupStartedAt) >= SetupTimeoutMilliseconds)
    {
        m_console.println("Wi-Fi: Five-minute setup timeout; restarting with saved credentials.");
        ESP.restart();
    }
    if (m_rebootPending && static_cast<int32_t>(millis() - m_rebootAt) >= 0)
        ESP.restart();
}

bool WiFiManager::isProvisioning() const
{
    return m_provisioning;
}

bool WiFiManager::isConnected() const
{
    return WiFi.status() == WL_CONNECTED;
}

const char* WiFiManager::ssid() const
{
    return m_ssid;
}

const char* WiFiManager::hostname() const
{
    return m_hostname;
}

String WiFiManager::ipAddress() const
{
    return WiFi.localIP().toString();
}

int32_t WiFiManager::rssi() const
{
    if (!isConnected())
    {
        return 0;
    }

    return WiFi.RSSI();
}

}   // namespace AK3Y
