/******************************************************************************
 * File: WebServerManager.cpp
 * Project: AK3Y Instrument Controller
 *
 * Purpose:
 *   Implements the Wi-Fi HTTP interface for configuration-driven control of
 *   laboratory instruments and management of controller resources.
 *
 * Responsibilities:
 *   - Generate and stream browser pages and instrument control panels.
 *   - Dispatch instrument commands and report results and diagnostics.
 *   - Present and execute SD-card macros with progress and abort controls.
 *   - Manage SD-card files, uploads, and related browser operations.
 *   - Maintain browser-side workspace and control-panel interactions.
 *
 * Dependencies:
 *   WiFiManager, CommandExecutor, configuration and macro services,
 *   ActivityMonitor, Console, and the SD filesystem.
 *
 * Implementation notes:
 *   HTML and JavaScript are emitted from C++ strings. Preserve their escaping
 *   and delimiters when making future changes. HTTP responses are streamed to
 *   limit memory use on the embedded controller.
 ******************************************************************************/

#include "WebServerManager.h"

#include <ActivityMonitor.h>

#include <CommandExecutor.h>
#include <ConfigurationFile.h>
#include <Configuration.h>
#include <Console.h>
#include <EngineeringValue.h>
#include <MacroConfiguration.h>
#include <MacroExecutor.h>
#include <SD.h>
#include <WiFiManager.h>
#include <Update.h>

#include <ctype.h>
#include <time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace AK3Y
{

namespace
{

// Changes on every controller boot; distinguishes browser reload from reboot.
uint32_t macroWorkspaceBootId = 0;

void trim(char* text)
{
    if (text == nullptr)
        return;

    char* start = text;

    while (*start != '\0' &&
           isspace(static_cast<unsigned char>(*start)))
        ++start;

    if (start != text)
        memmove(text, start, strlen(start) + 1);

    size_t length = strlen(text);

    while (length > 0 &&
           isspace(static_cast<unsigned char>(text[length - 1])))
        text[--length] = '\0';
}

bool readLine(File& file, char* line, size_t lineSize)
{
    if (!file || line == nullptr || lineSize < 2)
        return false;

    size_t length = 0;
    bool received = false;

    while (file.available())
    {
        const int c = file.read();

        if (c < 0)
            break;

        received = true;

        if (c == '\r')
            continue;

        if (c == '\n')
            break;

        if (length + 1 < lineSize)
            line[length++] = static_cast<char>(c);
    }

    line[length] = '\0';
    return received;
}

bool copyText(
    char* destination,
    size_t destinationSize,
    const char* source)
{
    if (destination == nullptr ||
        destinationSize == 0 ||
        source == nullptr)
        return false;

    const int written =
        snprintf(destination, destinationSize, "%s", source);

    return written >= 0 &&
           static_cast<size_t>(written) < destinationSize;
}

// Format exact file sizes with thousands separators.
String formatFileBytes(uint64_t bytes)
{
    char digits[32];
    snprintf(digits, sizeof(digits), "%llu",
             static_cast<unsigned long long>(bytes));
    String result(digits);
    for (int i = static_cast<int>(result.length()) - 3; i > 0; i -= 3)
        result = result.substring(0, i) + "," + result.substring(i);
    return result;
}

// A plausible wall clock indicates that background SNTP has supplied time.
// Until then, retain explicit uptime timestamps. No waiting or network calls.
bool diagnosticTimestamp(char* output, size_t capacity)
{
    const time_t now = time(nullptr);
    if (now >= static_cast<time_t>(1735689600)) // 2025-01-01 UTC
    {
        struct tm local;
        if (localtime_r(&now, &local))
        {
            char date[48];
            if (strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S %Z", &local))
            {
                snprintf(output, capacity, "%s", date);
                return true;
            }
        }
    }
    snprintf(output, capacity, "[%010lu ms]", static_cast<unsigned long>(millis()));
    return false;
}

// Best-effort persistent diagnostics.  Failure must never affect an instrument
// transaction or the existing serial-console reporting.
void appendDiagnosticLog(const char* text)
{
    if (text == nullptr || text[0] == '\0')
        return;

    if (!SD.exists("/logs") && !SD.mkdir("/logs"))
        return;

    // Rotate before a write would exceed 256 KiB.  Archived files are never
    // deleted or overwritten; numbering continues beyond the highest archive.
    static constexpr size_t MaxLogBytes = 256U * 1024U;
    static constexpr const char* ActiveLog = "/logs/diagnostics.txt";

    File current = SD.open(ActiveLog, FILE_READ);
    if (current)
    {
        const size_t currentSize = current.size();
        current.close();

        if (currentSize > 0 &&
            (currentSize >= MaxLogBytes ||
             strlen(text) > MaxLogBytes - currentSize))
        {
            unsigned long highest = 0;
            File directory = SD.open("/logs");
            if (directory && directory.isDirectory())
            {
                File entry = directory.openNextFile();
                while (entry)
                {
                    if (!entry.isDirectory())
                    {
                        const char* filename = entry.name();
                        const char* slash = strrchr(filename, '/');
                        if (slash)
                            filename = slash + 1;

                        unsigned long number = 0;
                        int consumed = 0;
                        if (sscanf(filename, "diagnostics-%lu.txt%n",
                                   &number, &consumed) == 1 &&
                            consumed == static_cast<int>(strlen(filename)) &&
                            number > highest)
                            highest = number;
                    }
                    entry.close();
                    entry = directory.openNextFile();
                }
            }
            directory.close();

            if (highest < 999999999UL)
            {
                char archive[64];
                snprintf(archive, sizeof(archive),
                         "/logs/diagnostics-%lu.txt", highest + 1);
                if (!SD.exists(archive))
                    SD.rename(ActiveLog, archive);
                // If rename fails, continue appending to the active log.
            }
        }
    }

    File log = SD.open(ActiveLog, FILE_APPEND);
    if (!log)
        return;

    log.print(text);
    log.close();
}

// v0.87(b): best-effort history with parameter snapshots and bounded archives.
// The newest archive is -1; up to three archives are retained.
void appendMacroHistory(const char* macroFile, const char* event,
                        const char* detail, uint32_t elapsedMs,
                        bool includeElapsed,
                        const MacroConfiguration::ValueOverride* parameters = nullptr,
                        size_t parameterCount = 0)
{
    static const char* ActiveLog = "/logs/macro_history.txt";
    static const size_t MaxLogBytes = 256UL * 1024UL;
    static const int ArchiveCount = 3;

    if (!SD.exists("/logs") && !SD.mkdir("/logs"))
        return;

    // Rotate before writing a new entry. Avoid touching existing archives
    // unless the active file has actually reached its size limit.
    File existing = SD.open(ActiveLog, FILE_READ);
    const bool rotate = existing && existing.size() >= MaxLogBytes;
    if (existing)
        existing.close();
    if (rotate)
    {
        char older[64], newer[64];
        snprintf(older, sizeof(older), "/logs/macro_history-%d.txt", ArchiveCount);
        if (SD.exists(older))
            SD.remove(older);
        for (int i = ArchiveCount - 1; i >= 1; --i)
        {
            snprintf(older, sizeof(older), "/logs/macro_history-%d.txt", i);
            snprintf(newer, sizeof(newer), "/logs/macro_history-%d.txt", i + 1);
            if (SD.exists(older))
                SD.rename(older, newer);
        }
        if (SD.exists(ActiveLog))
            SD.rename(ActiveLog, "/logs/macro_history-1.txt");
    }

    File history = SD.open(ActiveLog, FILE_APPEND);
    if (!history)
        return;

    char timestamp[64];
    diagnosticTimestamp(timestamp, sizeof(timestamp));
    history.printf("%s  %s  %s", timestamp, event,
                   macroFile ? macroFile : "(unknown)");
    if (includeElapsed)
        history.printf("  elapsed=%.3f s", elapsedMs / 1000.0);
    if (detail && *detail)
        history.printf("  result=%s", detail);
    history.println();

    if (parameters && parameterCount)
    {
        history.println("  Parameters:");
        for (size_t i = 0; i < parameterCount; ++i)
        {
            // Prevent embedded newlines in browser-supplied values from
            // creating forged log records.
            String name = parameters[i].name ? parameters[i].name : "";
            String value = parameters[i].value ? parameters[i].value : "";
            name.replace("\r", " "); name.replace("\n", " ");
            value.replace("\r", " "); value.replace("\n", " ");
            history.printf("    %s = %s\n", name.c_str(), value.c_str());
        }
    }
    if (includeElapsed)
        history.println();
    history.close();
}

// Restore standard top-level directories if removed outside File Management.
// Never restore deleted contents or overwrite a path occupied by a file.
bool ensureWebSystemDirectories()
{
    constexpr const char* paths[] = {"/config", "/instruments", "/macros", "/logs"};
    for (const char* path : paths) {
        if (!SD.exists(path) && !SD.mkdir(path)) return false;
        File directory = SD.open(path, FILE_READ);
        const bool valid = directory && directory.isDirectory();
        if (directory) directory.close();
        if (!valid) return false;
    }
    return true;
}

bool validSavedConfigurationName(const String& name)
{
    if (name.length() < 1 || name.length() > 40) return false;
    for (size_t i = 0; i < name.length(); ++i) {
        const char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

} // anonymous namespace

WebServerManager::WebServerManager(
    WiFiManager& wifiManager,
    ConfigurationFile& configurationFile,
    CommandExecutor& commandExecutor,
    MacroConfiguration& macroConfiguration,
    MacroExecutor& macroExecutor,
    ActivityMonitor& activityMonitor,
    Console& console)
    :
    m_wifiManager(wifiManager),
    m_configurationFile(configurationFile),
    m_commandExecutor(commandExecutor),
    m_macroConfiguration(macroConfiguration),
    m_macroExecutor(macroExecutor),
    m_activityMonitor(activityMonitor),
    m_console(console),
    m_server(80)
{
    m_commandExecutor.setDiagnosticCallback(
        &WebServerManager::runtimeDiagnosticCallback, this);
}

void WebServerManager::runtimeDiagnosticCallback(
    bool recovered,
    const CommandExecutor::TransportDiagnostic& diagnostic,
    const char* errorText,
    void* context)
{
    if (!context)
        return;

    WebServerManager* self = static_cast<WebServerManager*>(context);
    const char* name = diagnostic.instrumentName ? diagnostic.instrumentName : "";
    char timestamp[64];
    diagnosticTimestamp(timestamp, sizeof(timestamp));
    char endpoint[112];

    switch (diagnostic.transport)
    {
        case CommandExecutor::Transport::GPIB:
            snprintf(endpoint, sizeof(endpoint), "%s (GPIB %u)",
                     name, static_cast<unsigned>(diagnostic.gpibAddress));
            break;
        case CommandExecutor::Transport::RS232:
            snprintf(endpoint, sizeof(endpoint), "%s (RS-232 port %u)",
                     name, static_cast<unsigned>(diagnostic.serialPort));
            break;
        case CommandExecutor::Transport::TCP:
            snprintf(endpoint, sizeof(endpoint), "%s (TCP port %u)",
                     name, static_cast<unsigned>(diagnostic.tcpPort));
            break;
        default:
            return;
    }

    char entry[320];
    if (recovered)
        snprintf(entry, sizeof(entry), "%s RECOVERED %s\n",
                 timestamp, endpoint);
    else
        snprintf(entry, sizeof(entry), "%s ERROR %s: %s\n",
                 timestamp, endpoint,
                 errorText ? errorText : "Unknown transport error");

    self->m_console.print(entry);
    appendDiagnosticLog(entry);
}

bool WebServerManager::initialize()
{
    macroWorkspaceBootId = esp_random();
    if (!m_wifiManager.isConnected())
        return false;

    // Separate successive boots with a blank line.  Build date/time is
    // explicitly identified as such; millis() is uptime, not wall-clock time.
    // The log write is best-effort and never prevents web-server startup.
    char startup[256];
    char stamp[64];
    diagnosticTimestamp(stamp, sizeof(stamp));
    snprintf(startup, sizeof(startup),
             "\n%s STARTUP firmware %s; build %s %s\n",
             stamp, Configuration::Firmware::Version, __DATE__, __TIME__);
    appendDiagnosticLog(startup);

    m_server.on("/", HTTP_GET,
        [this]() { handleRoot(); });

    // lightweight live RSSI readout; no page regeneration.
    m_server.on("/wifi-rssi", HTTP_GET, [this]() {
        m_server.sendHeader("Cache-Control", "no-store");
        if (!m_wifiManager.isConnected()) {
            m_server.send(200, "text/plain", "Disconnected");
            return;
        }
        m_server.send(200, "text/plain", String(m_wifiManager.rssi()));
    });

    // Small same-origin probe for the computer's setup-waiting page.
    // Available only while the normal controller web server is running.
    m_server.on("/wifi-return-ready", HTTP_GET, [this]() {
        m_server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
        m_server.send(200, "text/plain", "AK3Y-CONTROLLER-READY");
    });

    // Operator-initiated LAN OTA. The upload endpoint is not authenticated:
    // intended only for a trusted LAN, never direct Internet exposure.
    m_server.on("/firmware-update", HTTP_GET, [this]() {
        m_server.sendHeader("Cache-Control", "no-store");
        m_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        m_server.send(200, "text/html; charset=utf-8", "");
        m_server.sendContent(
            "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>AK3Y Firmware Update</title><style>"
            "body{font:16px system-ui,sans-serif;max-width:680px;margin:40px auto;padding:0 20px;line-height:1.55;color:#243446}"
            "a{color:#155a95}.note{background:#eef3f8;padding:14px;border-radius:8px}"
            ".file-picker,button{display:inline-block;padding:13px 22px;background:#155a95;color:white;"
            "border:0;border-radius:7px;font-size:17px;font-weight:600;cursor:pointer}"
            ".file-picker:hover,button:hover{background:#104977}button:disabled{opacity:.5;cursor:default}"
            ".file-picker input{position:absolute;width:1px;height:1px;opacity:0;overflow:hidden}"
            "progress{width:100%;height:22px}"
            "</style></head><body><h1>Firmware Update</h1><p><b>Running version:</b> ");
        m_server.sendContent(Configuration::Firmware::Version);
        m_server.sendContent("</p><p><b>Current firmware image:</b> ");
        m_server.sendContent(String(ESP.getSketchSize()));
        m_server.sendContent(" bytes</p><p><b>Available OTA application space:</b> ");
        m_server.sendContent(String(ESP.getFreeSketchSpace()));
        m_server.sendContent(" bytes</p><p><b>Flash capacity:</b> ");
        m_server.sendContent(String(ESP.getFlashChipSize()));
        m_server.sendContent(
            " bytes</p><p class='note'>Select a compiled ESP32 application image (.bin). "
            "Installation will restart the controller; do not interrupt power during the update.</p>"
            "<p><b>Select OTA firmware (.bin):</b></p>"
            "<p><label class='file-picker'>Select Firmware File..."
            "<input id='fwfile' type='file' accept='.bin,application/octet-stream'></label></p>"
            "<p id='fwstatus' role='status'>No file selected.</p>"
            "<p><button id='install' disabled>Install Firmware</button></p>"
            "<progress id='progress' value='0' max='100' hidden></progress>"
            "<p id='uploadstatus' role='status'></p>"
            "<script>const f=document.getElementById('fwfile'),s=document.getElementById('fwstatus');"
            "const btn=document.getElementById('install'),bar=document.getElementById('progress');"
            "const out=document.getElementById('uploadstatus');"
            "const max=" + String(ESP.getFreeSketchSpace()) + ";"
            "function valid(){const x=f.files[0];btn.disabled=true;"
            "if(!x){s.textContent='No file selected.';return false;}"
            "if(!/\\.bin$/i.test(x.name)){s.textContent='Please select a .bin firmware file.';return false;}"
            "if(!x.size||x.size>max){s.textContent='Invalid image size: '+x.size.toLocaleString()+' bytes; limit '+max.toLocaleString()+' bytes.';return false;}"
            "s.textContent='Selected '+x.name+' ('+x.size.toLocaleString()+' bytes). Size check passed.';"
            "btn.disabled=false;return true;}"
            "f.addEventListener('change',valid);"
            "btn.addEventListener('click',()=>{if(!valid())return;"
            "if(!confirm('Install the selected firmware? The controller will restart and any instrument operations will be interrupted.'))return;"
            "const x=f.files[0];btn.disabled=true;f.disabled=true;bar.hidden=false;bar.value=0;"
            "out.textContent='Uploading firmware...';"
            "const data=new FormData();data.append('firmware',x,x.name);"
            "const req=new XMLHttpRequest();req.open('POST','/firmware-install?size='+x.size);"
            "req.upload.onprogress=e=>{if(e.lengthComputable){bar.value=Math.round(100*e.loaded/e.total);"
            "out.textContent='Uploading: '+bar.value+'%';}};"
            "req.onload=()=>{if(req.status===200){out.textContent='Firmware installed. Controller restarting...';"
            "setTimeout(()=>{location.href='/';},12000);}else{out.textContent='Update failed: '+req.responseText;"
            "btn.disabled=false;f.disabled=false;}};"
            "req.onerror=()=>{out.textContent='Connection lost during upload. Check controller status before retrying.';"
            "btn.disabled=false;f.disabled=false;};req.send(data);});"
            "</script><p><a href='/'>Return to Controller</a></p></body></html>");
        m_server.sendContent("");
    });

    m_server.on("/firmware-install", HTTP_POST,
        [this]() {
            if (m_otaOK && m_otaFinalized && !Update.hasError() && m_otaBytes == m_otaExpected) {
                m_server.sendHeader("Cache-Control", "no-store");
                m_server.sendHeader("Connection", "close");
                m_server.send(200, "text/plain", "Firmware installed. Restarting.");
                m_otaRestartPending = true;
                m_otaRestartAt = millis();
            } else {
                m_server.send(400, "text/plain", m_otaError.length() ? m_otaError : "Firmware upload failed.");
            }
        },
        [this]() {
            HTTPUpload& upload = m_server.upload();
            if (upload.status == UPLOAD_FILE_START) {
                m_otaOK = false;
                m_otaFinalized = false;
                m_otaBytes = 0;
                m_otaExpected = 0;
                m_otaError = "";
                if (m_macroPending || m_macroRunning || m_wifiSetupPending || m_otaRestartPending) {
                    m_otaError = "Controller busy; cannot update firmware.";
                    return;
                }
                const String declared = m_server.arg("size");
                const unsigned long n = strtoul(declared.c_str(), nullptr, 10);
                if (!declared.length() || n == 0 || String(n) != declared ||
                    n > ESP.getFreeSketchSpace()) {
                    m_otaError = "Invalid or oversized firmware image.";
                    return;
                }
                if (!upload.filename.endsWith(".bin")) {
                    m_otaError = "Firmware must be a .bin file.";
                    return;
                }
                m_otaExpected = n;
                if (!Update.begin(m_otaExpected, U_FLASH)) {
                    m_otaError = "Unable to begin OTA: " + String(Update.errorString());
                    return;
                }
                m_otaOK = true;
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (!m_otaOK) return;
                if (upload.currentSize > m_otaExpected - m_otaBytes ||
                    Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                    m_otaOK = false;
                    m_otaError = "Firmware write failed or image exceeded declared size.";
                    Update.abort();
                    return;
                }
                m_otaBytes += upload.currentSize;
            } else if (upload.status == UPLOAD_FILE_END) {
                if (!m_otaOK) return;
                if (m_otaBytes != m_otaExpected || !Update.end(false)) {
                    m_otaOK = false;
                    m_otaError = "Firmware image incomplete or invalid: " + String(Update.errorString());
                    Update.abort();
                } else {
                    m_otaFinalized = true;
                }
            } else if (upload.status == UPLOAD_FILE_ABORTED) {
                m_otaOK = false;
                m_otaError = "Upload aborted.";
                Update.abort();
            }
        });

    // User-initiated Wi-Fi provisioning: display instructions before disconnecting.
    m_server.on("/wifi-settings", HTTP_GET, [this]() {
        m_server.sendHeader("Cache-Control", "no-store");
        m_server.send(200, "text/html; charset=utf-8",
            "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>AK3Y Wi-Fi Settings</title><style>body{font:16px system-ui,sans-serif;"
            "max-width:620px;margin:45px auto;padding:0 20px;line-height:1.55;color:#243446}"
            "button{padding:12px 18px;font-size:16px;background:#155a95;color:white;border:0;border-radius:6px}"
            "</style></head><body><h1>Update Wi-Fi</h1>"
            "<p>Before proceeding, note these instructions. This webpage will disconnect when setup begins.</p>"
            "<p><b>1.</b> Join the open Wi-Fi network <b>AK3Y-Controller-Setup</b> (no password).</p>"
            "<p><b>2.</b> Open <b>http://192.168.4.1/</b> to enter the new network credentials.</p>"
            "<p><b>3.</b> After saving, reconnect to the new network and use the IP address on the OLED.</p>"
            "<p>Setup lasts five minutes. Cancel or timeout restores the previous saved network.</p>"
            "<form method='POST' action='/wifi-enter-setup'>"
            "<button type='submit'>Enter Setup Mode</button></form>"
            "<p><a href='/'>Return to Controller</a></p></body></html>");
    });
    m_server.on("/wifi-enter-setup", HTTP_POST, [this]() {
        if (m_macroPending || m_macroRunning || m_wifiSetupPending) {
            m_server.send(409, "text/plain", "Cannot change Wi-Fi while a macro or transition is active.");
            return;
        }
        m_server.sendHeader("Cache-Control", "no-store");
        m_server.sendHeader("Connection", "close");
        m_server.send(200, "text/html; charset=utf-8",
            "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>Entering Wi-Fi Setup</title><style>body{font:16px system-ui,sans-serif;"
            "max-width:620px;margin:45px auto;padding:0 20px;line-height:1.55;color:#243446}"
            "</style></head><body><h2>Entering Setup Mode</h2>"
            "<p>Join the open Wi-Fi network <b>AK3Y-Controller-Setup</b>, then open "
            "<a href='http://192.168.4.1/'>http://192.168.4.1/</a>.</p>"
            "<p>Use <b>Cancel Setup</b> on that page to restore the saved network, "
            "or wait for the five-minute timeout.</p>"
            "<p id='status'>Waiting for the controller to leave setup mode...</p>"
            "<p>The computer will return to the Controller automatically if it "
            "reconnects at its previous IP address. If the new network gives the "
            "controller a different IP, use the address shown on the OLED.</p>"
            "<script>"
            "const statusEl=document.getElementById('status');"
            "let wasOffline=false;"
            "async function checkController(){"
            "const abort=new AbortController();"
            "const timer=setTimeout(()=>abort.abort(),1500);"
            "try{"
            "const response=await fetch('/wifi-return-ready?check='+Date.now(),"
            "{cache:'no-store',signal:abort.signal});"
            "const body=await response.text();"
            "if(response.ok && body==='AK3Y-CONTROLLER-READY' && wasOffline){"
            "statusEl.textContent='Controller reconnected. Returning...';"
            "window.location.replace('/');return;}"
            "}catch(e){wasOffline=true;"
            "statusEl.textContent='Controller in setup mode. Waiting for it to reconnect...';}"
            "finally{clearTimeout(timer);}"
            "setTimeout(checkController,2500);"
            "}"
            "setTimeout(checkController,4000);"
            "</script></body></html>");
        // Defer AP transition until the HTTP response has been sent.
        m_wifiSetupPending = true;
        m_wifiSetupAt = millis();
    });

    m_server.on("/command", HTTP_POST,
        [this]() { handleCommand(); });

    //
    // execute the displayed macro with browser-supplied
    // parameter overrides.
    //
    m_server.on("/macro-start", HTTP_POST,
        [this]() { handleMacroStart(); });

    m_server.on("/macro-status", HTTP_GET,
        [this]() { handleMacroStatus(); });

    m_server.on("/macro-abort", HTTP_POST,
        [this]() { handleMacroAbort(); });

    //
    // asynchronous Advanced Command Console.
    //

    m_server.on("/command-async", HTTP_POST,
        [this]() { handleCommandAsync(); });

    m_server.on("/capability", HTTP_POST,
        [this]() { handleCapability(); });

    //
    // generic asynchronous numeric capability transaction.
    //

    m_server.on("/capability-async", HTTP_POST,
        [this]() { handleCapabilityAsync(); });

    //
    // generic function-entry transaction.
    //
    m_server.on("/function-async", HTTP_POST,
        [this]() { handleFunctionAsync(); });

    //
    // save/load complete browser workspace configuration.
    // Loading is browser-only and performs no instrument I/O.
    //
    m_server.on("/control-box-config", HTTP_GET,
        [this]() { handleControlBoxConfig(); });

    m_server.on("/save-control-box-config", HTTP_POST,
        [this]() { handleSaveControlBoxConfig(); });

    // v0.89(a): enumerate named configurations (legacy default is separate).
    m_server.on("/saved-configurations", HTTP_GET, [this]() {
        String names = "";
        File directory = SD.open("/config/workspaces");
        if (directory && directory.isDirectory()) {
            File entry = directory.openNextFile();
            while (entry) {
                if (!entry.isDirectory()) {
                    String filename = entry.name();
                    const int slash = filename.lastIndexOf('/');
                    if (slash >= 0) filename = filename.substring(slash + 1);
                    if (filename.endsWith(".cfg")) {
                        String name = filename.substring(0, filename.length() - 4);
                        bool valid = name.length() >= 1 && name.length() <= 40;
                        for (size_t i = 0; i < name.length(); ++i) {
                            const char c = name[i];
                            if (!((c >= 'A' && c <= 'Z') ||
                                  (c >= 'a' && c <= 'z') ||
                                  (c >= '0' && c <= '9') || c == '_' || c == '-'))
                                valid = false;
                        }
                        if (valid && name != "default") names += name + "\n";
                    }
                }
                entry.close();
                entry = directory.openNextFile();
            }
        }
        if (directory) directory.close();
        m_server.sendHeader("Cache-Control", "no-store");
        m_server.send(200, "text/plain; charset=utf-8", names);
    });

    // Stage 5: optional named workspace loaded on a fresh browser visit.
    // Pointer is separate from all existing configuration files.
    m_server.on("/startup-configuration", HTTP_GET, [this]() {
        String name;
        File f = SD.open("/config/startup_workspace.txt", FILE_READ);
        if (f) { name = f.readStringUntil('\n'); f.close(); name.trim(); }
        if (!validSavedConfigurationName(name) || name == "default" ||
            !SD.exists(("/config/workspaces/" + name + ".cfg").c_str())) name = "";
        m_server.sendHeader("Cache-Control", "no-store");
        m_server.send(200, "text/plain", name);
    });
    m_server.on("/startup-configuration", HTTP_POST, [this]() {
        if (m_macroPending || m_macroRunning) {
            m_server.send(409, "text/plain", "Cannot change startup configuration while macro runs."); return;
        }
        if (!m_server.hasArg("name")) {
            m_server.send(400, "text/plain", "Missing startup configuration name."); return;
        }
        String name = m_server.arg("name");
        if (name.length() && (!validSavedConfigurationName(name) || name == "default" ||
            !SD.exists(("/config/workspaces/" + name + ".cfg").c_str()))) {
            m_server.send(400, "text/plain", "Startup configuration does not exist."); return;
        }
        const char* path = "/config/startup_workspace.txt";
        if (!name.length()) {
            if (SD.exists(path) && !SD.remove(path)) {
                m_server.send(500, "text/plain", "Unable to clear startup setting."); return;
            }
        } else {
            const char* tmp = "/config/startup_workspace.tmp";
            if (SD.exists(tmp)) SD.remove(tmp);
            File f = SD.open(tmp, FILE_WRITE);
            if (!f) { m_server.send(500, "text/plain", "Unable to write startup setting."); return; }
            bool ok = f.println(name) > name.length(); // newline written
            f.close();
            if (!ok) { SD.remove(tmp); m_server.send(500, "text/plain", "Startup setting write failed."); return; }
            // Keep old setting if temporary write fails; replace only afterward.
            if (SD.exists(path) && !SD.remove(path)) {
                SD.remove(tmp); m_server.send(500, "text/plain", "Unable to replace startup setting."); return;
            }
            if (!SD.rename(tmp, path)) {
                m_server.send(500, "text/plain", "Unable to install startup setting."); return;
            }
        }
        m_server.sendHeader("Cache-Control", "no-store");
        m_server.send(200, "text/plain", name.length() ? "Startup configuration selected." : "Startup configuration cleared.");
    });

    // Stage 3: rename a named workspace without altering its contents.
    m_server.on("/rename-saved-configuration", HTTP_POST, [this]() {
        if (m_macroPending || m_macroRunning) {
            m_server.send(409, "text/plain", "Cannot rename while a macro is executing."); return;
        }
        if (!m_server.hasArg("old") || !m_server.hasArg("new")) {
            m_server.send(400, "text/plain", "Missing configuration names."); return;
        }
        const String oldName = m_server.arg("old");
        const String newName = m_server.arg("new");
        if (!validSavedConfigurationName(oldName) ||
            !validSavedConfigurationName(newName) ||
            oldName == "default" || newName == "default" || oldName == newName) {
            m_server.send(400, "text/plain", "Invalid configuration rename."); return;
        }
        const String oldPath = "/config/workspaces/" + oldName + ".cfg";
        const String newPath = "/config/workspaces/" + newName + ".cfg";
        if (!SD.exists(oldPath.c_str())) {
            m_server.send(404, "text/plain", "Original configuration not found."); return;
        }
        if (SD.exists(newPath.c_str())) {
            m_server.send(409, "text/plain", "Destination name already exists. No changes made."); return;
        }
        if (!SD.rename(oldPath.c_str(), newPath.c_str())) {
            m_server.send(500, "text/plain", "SD card rename failed. Original retained."); return;
        }
        File startup = SD.open("/config/startup_workspace.txt", FILE_READ);
        if (startup) {
            String selected = startup.readStringUntil('\n'); startup.close(); selected.trim();
            if (selected == oldName) {
                SD.remove("/config/startup_workspace.txt");
                File out = SD.open("/config/startup_workspace.txt", FILE_WRITE);
                if (out) { out.println(newName); out.close(); }
            }
        }
        m_server.sendHeader("Cache-Control", "no-store");
        m_server.send(200, "text/plain", "Configuration renamed.");
    });

    // Stage 4: explicitly delete one named workspace configuration.
    // The legacy default and all unrelated SD files are protected.
    m_server.on("/delete-saved-configuration", HTTP_POST, [this]() {
        if (m_macroPending || m_macroRunning) {
            m_server.send(409, "text/plain", "Cannot delete while a macro is executing."); return;
        }
        if (!m_server.hasArg("name")) {
            m_server.send(400, "text/plain", "Missing configuration name."); return;
        }
        const String name = m_server.arg("name");
        if (!validSavedConfigurationName(name) || name == "default") {
            m_server.send(400, "text/plain", "Invalid configuration name."); return;
        }
        const String path = "/config/workspaces/" + name + ".cfg";
        if (!SD.exists(path.c_str())) {
            m_server.send(404, "text/plain", "Configuration not found."); return;
        }
        if (!SD.remove(path.c_str())) {
            m_server.send(500, "text/plain", "SD card delete failed."); return;
        }
        File startup = SD.open("/config/startup_workspace.txt", FILE_READ);
        if (startup) {
            String selected = startup.readStringUntil('\n'); startup.close(); selected.trim();
            if (selected == name) SD.remove("/config/startup_workspace.txt");
        }
        m_server.sendHeader("Cache-Control", "no-store");
        m_server.send(200, "text/plain", "Configuration deleted.");
    });

    //
    // validate an entire Control Box first, then explicitly
    // apply its active settable parameters to the selected instrument.
    //
    m_server.on("/send-control-box", HTTP_POST,
        [this]() { handleSendControlBox(); });

    //
    // view an individual SD-card file in a read-only browser page.
    // The file contents are displayed verbatim in a <pre> block.
    //
    m_server.on("/sd-view", HTTP_GET,
        [this]() { handleSDView(); });

    //
    // save a small text file without touching the original until
    // the complete replacement has been written and verified.
    m_server.on("/sd-edit-save", HTTP_POST, [this]() {
        if (!m_server.hasArg("path") || !m_server.hasArg("version") ||
            !m_server.hasArg("plain")) {
            m_server.send(400, "text/plain", "Missing file path, version, or text."); return;
        }
        const String path = m_server.arg("path");
        String lower(path); lower.toLowerCase();
        if (path.length() < 6 || !path.startsWith("/") || path.endsWith("/") ||
            path.indexOf("..") >= 0 || path.indexOf('\\') >= 0 ||
            path.indexOf("//") >= 0 || path.indexOf(':') >= 0 ||
            path.indexOf('\r') >= 0 || path.indexOf('\n') >= 0 ||
            !(lower.endsWith(".cfg") || lower.endsWith(".txt")) ||
            lower.startsWith("/logs/") || path.length() > 180) {
            m_server.send(400, "text/plain", "Invalid editable file path."); return;
        }
        const String& contents = m_server.arg("plain");
        // WebServer buffers the HTTP body. Keep the limit conservative on ESP32.
        if (contents.length() > 32768U) {
            m_server.send(413, "text/plain", "Save limit is 32 KB."); return;
        }
        File original = SD.open(path.c_str(), FILE_READ);
        if (!original || original.isDirectory()) {
            if (original) original.close();
            m_server.send(404, "text/plain", "Original file not found."); return;
        }
        uint32_t hash = 2166136261UL;
        while (original.available()) {
            const int b = original.read();
            if (b < 0) break;
            hash = (hash ^ static_cast<uint8_t>(b)) * 16777619UL;
        }
        original.close();
        char version[9]; snprintf(version, sizeof(version), "%08lx", (unsigned long)hash);
        if (m_server.arg("version") != version) {
            m_server.send(409, "text/plain", "File changed on SD since opening. Reopen before saving."); return;
        }
        const String temporary = path + ".edit-tmp";
        const String backup = path + ".edit-bak";
        if (SD.exists(temporary.c_str()) || SD.exists(backup.c_str())) {
            m_server.send(409, "text/plain", "Temporary or backup file already exists; no changes made."); return;
        }
        File replacement = SD.open(temporary.c_str(), FILE_WRITE);
        if (!replacement) {
            m_server.send(500, "text/plain", "Unable to create temporary file."); return;
        }
        const size_t written = replacement.write(
            reinterpret_cast<const uint8_t*>(contents.c_str()), contents.length());
        replacement.flush();
        const size_t actualSize = replacement.size();
        replacement.close();
        if (written != contents.length() || actualSize != contents.length()) {
            SD.remove(temporary.c_str());
            m_server.send(500, "text/plain", "Incomplete temporary write; original preserved."); return;
        }
        if (!SD.rename(path.c_str(), backup.c_str())) {
            SD.remove(temporary.c_str());
            m_server.send(500, "text/plain", "Could not back up original; original preserved."); return;
        }
        if (!SD.rename(temporary.c_str(), path.c_str())) {
            const bool restored = SD.rename(backup.c_str(), path.c_str());
            m_server.send(500, "text/plain", restored ?
                "Replacement failed; original restored." :
                "Replacement failed; original remains in .edit-bak. Recover manually.");
            return;
        }
        // Leave a backup rather than risk loss if a subsequent operation fails.
        if (!SD.remove(backup.c_str())) {
            m_server.send(200, "text/plain", "Saved; old backup retained as .edit-bak.");
            return;
        }
        m_server.send(200, "text/plain", "Saved to SD card.");
    });

    // browser text editor (SD card remains read-only).
    m_server.on("/sd-edit", HTTP_GET, [this]() {
        if (!m_server.hasArg("path")) {
            m_server.send(400, "text/plain", "Missing SD-card path.");
            return;
        }
        const String path = m_server.arg("path");
        const String lower = [&path]() { String p(path); p.toLowerCase(); return p; }();
        if (path.length() < 6 || !path.startsWith("/") ||
            path.endsWith("/") || path.indexOf("..") >= 0 ||
            path.indexOf('\\') >= 0 || path.indexOf("//") >= 0 ||
            path.indexOf(':') >= 0 || path.indexOf('\r') >= 0 ||
            path.indexOf('\n') >= 0 ||
            !(lower.endsWith(".cfg") || lower.endsWith(".txt")) ||
            lower.startsWith("/logs/")) {
            m_server.send(400, "text/plain", "Only ordinary .cfg and .txt files outside /logs can be edited.");
            return;
        }
        File file = SD.open(path.c_str(), FILE_READ);
        if (!file || file.isDirectory()) {
            if (file) file.close();
            m_server.send(404, "text/plain", "Text file not found.");
            return;
        }
        if (file.size() > 32768U) {
            file.close();
            m_server.send(413, "text/plain", "Editor limit is 32 KB.");
            return;
        }
        uint32_t originalHash = 2166136261UL;
        while (file.available()) {
            const int b = file.read();
            if (b < 0) break;
            originalHash = (originalHash ^ static_cast<uint8_t>(b)) * 16777619UL;
        }
        file.seek(0);
        char version[9];
        snprintf(version, sizeof(version), "%08lx", (unsigned long)originalHash);
        m_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        m_server.sendHeader("Cache-Control", "no-store");
        m_server.sendHeader("X-Content-Type-Options", "nosniff");
        m_server.send(200, "text/html; charset=utf-8", "");
        sendChunk(
            "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>SD File Editor</title><style>"
            "body{font-family:Arial,sans-serif;margin:24px;color:#222}"
            ".path{font-family:monospace;font-weight:bold;overflow-wrap:anywhere;margin:14px 0}"
            "textarea{display:block;width:100%;box-sizing:border-box;min-height:65vh;"
            "font-family:monospace;font-size:14px;line-height:1.4;white-space:pre;"
            "tab-size:4;padding:12px;border:1px solid #aaa}"
            ".note{color:#555;margin:12px 0}</style></head><body>"
            "<h2>SD Card File Editor</h2><div class=\"path\">");
        sendEscaped(path.c_str());
        sendChunk("</div><p class=\"note\">Edit this file, then Save to SD. Saving does not execute macros.</p>"
                  "<textarea id=\"editor\" spellcheck=\"false\" aria-label=\"File contents\">");
        // Escape HTML while streaming: no whole-file String allocation.
        char output[1024];
        size_t used = 0;
        auto flush = [&]() {
            if (used) { m_server.sendContent(output, used); used = 0; }
        };
        uint8_t input[512];
        while (file.available()) {
            const size_t count = file.read(input, sizeof(input));
            if (!count) break;
            for (size_t i = 0; i < count; ++i) {
                const unsigned char c = input[i];
                const char* entity = nullptr;
                switch (c) {
                    case '&': entity = "&amp;"; break;
                    case '<': entity = "&lt;"; break;
                    case '>': entity = "&gt;"; break;
                    case 0: entity = "&#0;"; break;
                    default: break;
                }
                if (entity) {
                    const size_t n = strlen(entity);
                    if (used + n > sizeof(output)) flush();
                    memcpy(output + used, entity, n);
                    used += n;
                } else {
                    if (used == sizeof(output)) flush();
                    output[used++] = static_cast<char>(c);
                }
            }
        }
        flush();
        file.close();
        sendChunk("</textarea><p class=\"note\" id=\"editStatus\">No changes.</p>"
                  "<button type=\"button\" id=\"saveBtn\">Save to SD</button> "
                  "<button type=\"button\" onclick=\"window.close()\">Close</button>"
                  "<script>const e=document.getElementById('editor');"
                  "let initial=e.value;let version='");
        sendChunk(version);
        sendChunk("';const path=");
        // JSON-safe path via percent encoding; never interpolate raw filenames into JS.
        sendChunk("decodeURIComponent('");
        for (size_t i = 0; i < path.length(); ++i) {
            const unsigned char c = static_cast<unsigned char>(path[i]);
            char hex[4]; snprintf(hex, sizeof(hex), "%%%02X", c);
            sendChunk(hex);
        }
        sendChunk("');const status=document.getElementById('editStatus');"
                  "const btn=document.getElementById('saveBtn');"
                  "function update(){status.textContent=e.value===initial?'No changes.':"
                  "'Unsaved changes.';}e.addEventListener('input',update);"
                  "btn.addEventListener('click',async()=>{if(e.value===initial)return;"
                  "if(!confirm('Replace the SD-card file with these edits?'))return;"
                  "btn.disabled=true;status.textContent='Saving...';"
                  "try{const body=e.value;if(new TextEncoder().encode(body).length>32768)"
                  "throw Error('Save limit is 32 KB.');"
                  "const r=await fetch('/sd-edit-save?path='+encodeURIComponent(path)+"
                  "'&version='+version,{method:'POST',headers:{'Content-Type':'text/plain; charset=utf-8'},body});"
                  "const msg=await r.text();if(!r.ok)throw Error(msg);"
                  "initial=body;let h=2166136261;for(const byte of new TextEncoder().encode(body))"
                  "{h=Math.imul(h^byte,16777619)>>>0;}version=h.toString(16).padStart(8,'0');"
                  "status.textContent=msg;}catch(err){status.textContent='Save failed: '+err.message;}"
                  "finally{btn.disabled=false;}});"
                  "window.addEventListener('beforeunload',event=>{if(e.value!==initial){"
                  "event.preventDefault();event.returnValue='';}});"
                  "</script></body></html>");
        m_server.sendContent("");
    });

    // download an individual SD-card file.
    // This is intentionally read-only and does not alter the SD card.
    //
    m_server.on("/sd-download", HTTP_GET,
        [this]()
        {
            if (!m_server.hasArg("path"))
            {
                m_server.send(400, "text/plain", "Missing SD-card path.");
                return;
            }

            String path = m_server.arg("path");
            path.trim();

            if (path.length() < 2 ||
                !path.startsWith("/") ||
                path.indexOf("..") >= 0 ||
                path.indexOf('\\') >= 0)
            {
                m_server.send(400, "text/plain", "Invalid SD-card path.");
                return;
            }

            File file = SD.open(path.c_str(), FILE_READ);
            if (!file || file.isDirectory())
            {
                if (file)
                    file.close();
                m_server.send(404, "text/plain", "SD-card file not found.");
                return;
            }

            int slash = path.lastIndexOf('/');
            String filename = (slash >= 0) ? path.substring(slash + 1) : path;
            filename.replace("\"", "");
            filename.replace("\r", "");
            filename.replace("\n", "");

            m_server.sendHeader(
                "Content-Disposition",
                String("attachment; filename=\"") + filename + "\"");
            m_server.sendHeader("Cache-Control", "no-store");
            m_server.streamFile(file, "application/octet-stream");
            file.close();
        });

    // authoritative directory choices for both SD-card selectors.
    m_server.on("/sd-folders", HTTP_GET, [this]() {
        if (!ensureWebSystemDirectories()) {
            m_server.send(500, "text/plain", "Unable to restore SD system directories.");
            return;
        }
        String folders = "/\n";
        File root = SD.open("/");
        if (root && root.isDirectory()) {
            File entry = root.openNextFile();
            while (entry) {
                if (entry.isDirectory()) {
                    String name = entry.name();
                    int slash = name.lastIndexOf('/');
                    String base = name.substring(slash + 1);
                    if (!base.startsWith(".") && name.indexOf('\n') < 0 && name.indexOf('\r') < 0) {
                        if (!name.startsWith("/")) name = "/" + name;
                        folders += name + "\n";
                    }
                }
                entry.close();
                entry = root.openNextFile();
            }
            root.close();
        }
        m_server.sendHeader("Cache-Control", "no-store");
        m_server.send(200, "text/plain; charset=utf-8", folders);
    });

    // SD-card-only HTML fragment, avoiding full GUI generation.
    m_server.on("/sd-list", HTTP_GET, [this]() {
        if (!ensureWebSystemDirectories()) {
            m_server.send(500, "text/plain", "Unable to restore SD system directories.");
            return;
        }
        const uint32_t started = millis();
        m_htmlBufferUsed = 0;
        m_htmlBufferActive = true;
        m_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        m_server.send(200, "text/html; charset=utf-8", "");
    // grouped SD-card downloads; hidden entries are omitted.
    sendChunk(
        "<div class=\"panel\" id=\"sdFilePanel\"><div style=\"display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap\"><h2>SD Card Files</h2><button type=\"button\" onclick=\"refreshSDFilePanel().catch(e=>alert(e.message))\">Refresh File List</button></div>"
        "<p>View, edit, download, or delete individual files; remove empty non-system directories.</p>");

    bool foundDownloadFile = false;
    bool rootHeadingShown = false;
    File downloadRoot = SD.open("/");
    if (downloadRoot && downloadRoot.isDirectory())
    {
        File entry = downloadRoot.openNextFile();
        while (entry)
        {
            String entryName = entry.name();
            int entrySlash = entryName.lastIndexOf('/');
            String entryBase = (entrySlash >= 0) ? entryName.substring(entrySlash + 1) : entryName;

            if (!entryBase.startsWith("."))
            {
                if (entry.isDirectory())
                {
                    String directoryPath = entryName;
                    if (!directoryPath.startsWith("/"))
                        directoryPath = String("/") + directoryPath;

                    File directory = SD.open(directoryPath.c_str());
                    if (directory && directory.isDirectory())
                    {
                        // always display the directory, including
                        // empty directories and directories containing hidden files.
                        String normalized = directoryPath; normalized.toLowerCase();
                        bool protectedDirectory = normalized == "/config" || normalized == "/instruments" || normalized == "/macros" || normalized == "/logs" ||
                            normalized.startsWith("/config/") || normalized.startsWith("/instruments/") || normalized.startsWith("/macros/") || normalized.startsWith("/logs/");
                        // Count hidden regular files; never offer bulk removal if
                        // another entry (visible or directory) is present.
                        unsigned hiddenCount = 0;
                        bool otherEntries = false;
                        File scan = directory.openNextFile();
                        while (scan) {
                            String scanName = scan.name();
                            int scanSlash = scanName.lastIndexOf('/');
                            String base = scanName.substring(scanSlash + 1);
                            if (!scan.isDirectory() && base.startsWith(".")) ++hiddenCount;
                            else otherEntries = true;
                            scan.close();
                            scan = directory.openNextFile();
                        }
                        bool directoryEmpty = !hiddenCount && !otherEntries;
                        directory.rewindDirectory();

                        sendChunk("<div style=\"display:flex;align-items:center;gap:10px;margin-top:18px;padding-bottom:5px;border-bottom:1px solid #ddd\"><strong style=\"flex:1;font-size:14px;letter-spacing:.04em\">");
                        String heading = entryBase;
                        heading.toUpperCase();
                        sendEscaped(heading.c_str());
                        if (directoryEmpty) sendChunk(" (empty)");
                        if (hiddenCount) {
                            sendChunk(" (");
                            sendChunk(String(hiddenCount));
                            sendChunk(hiddenCount == 1 ? " hidden file)" : " hidden files)");
                        }
                        sendChunk("</strong>");
                        if (hiddenCount && !otherEntries && !protectedDirectory) {
                            sendChunk("<form method=\"post\" action=\"/sd-remove-hidden\" style=\"margin:0\" onsubmit=\"return confirmSDHiddenDelete(this);\"><input type=\"hidden\" name=\"path\" value=\"");
                            sendEscaped(directoryPath.c_str());
                            sendChunk("\"><button type=\"submit\">Remove Hidden Files</button></form>");
                        }
                        if (directoryEmpty && !protectedDirectory)
                        {
                            sendChunk("<form method=\"post\" action=\"/sd-rmdir\" style=\"margin:0\" onsubmit=\"return confirmSDDirectoryDelete(this);\"><input type=\"hidden\" name=\"path\" value=\"");
                            sendEscaped(directoryPath.c_str());
                            sendChunk("\"><button type=\"submit\">Remove Empty Directory</button></form>");
                        }
                        sendChunk("</div>");
                        File child = directory.openNextFile();
                        while (child)
                        {
                            String childName = child.name();
                            int childSlash = childName.lastIndexOf('/');
                            String childBase = (childSlash >= 0) ? childName.substring(childSlash + 1) : childName;

                            if (!child.isDirectory() && !childBase.startsWith("."))
                            {
                                String fullPath = childName.startsWith("/") ? childName : directoryPath + "/" + childName;
                                sendChunk("<div style=\"display:flex;align-items:center;gap:8px;padding:5px 0;border-bottom:1px solid #f0f0f0\"><span style=\"flex:1;font-family:monospace;font-size:15px;overflow-wrap:anywhere\">");
                                sendEscaped(childBase.c_str());
                                sendChunk("</span><span style=\"min-width:100px;text-align:right;font-family:monospace;font-size:13px;white-space:nowrap;color:#555\">");
                                sendChunk(formatFileBytes(child.size()));
                                sendChunk(" bytes");
                                sendChunk("</span>");
                                if ((fullPath.endsWith(".cfg") || fullPath.endsWith(".txt")) &&
                                    !fullPath.startsWith("/logs/")) {
                                    sendChunk("<form method=\"get\" action=\"/sd-edit\" target=\"_blank\" style=\"margin:0\"><input type=\"hidden\" name=\"path\" value=\"");
                                    sendEscaped(fullPath.c_str());
                                    sendChunk("\"><button type=\"submit\" style=\"margin:0;padding:5px 12px;font-size:14px\">Edit</button></form>");
                                }
                                sendChunk("<form method=\"get\" action=\"/sd-view\" target=\"_blank\" style=\"margin:0\"><input type=\"hidden\" name=\"path\" value=\"");
                                sendEscaped(fullPath.c_str());
                                sendChunk("\"><button type=\"submit\" style=\"margin:0;padding:5px 12px;font-size:14px\">View</button></form><form method=\"get\" action=\"/sd-download\" style=\"margin:0\"><input type=\"hidden\" name=\"path\" value=\"");
                                sendEscaped(fullPath.c_str());
                                sendChunk("\"><button type=\"submit\" style=\"margin:0;padding:5px 12px;font-size:14px\">Download</button></form><form method=\"post\" action=\"/sd-delete\" style=\"margin:0\" onsubmit=\"return confirmSDDelete(this);\"><input type=\"hidden\" name=\"path\" value=\"");
                                sendEscaped(fullPath.c_str());
                                sendChunk("\"><button type=\"submit\" style=\"margin:0;padding:5px 12px;font-size:14px\">Delete</button></form></div>");
                                foundDownloadFile = true;
                            }
                            child.close();
                            child = directory.openNextFile();
                        }
                        directory.close();
                    }
                }
                else
                {
                    if (!rootHeadingShown)
                    {
                        sendChunk("<div style=\"margin-top:18px;padding-bottom:5px;border-bottom:1px solid #ddd;font-weight:bold;font-size:14px;letter-spacing:.04em\">ROOT</div>");
                        rootHeadingShown = true;
                    }

                    String fullPath = entryName;
                    if (!fullPath.startsWith("/"))
                        fullPath = String("/") + fullPath;

                    sendChunk("<div style=\"display:flex;align-items:center;gap:8px;padding:5px 0;border-bottom:1px solid #f0f0f0\"><span style=\"flex:1;font-family:monospace;font-size:15px;overflow-wrap:anywhere\">");
                    sendEscaped(entryBase.c_str());
                                sendChunk("</span><span style=\"min-width:100px;text-align:right;font-family:monospace;font-size:13px;white-space:nowrap;color:#555\">");
                                sendChunk(formatFileBytes(entry.size()));
                                sendChunk(" bytes");
                    sendChunk("</span>");
                                if ((fullPath.endsWith(".cfg") || fullPath.endsWith(".txt")) &&
                                    !fullPath.startsWith("/logs/")) {
                                    sendChunk("<form method=\"get\" action=\"/sd-edit\" target=\"_blank\" style=\"margin:0\"><input type=\"hidden\" name=\"path\" value=\"");
                                    sendEscaped(fullPath.c_str());
                                    sendChunk("\"><button type=\"submit\" style=\"margin:0;padding:5px 12px;font-size:14px\">Edit</button></form>");
                                }
                                sendChunk("<form method=\"get\" action=\"/sd-view\" target=\"_blank\" style=\"margin:0\"><input type=\"hidden\" name=\"path\" value=\"");
                    sendEscaped(fullPath.c_str());
                    sendChunk("\"><button type=\"submit\" style=\"margin:0;padding:5px 12px;font-size:14px\">View</button></form><form method=\"get\" action=\"/sd-download\" style=\"margin:0\"><input type=\"hidden\" name=\"path\" value=\"");
                    sendEscaped(fullPath.c_str());
                    sendChunk("\"><button type=\"submit\" style=\"margin:0;padding:5px 12px;font-size:14px\">Download</button></form><form method=\"post\" action=\"/sd-delete\" style=\"margin:0\" onsubmit=\"return confirmSDDelete(this);\"><input type=\"hidden\" name=\"path\" value=\"");
                    sendEscaped(fullPath.c_str());
                    sendChunk("\"><button type=\"submit\" style=\"margin:0;padding:5px 12px;font-size:14px\">Delete</button></form></div>");
                    foundDownloadFile = true;
                }
            }

            entry.close();
            entry = downloadRoot.openNextFile();
        }
        downloadRoot.close();
    }

    if (!foundDownloadFile)
        sendChunk("<div class=\"range\">No files found on the SD card.</div>");

    sendChunk("<div class=\"range\" style=\"margin-top:14px\">Hidden entries are omitted. Only empty, non-system directories may be removed.</div></div>");

        flushHtmlBuffer();
        m_htmlBufferActive = false;
        m_server.sendContent("");
        Serial.printf("[SD LIST] Directory fragment: %lu ms\n",
                      (unsigned long)(millis() - started));
    });

    //
    // delete one explicitly selected ordinary SD-card file.
    // Directories are never deleted by this endpoint.
    //
    m_server.on("/sd-delete", HTTP_POST,
        [this]() { handleSDDelete(); });

    // remove only hidden regular files from a non-system folder.
    // Preflight the complete directory before deleting anything.
    m_server.on("/sd-remove-hidden", HTTP_POST, [this]() {
        if (!m_server.hasArg("path")) { m_server.send(400, "text/plain", "Missing directory path."); return; }
        String path = m_server.arg("path");
        if (path.length() < 2 || !path.startsWith("/") || path.endsWith("/") ||
            path.indexOf("..") >= 0 || path.indexOf('\\') >= 0 ||
            path.indexOf("//") >= 0 || path.indexOf(':') >= 0 ||
            path.indexOf('\r') >= 0 || path.indexOf('\n') >= 0) {
            m_server.send(400, "text/plain", "Invalid directory path."); return;
        }
        String normalized = path; normalized.toLowerCase();
        const char* protectedFolders[] = {"/config", "/instruments", "/macros", "/logs"};
        for (const char* folder : protectedFolders) {
            String root(folder);
            if (normalized == root || normalized.startsWith(root + "/")) {
                m_server.send(403, "text/plain", "Protected directory."); return;
            }
        }
        File directory = SD.open(path.c_str());
        if (!directory || !directory.isDirectory()) {
            if (directory) directory.close();
            m_server.send(404, "text/plain", "Directory not found."); return;
        }
        unsigned count = 0;
        bool unsafe = false;
        File entry = directory.openNextFile();
        while (entry) {
            String name = entry.name();
            int slash = name.lastIndexOf('/');
            String base = name.substring(slash + 1);
            if (entry.isDirectory() || !base.startsWith(".") ||
                base == "." || base == ".." || base.indexOf('\\') >= 0 ||
                base.indexOf(':') >= 0 || base.indexOf('\r') >= 0 ||
                base.indexOf('\n') >= 0 || ++count > 64) unsafe = true;
            entry.close();
            entry = directory.openNextFile();
        }
        directory.close();
        if (unsafe || !count) {
            m_server.send(409, "text/plain", "Directory must contain only 1-64 hidden regular files."); return;
        }
        // Reopen and remove one file at a time; don't retain an open directory
        // iterator while mutating its contents.
        unsigned removed = 0;
        for (unsigned i = 0; i < count; ++i) {
            File dir = SD.open(path.c_str());
            if (!dir || !dir.isDirectory()) {
                if (dir)
                    dir.close();
                break;
            }
            File child = dir.openNextFile();
            if (!child) { dir.close(); break; }
            String raw = child.name();
            int slash = raw.lastIndexOf('/');
            String base = raw.substring(slash + 1);
            bool valid = !child.isDirectory() && base.startsWith(".") &&
                base != "." && base != ".." && base.indexOf('/') < 0 &&
                base.indexOf('\\') < 0 && base.indexOf(':') < 0;
            child.close(); dir.close();
            if (!valid || !SD.remove((path + "/" + base).c_str())) break;
            ++removed;
        }
        if (removed != count) {
            m_server.send(500, "text/plain", "Removed " + String(removed) +
                          " of " + String(count) + " hidden files. Refresh listing."); return;
        }
        m_server.send(200, "text/plain", "Removed " + String(removed) + " hidden files.");
    });

    // delete an empty, non-system SD directory only.
    m_server.on("/sd-rmdir", HTTP_POST, [this]() {
        if (!m_server.hasArg("path")) { m_server.send(400, "text/plain", "Missing directory path."); return; }
        String path = m_server.arg("path"); path.trim();
        if (path.length() < 2 || !path.startsWith("/") || path.endsWith("/") ||
            path.indexOf("..") >= 0 || path.indexOf('\\') >= 0 ||
            path.indexOf("//") >= 0 || path.indexOf(':') >= 0) {
            m_server.send(400, "text/plain", "Invalid directory path."); return;
        }
        String normalized = path; normalized.toLowerCase();
        // Protect all top-level system folders, including their descendants.
        const char* protectedFolders[] = {"/config", "/instruments", "/macros", "/logs"};
        for (const char* protectedFolder : protectedFolders) {
            String root(protectedFolder);
            if (normalized == root || normalized.startsWith(root + "/")) {
                m_server.send(403, "text/plain", "Protected system directory."); return;
            }
        }
        File directory = SD.open(path.c_str());
        if (!directory) { m_server.send(404, "text/plain", "Directory not found."); return; }
        if (!directory.isDirectory()) { directory.close(); m_server.send(400, "text/plain", "Path is not a directory."); return; }
        File child = directory.openNextFile();
        bool empty = !child;
        if (child) child.close();
        directory.close();
        if (!empty) { m_server.send(409, "text/plain", "Directory is not empty."); return; }
        if (!SD.rmdir(path.c_str())) { m_server.send(500, "text/plain", "Unable to remove directory."); return; }
        m_server.send(200, "text/plain", "Removed.");
    });


    //
    // create an SD-card directory from the web interface.
    //
    m_server.on("/sd-mkdir", HTTP_POST,
        [this]() { handleSDCreateDirectory(); });

    //
    // check whether an SD-card upload destination already exists.
    //
    m_server.on("/sd-exists", HTTP_POST,
        [this]() { handleSDExists(); });

    //
    // browser-to-SD file upload.
    //
    m_server.on(
        "/sd-upload",
        HTTP_POST,
        [this]() { handleSDUpload(); },
        [this]() { handleSDUploadData(); });

    //
    // Safari commonly requests these automatically.  They intentionally
    // have no representation, so answer with HTTP 204 No Content.
    //

    m_server.on("/favicon.ico", HTTP_GET,
        [this]() { m_server.send(204); });

    m_server.on("/apple-touch-icon.png", HTTP_GET,
        [this]() { m_server.send(204); });

    m_server.on("/apple-touch-icon-precomposed.png", HTTP_GET,
        [this]() { m_server.send(204); });

    m_server.onNotFound(
        [this]() { handleNotFound(); });

    m_server.begin();
    return true;
}

void WebServerManager::update()
{
    // First valid clock observation: log transition exactly once per boot.
    // This does not wait for NTP and never interferes with instrument control.
    if (!m_timeSyncLogged)
    {
        char stamp[64];
        if (diagnosticTimestamp(stamp, sizeof(stamp)))
        {
            char entry[128];
            snprintf(entry, sizeof(entry), "%s TIME_SYNC clock available\n", stamp);
            m_console.print(entry);
            appendDiagnosticLog(entry);
            m_timeSyncLogged = true;
        }
    }

    m_server.handleClient();

    if (m_otaRestartPending && static_cast<uint32_t>(millis() - m_otaRestartAt) >= 1500U) {
        ESP.restart();
        return;
    }

    if (m_wifiSetupPending && static_cast<uint32_t>(millis() - m_wifiSetupAt) >= 1200U) {
        m_wifiSetupPending = false;
        // Port 80 must be released before WiFiManager starts its setup server.
        m_server.stop();
        if (!m_wifiManager.enterUserProvisioning()) {
            m_console.println("Wi-Fi: User-initiated provisioning failed; restarting.");
            ESP.restart();
        }
        return;
    }

    // Start only after /macro-start has returned to the browser.
    if (m_macroPending && !m_macroRunning)
        executePendingMacro();
}

void WebServerManager::handleRoot()
{
    // Explicit name= (including empty) bypasses startup auto-selection.
    if (!m_server.hasArg("name") && !m_server.hasArg("macro")) {
        File f = SD.open("/config/startup_workspace.txt", FILE_READ);
        if (f) {
            String name = f.readStringUntil('\n'); f.close(); name.trim();
            if (validSavedConfigurationName(name) && name != "default" &&
                SD.exists(("/config/workspaces/" + name + ".cfg").c_str())) {
                m_server.sendHeader("Location", "/?name=" + name, true);
                m_server.sendHeader("Cache-Control", "no-store");
                m_server.send(302, "text/plain", "");
                return;
            }
        }
    }
    sendPage("", "", "");
}


void WebServerManager::handleSDView()
{
    if (!m_server.hasArg("path"))
    {
        m_server.send(400, "text/plain", "Missing SD-card path.");
        return;
    }

    String path = m_server.arg("path");
    path.trim();

    // Use the same path restrictions as Download/Delete.  View is read-only,
    // but still accepts only an absolute ordinary-file path.
    if (path.length() < 2 ||
        !path.startsWith("/") ||
        path.endsWith("/") ||
        path.indexOf("..") >= 0 ||
        path.indexOf('\\') >= 0)
    {
        m_server.send(400, "text/plain", "Invalid SD-card path.");
        return;
    }

    File file = SD.open(path.c_str(), FILE_READ);

    if (!file || file.isDirectory())
    {
        if (file)
            file.close();
        m_server.send(404, "text/plain", "SD-card file not found.");
        return;
    }

    m_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    m_server.sendHeader("Cache-Control", "no-store");
    m_server.send(
        200,
        "text/html; charset=utf-8",
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>SD File View</title>"
        "<style>body{font-family:Arial,sans-serif;margin:24px;color:#222}"
        ".path{font-family:monospace;font-size:16px;font-weight:bold;margin-bottom:14px;overflow-wrap:anywhere}"
        "pre{font-family:monospace;font-size:14px;line-height:1.35;white-space:pre;overflow:auto;"
        "border:1px solid #ccc;padding:14px;background:#fafafa}</style></head><body>"
        "<div class=\"path\">"
    );

    sendEscaped(path.c_str());
    sendChunk("</div><pre>");

    // buffer escaped output to avoid thousands of tiny
    // chunked HTTP writes for large text files. RAM usage stays bounded.
    char output[1024];
    size_t used = 0;
    uint8_t input[512];

    auto flushOutput = [&]()
    {
        if (used != 0)
        {
            m_server.sendContent(output, used);
            used = 0;
        }
    };

    while (file.available())
    {
        const size_t count = file.read(input, sizeof(input));
        if (count == 0)
            break;

        for (size_t i = 0; i < count; ++i)
        {
            const unsigned char c = input[i];
            const char* entity = nullptr;
            switch (c)
            {
                case '&': entity = "&amp;"; break;
                case '<': entity = "&lt;"; break;
                case '>': entity = "&gt;"; break;
                case '\0': entity = "&#0;"; break;
                default: break;
            }

            if (entity != nullptr)
            {
                const size_t n = strlen(entity);
                if (used + n > sizeof(output))
                    flushOutput();
                memcpy(output + used, entity, n);
                used += n;
            }
            else
            {
                if (used == sizeof(output))
                    flushOutput();
                output[used++] = static_cast<char>(c);
            }
        }
    }
    flushOutput();

    file.close();
    sendChunk("</pre></body></html>");
    m_server.sendContent("");
}


void WebServerManager::handleSDDelete()
{
    if (!m_server.hasArg("path"))
    {
        m_server.send(400, "text/plain", "Missing SD-card path.");
        return;
    }

    String path = m_server.arg("path");
    path.trim();

    // Accept only an absolute file path.  Do not allow traversal,
    // backslashes, the root directory, or a trailing directory separator.
    if (path.length() < 2 ||
        !path.startsWith("/") ||
        path.endsWith("/") ||
        path.indexOf("..") >= 0 ||
        path.indexOf('\\') >= 0)
        {
        m_server.send(400, "text/plain", "Invalid SD-card path.");
        return;
    }

    File file = SD.open(path.c_str(), FILE_READ);

    if (!file)
    {
        m_server.send(404, "text/plain", "SD-card file not found.");
        return;
    }

    if (file.isDirectory())
    {
        file.close();
        m_server.send(400, "text/plain", "Directory deletion is not supported.");
        return;
    }

    file.close();

    if (!SD.remove(path.c_str()))
    {
        m_server.send(500, "text/plain", "Unable to delete SD-card file.");
        return;
    }

    // asynchronous deletion returns a simple success response.
    // Legacy form submissions retain their original redirect behavior.
    if (m_server.hasArg("ajax") && m_server.arg("ajax") == "1")
    {
        m_server.send(200, "text/plain", "Deleted.");
        return;
    }

    m_server.sendHeader("Location", "/", true);
    m_server.send(303, "text/plain", "");
}


void WebServerManager::handleMacroStart()
{
    if (m_macroPending || m_macroRunning)
    {
        m_server.send(409, "text/plain", "A macro is already running.");
        return;
    }

    if (!m_server.hasArg("macro_file"))
    {
        m_server.send(400, "text/plain", "Missing selected macro file.");
        return;
    }

    String requestedMacro = m_server.arg("macro_file");
    requestedMacro.trim();

    //
    // Accept only a simple visible .cfg filename that is actually present
    // in /macros.  The browser is never allowed to supply an arbitrary
    // SD-card pathname.
    //
    if (requestedMacro.length() <= 4 ||
        requestedMacro.length() > 100 ||
        requestedMacro[0] == '.' ||
        requestedMacro.indexOf('/') >= 0 ||
        requestedMacro.indexOf('\\') >= 0 ||
        requestedMacro.indexOf("..") >= 0 ||
        !requestedMacro.endsWith(".cfg"))
    {
        m_server.send(400, "text/plain", "Invalid selected macro file.");
        return;
    }

    bool foundMacro = false;
    File directory = SD.open("/macros");

    if (directory && directory.isDirectory())
    {
        File entry = directory.openNextFile();

        while (entry)
        {
            if (!entry.isDirectory())
            {
                const char* rawName = entry.name();
                const char* baseName = strrchr(rawName, '/');
                baseName = (baseName != nullptr) ? baseName + 1 : rawName;

                if (baseName[0] != '.' &&
                    requestedMacro == baseName)
                {
                    foundMacro = true;
                    entry.close();
                    break;
                }
            }

            entry.close();
            entry = directory.openNextFile();
        }
    }

    directory.close();

    if (!foundMacro)
    {
        m_server.send(400, "text/plain", "Selected macro file was not found.");
        return;
    }

    char macroFile[128];

    const int pathLength =
        snprintf(
            macroFile,
            sizeof(macroFile),
            "/macros/%s",
            requestedMacro.c_str());

    if (pathLength < 0 ||
        static_cast<size_t>(pathLength) >= sizeof(macroFile))
    {
        m_server.send(400, "text/plain", "Selected macro path is too long.");
        return;
    }

    static MacroConfiguration::Macro macro;

    const MacroConfiguration::Result loadResult =
        m_macroConfiguration.load(macroFile, macro);

    if (loadResult != MacroConfiguration::Result::Success)
    {
        String body = "Macro configuration error: ";
        body += MacroConfiguration::resultText(loadResult);
        m_server.send(400, "text/plain", body);
        return;
    }

    if (macro.parameterCount > WebMacroParameterLimit)
    {
        m_server.send(
            400,
            "text/plain",
            "Macro has too many parameters for the web executor.");
        return;
    }

    m_macroOverrideCount = macro.parameterCount;

    for (size_t i = 0; i < macro.parameterCount; ++i)
    {
        const MacroConfiguration::Parameter& parameter = macro.parameters[i];

        if (!m_server.hasArg(parameter.name))
        {
            String body = "Missing macro parameter: ";
            body += parameter.name;
            m_server.send(400, "text/plain", body);
            return;
        }

        String value = m_server.arg(parameter.name);
        value.trim();

        if (value.length() == 0)
        {
            String body = "Empty macro parameter: ";
            body += parameter.name;
            m_server.send(400, "text/plain", body);
            return;
        }

        if (!copyText(
                m_macroOverrideNames[i],
                sizeof(m_macroOverrideNames[i]),
                parameter.name) ||
            !copyText(
                m_macroOverrideValues[i],
                sizeof(m_macroOverrideValues[i]),
                value.c_str()))
        {
            m_server.send(
                400,
                "text/plain",
                "Macro parameter name or value is too long.");
            return;
        }
    }

    //
    // Preflight every step before accepting the selected macro.  Ordinary
    // parameters are fully expanded as before.  Runtime variables introduced
    // by an earlier QUERY ... AS name step are represented by a harmless
    // numeric placeholder so later {name}, {name + constant}, and
    // {name - constant} expressions can be syntax-checked before any
    // instrument I/O begins.
    //
    MacroConfiguration::ValueOverride
        preflightOverrides[WebMacroParameterLimit];

    for (size_t i = 0; i < m_macroOverrideCount; ++i)
    {
        preflightOverrides[i].name = m_macroOverrideNames[i];
        preflightOverrides[i].value = m_macroOverrideValues[i];
    }

    m_macroChangesSD = false;

    static constexpr size_t PreflightRuntimeLimit = 16;
    char preflightRuntimeNames[PreflightRuntimeLimit][32];
    MacroConfiguration::RuntimeValue
        preflightRuntimeValues[PreflightRuntimeLimit];
    size_t preflightRuntimeCount = 0;

    for (size_t i = 0; i < PreflightRuntimeLimit; ++i)
    {
        preflightRuntimeNames[i][0] = '\0';
        preflightRuntimeValues[i].name = preflightRuntimeNames[i];
        preflightRuntimeValues[i].value = "0";
    }

    for (size_t step = 0; step < macro.stepCount; ++step)
    {
        char expanded[224];

        const MacroConfiguration::Result expansionResult =
            m_macroConfiguration.expandStep(
                macro,
                step,
                preflightOverrides,
                m_macroOverrideCount,
                preflightRuntimeValues,
                preflightRuntimeCount,
                expanded,
                sizeof(expanded));

        if (expansionResult != MacroConfiguration::Result::Success)
        {
            char body[192];

            snprintf(
                body,
                sizeof(body),
                "Macro validation failed at step %u: %s",
                static_cast<unsigned int>(step + 1),
                MacroConfiguration::resultText(expansionResult));

            m_server.send(400, "text/plain", body);
            return;
        }

        // remember whether this macro can change the SD-card
        // file listing.  The browser uses this after successful completion
        // to reload the page so newly-created log files become visible.
        if (strncmp(expanded, "LOG ", 4) == 0)
            m_macroChangesSD = true;

        // A successful QUERY ... AS name makes that runtime variable
        // available to subsequent steps.  The actual value is not known
        // until execution, so preflight records only the name.
        char stepCopy[sizeof(expanded)];
        copyText(stepCopy, sizeof(stepCopy), expanded);

        char* save = nullptr;
        char* verb = strtok_r(stepCopy, " \t", &save);
        char* instrument = strtok_r(nullptr, " \t", &save);
        char* capability = strtok_r(nullptr, " \t", &save);
        char* asToken = strtok_r(nullptr, " \t", &save);
        char* captureName = strtok_r(nullptr, " \t", &save);
        char* extra = strtok_r(nullptr, " \t", &save);

        const char* runtimeName = nullptr;

        if (verb != nullptr && strcmp(verb, "QUERY") == 0 &&
            instrument != nullptr && capability != nullptr &&
            asToken != nullptr && strcmp(asToken, "AS") == 0 &&
            captureName != nullptr && extra == nullptr)
        {
            runtimeName = captureName;
        }
        else if (verb != nullptr && strcmp(verb, "SWEEP") == 0 &&
                 instrument != nullptr && capability != nullptr &&
                 asToken != nullptr && captureName != nullptr &&
                 extra == nullptr)
        {
            // SWEEP <variable> <start> <stop> <increment> creates the
            // sweep variable before execution enters the loop body.
            runtimeName = instrument;
        }

        if (runtimeName != nullptr)
        {
            bool alreadyKnown = false;

            for (size_t i = 0; i < preflightRuntimeCount; ++i)
            {
                if (strcmp(preflightRuntimeNames[i], runtimeName) == 0)
                {
                    alreadyKnown = true;
                    break;
                }
            }

            if (!alreadyKnown)
            {
                if (preflightRuntimeCount >= PreflightRuntimeLimit ||
                    !copyText(
                        preflightRuntimeNames[preflightRuntimeCount],
                        sizeof(preflightRuntimeNames[preflightRuntimeCount]),
                        runtimeName))
                {
                    char body[192];

                    snprintf(
                        body,
                        sizeof(body),
                        "Macro validation failed at step %u: too many or invalid runtime variables",
                        static_cast<unsigned int>(step + 1));

                    m_server.send(400, "text/plain", body);
                    return;
                }

                ++preflightRuntimeCount;
            }
        }
    }

    if (!copyText(m_macroFile, sizeof(m_macroFile), macroFile) ||
        !copyText(m_macroName, sizeof(m_macroName), macro.name))
    {
        m_server.send(400, "text/plain", "Selected macro name or path is too long.");
        return;
    }

    m_macroCompletedSteps = 0;
    m_macroTotalSteps = macro.stepCount;
    m_macroCurrentStep = (macro.stepCount > 0) ? 1 : 0;
    m_macroResult[0] = '\0';
    m_macroFinished = false;
    m_macroSucceeded = false;
    m_macroAborted = false;
    m_macroAbortRequested = false;
    m_macroPending = true;

    m_server.send(202, "text/plain", "Macro accepted.");
}

void WebServerManager::handleMacroAbort()
{
    if (!m_macroPending && !m_macroRunning)
    {
        m_server.send(409, "text/plain", "No macro is running.");
        return;
    }

    m_macroAbortRequested = true;
    m_server.send(202, "text/plain", "Macro abort requested.");
}

void WebServerManager::macroResultCallback(const char* name, const char* value, void* context)
{
    if (context) static_cast<WebServerManager*>(context)->updateMacroResult(name, value);
}

void WebServerManager::updateMacroResult(const char* name, const char* value)
{
    for (size_t i = 0; i < m_macroResultCount; ++i)
        if (strcmp(m_macroResultValues[i].name, name) == 0)
        {
            copyText(m_macroResultValues[i].value, sizeof(m_macroResultValues[i].value), value);
            m_macroResultValues[i].available = true;
            break;
        }
}

void WebServerManager::handleMacroStatus()
{
    char body[384];

    const char* state = "idle";

    if (m_macroRunning || m_macroPending)
        state = "running";
    else if (m_macroFinished)
    {
        if (m_macroAborted)
            state = "aborted";
        else
            state = m_macroSucceeded ? "success" : "error";
    }

    snprintf(
        body,
        sizeof(body),
        "{\"state\":\"%s\",\"name\":\"%s\","
        "\"completed\":%u,\"total\":%u,\"current\":%u,"
        "\"sd_changed\":%s,\"result\":\"%s\"}",
        state,
        m_macroName,
        static_cast<unsigned int>(m_macroCompletedSteps),
        static_cast<unsigned int>(m_macroTotalSteps),
        static_cast<unsigned int>(m_macroCurrentStep),
        m_macroChangesSD ? "true" : "false",
        m_macroResult);

    m_server.sendHeader("Cache-Control", "no-store");
    String json(body);
    if (json.endsWith("}")) json.remove(json.length()-1);
    json += ",\"results\":{";
    for (size_t i = 0; i < m_macroResultCount; ++i)
    {
        if (i) json += ',';
        String key(m_macroResultValues[i].name);
        String value(m_macroResultValues[i].available ? m_macroResultValues[i].value : "");
        key.replace("\\", "\\\\"); key.replace("\"", "\\\"");
        value.replace("\\", "\\\\"); value.replace("\"", "\\\"");
        value.replace("\n", "\\n"); value.replace("\r", "\\r");
        json += "\"" + key + "\":\"" + value + "\"";
    }
    json += "}}";
    m_server.send(200, "application/json", json);
}

void WebServerManager::macroProgressCallback(
    size_t completedSteps,
    size_t totalSteps,
    size_t currentStep,
    void* context)
{
    if (context != nullptr)
    {
        static_cast<WebServerManager*>(context)->updateMacroProgress(
            completedSteps,
            totalSteps,
            currentStep);
    }
}

bool WebServerManager::macroCancellationCallback(void* context)
{
    if (context == nullptr)
        return false;

    return static_cast<WebServerManager*>(context)->m_macroAbortRequested;
}

void WebServerManager::updateMacroProgress(
    size_t completedSteps,
    size_t totalSteps,
    size_t currentStep)
{
    m_macroCompletedSteps = completedSteps;
    m_macroTotalSteps = totalSteps;
    m_macroCurrentStep = currentStep;

    // MacroExecutor is synchronous, so Application::update() cannot run
    // during the macro.  Service the shared activity monitor here as part
    // of the cooperative progress callback.  This preserves the same
    // 500 ms OLED activity indication used during ordinary operation.
    m_activityMonitor.update();

    // /macro-start is already complete, so it is safe to service the
    // browser's status connection while the macro itself is running.
    m_server.handleClient();
}

void WebServerManager::executePendingMacro()
{
    m_macroPending = false;
    m_macroRunning = true;
    m_macroResultCount = 0;
    for (auto& field : m_macroResultValues) { field.name[0] = '\0'; field.value[0] = '\0'; field.available = false; }
    static MacroConfiguration::Macro resultMacro;
    if (m_macroConfiguration.load(m_macroFile, resultMacro) == MacroConfiguration::Result::Success)
    {
        m_macroResultCount = resultMacro.resultCount;
        for (size_t i = 0; i < m_macroResultCount; ++i)
            copyText(m_macroResultValues[i].name, sizeof(m_macroResultValues[i].name), resultMacro.results[i].name);
    }
    m_macroExecutor.setResultCallback(&WebServerManager::macroResultCallback, this);

    MacroConfiguration::ValueOverride overrides[WebMacroParameterLimit];

    for (size_t i = 0; i < m_macroOverrideCount; ++i)
    {
        overrides[i].name = m_macroOverrideNames[i];
        overrides[i].value = m_macroOverrideValues[i];
    }

    // Record accepted execution before entering the synchronous executor.
    const uint32_t macroStartMs = millis();
    appendMacroHistory(m_macroFile, "START", nullptr, 0, false,
                       overrides, m_macroOverrideCount);

    const MacroExecutor::Result result =
        m_macroExecutor.execute(
            m_macroFile,
            overrides,
            m_macroOverrideCount,
            &WebServerManager::macroProgressCallback,
            this,
            &WebServerManager::macroCancellationCallback,
            this);

    m_macroExecutor.setResultCallback(nullptr, nullptr);
    const uint32_t macroElapsedMs = static_cast<uint32_t>(millis() - macroStartMs);
    const char* historyEvent =
        (result == MacroExecutor::Result::Success) ? "COMPLETE" :
        (result == MacroExecutor::Result::Cancelled) ? "ABORT" : "FAIL";
    appendMacroHistory(m_macroFile, historyEvent,
                       MacroExecutor::resultText(result), macroElapsedMs, true);

    m_macroRunning = false;
    m_macroFinished = true;
    m_macroSucceeded = (result == MacroExecutor::Result::Success);
    m_macroAborted = (result == MacroExecutor::Result::Cancelled);
    m_macroAbortRequested = false;
    m_macroCurrentStep = 0;

    if (m_macroSucceeded)
    {
        m_macroCompletedSteps = m_macroTotalSteps;
        copyText(
            m_macroResult,
            sizeof(m_macroResult),
            "Macro completed successfully.");
    }
    else if (m_macroAborted)
    {
        copyText(
            m_macroResult,
            sizeof(m_macroResult),
            "Macro aborted by user.");
    }
    else
    {
        snprintf(
            m_macroResult,
            sizeof(m_macroResult),
            "Macro failed: %s",
            MacroExecutor::resultText(result));
    }

    m_server.handleClient();
}

// add the underlying transport failure to the existing result.
// Called immediately after execute(), before another command can overwrite it.
String WebServerManager::commandResultMessage(const char* generalResult)
{
    String message = generalResult ? generalResult : "";
    const CommandExecutor::TransportDiagnostic& d =
        m_commandExecutor.lastTransportDiagnostic();

    if (d.transport == CommandExecutor::Transport::None)
        return message;

    message += ": ";
    if (d.instrumentName != nullptr && d.instrumentName[0] != '\0')
    {
        message += d.instrumentName;
        message += " (";
    }
    else
        message += "(";

    switch (d.transport)
    {
        case CommandExecutor::Transport::GPIB:
            message += "GPIB address ";
            message += String(d.gpibAddress);
            break;
        case CommandExecutor::Transport::RS232:
            message += "RS-232 port ";
            message += String(d.serialPort);
            break;
        case CommandExecutor::Transport::TCP:
            message += "TCP port ";
            message += String(d.tcpPort);
            break;
        default:
            break;
    }
    message += "): ";
    message += m_commandExecutor.lastTransportErrorText();
    return message;
}

void WebServerManager::handleCommand()
{
    if (!m_server.hasArg("command"))
    {
        sendPage("", "Invalid request: missing command.", "");
        return;
    }

    const String command = m_server.arg("command");

    if (command.length() == 0)
    {
        sendPage("", "Invalid request: empty command.", "");
        return;
    }

    char response[512];
    size_t responseLength = 0;
    response[0] = '\0';

    const CommandExecutor::Result result =
        m_commandExecutor.execute(
            command.c_str(),
            response,
            sizeof(response),
            responseLength,
            5000);

    sendPage(
        command.c_str(),
        commandResultMessage(CommandExecutor::resultText(result)).c_str(),
        response);
}

void WebServerManager::handleCommandAsync()
{
    if (!m_server.hasArg("command"))
    {
        m_server.send(400, "text/plain",
                      "Invalid request: missing command.");
        return;
    }

    const String command = m_server.arg("command");

    if (command.length() == 0)
    {
        m_server.send(400, "text/plain",
                      "Invalid request: empty command.");
        return;
    }

    char response[512];
    size_t responseLength = 0;
    response[0] = '\0';

    const CommandExecutor::Result result =
        m_commandExecutor.execute(
            command.c_str(),
            response,
            sizeof(response),
            responseLength,
            5000);

    String body = commandResultMessage(CommandExecutor::resultText(result));

    if (response[0] != '\0')
    {
        body += '\n';
        body += response;
    }

    if (result == CommandExecutor::Result::Success)
        m_server.send(200, "text/plain", body);
    else
        m_server.send(400, "text/plain", body);
}

void WebServerManager::handleCapability()
{
    if (!m_server.hasArg("instrument") ||
        !m_server.hasArg("capability") ||
        !m_server.hasArg("action"))
    {
        sendPage("", "", "", "", "", "",
                 "Invalid capability request.");
        return;
    }

    const String instrumentSection = m_server.arg("instrument");
    const String capabilitySection = m_server.arg("capability");
    const String action = m_server.arg("action");

    InstrumentDefinition instrument;
    CapabilityDefinition capability;

    if (!loadInstrument(
            instrumentSection.c_str(),
            instrument))
    {
        sendPage("", "", "",
                 instrumentSection.c_str(),
                 capabilitySection.c_str(),
                 "",
                 "Unable to load instrument definition.");
        return;
    }

    if (!loadCapability(
            instrument,
            capabilitySection.c_str(),
            capability))
    {
        sendPage("", "", "",
                 instrumentSection.c_str(),
                 capabilitySection.c_str(),
                 "",
                 "Unable to load capability definition.");
        return;
    }

    char semanticCommand[224];
    semanticCommand[0] = '\0';

    char cardValue[64];
    cardValue[0] = '\0';

    if (action == "read")
    {
        if (!capability.canQuery())
        {
            sendPage("", "", "",
                     instrument.section,
                     capability.section,
                     "",
                     "This capability is not readable.");
            return;
        }

        snprintf(
            semanticCommand,
            sizeof(semanticCommand),
            "QUERY %s %s",
            instrument.section,
            capability.section);
    }
    else if (action == "set")
    {
        if (!capability.canSet())
        {
            sendPage("", "", "",
                     instrument.section,
                     capability.section,
                     "",
                     "This capability is not writable.");
            return;
        }

        if (!m_server.hasArg("value") ||
            m_server.arg("value").length() == 0)
        {
            sendPage("", "", "",
                     instrument.section,
                     capability.section,
                     "",
                     "A value is required.");
            return;
        }

        const String enteredValue = m_server.arg("value");
        double baseValue = 0.0;

        const EngineeringValue::Result parseResult =
            EngineeringValue::parse(
                enteredValue.c_str(),
                capability.quantity,
                capability.baseUnit,
                baseValue);

        if (parseResult != EngineeringValue::Result::Success)
        {
            char message[128];

            snprintf(
                message,
                sizeof(message),
                "Invalid engineering value: %s.",
                EngineeringValue::resultText(parseResult));

            sendPage("", "", "",
                     instrument.section,
                     capability.section,
                     enteredValue.c_str(),
                     message);
            return;
        }

        snprintf(
            semanticCommand,
            sizeof(semanticCommand),
            "SET %s %s %.12g",
            instrument.section,
            capability.section,
            baseValue);

        EngineeringValue::format(
            baseValue,
            capability.quantity,
            capability.baseUnit,
            cardValue,
            sizeof(cardValue));
    }
    else
    {
        sendPage("", "", "",
                 instrument.section,
                 capability.section,
                 "",
                 "Unknown capability action.");
        return;
    }

    char response[512];
    size_t responseLength = 0;
    response[0] = '\0';

    const CommandExecutor::Result result =
        m_commandExecutor.execute(
            semanticCommand,
            response,
            sizeof(response),
            responseLength,
            5000);

    if (result != CommandExecutor::Result::Success)
    {
        sendPage("", "", "",
                 instrument.section,
                 capability.section,
                 cardValue,
                 commandResultMessage(CommandExecutor::resultText(result)).c_str());
        return;
    }

    if (action == "read")
    {
        double baseValue = 0.0;

        if (!extractSemanticValue(
                response,
                capability,
                baseValue))
        {
            sendPage("", "", "",
                     instrument.section,
                     capability.section,
                     "",
                     "Query succeeded, but the response value could not be parsed.");
            return;
        }

        const EngineeringValue::Result formatResult =
            EngineeringValue::format(
                baseValue,
                capability.quantity,
                capability.baseUnit,
                cardValue,
                sizeof(cardValue));

        if (formatResult != EngineeringValue::Result::Success)
        {
            sendPage("", "", "",
                     instrument.section,
                     capability.section,
                     "",
                     "Query succeeded, but the value could not be formatted.");
            return;
        }
    }

    sendPage("", "", "",
             instrument.section,
             capability.section,
             cardValue,
             "Success");
}

void WebServerManager::handleCapabilityAsync()
{
    if (!m_server.hasArg("instrument") ||
        !m_server.hasArg("capability") ||
        !m_server.hasArg("action"))
    {
        m_server.send(400, "text/plain",
                      "Invalid capability request.");
        return;
    }

    const String instrumentSection = m_server.arg("instrument");
    const String capabilitySection = m_server.arg("capability");
    const String action = m_server.arg("action");

    InstrumentDefinition instrument;
    CapabilityDefinition capability;

    if (!loadInstrument(
            instrumentSection.c_str(),
            instrument))
    {
        m_server.send(400, "text/plain",
                      "Unable to load instrument definition.");
        return;
    }

    if (!loadCapability(
            instrument,
            capabilitySection.c_str(),
            capability))
    {
        m_server.send(400, "text/plain",
                      "Unable to load capability definition.");
        return;
    }

    char semanticCommand[224];
    semanticCommand[0] = '\0';

    char formattedValue[64];
    formattedValue[0] = '\0';

    if (action == "execute")
    {
        if (strcmp(capability.type, "action") != 0 ||
            !capability.canExecute())
        {
            m_server.send(400, "text/plain",
                          "This capability is not executable.");
            return;
        }

        snprintf(
            semanticCommand,
            sizeof(semanticCommand),
            "ACTION %s %s",
            instrument.section,
            capability.section);
    }
    else if (action == "toggle")
    {
        if (strcmp(capability.type, "toggle_action") != 0 ||
            !capability.canExecute() ||
            !capability.canExecuteOn())
        {
            m_server.send(400, "text/plain",
                          "This capability is not toggleable.");
            return;
        }

        const String state =
            m_server.hasArg("state") ? m_server.arg("state") : "off";

        if (state == "off")
        {
            snprintf(
                semanticCommand,
                sizeof(semanticCommand),
                "ACTION %s %s",
                instrument.section,
                capability.section);
        }
        else if (state == "on")
        {
            snprintf(
                semanticCommand,
                sizeof(semanticCommand),
                "ACTIONON %s %s",
                instrument.section,
                capability.section);
        }
        else
        {
            m_server.send(400, "text/plain",
                          "Invalid toggle state.");
            return;
        }
    }
    else if (action == "read")
    {
        if (!capability.canQuery())
        {
            m_server.send(400, "text/plain",
                          "This capability is not readable.");
            return;
        }

        snprintf(
            semanticCommand,
            sizeof(semanticCommand),
            "QUERY %s %s",
            instrument.section,
            capability.section);
    }
    else if (action == "set")
    {
        if (!capability.canSet())
        {
            m_server.send(400, "text/plain",
                          "This capability is not writable.");
            return;
        }

        if (!m_server.hasArg("value") ||
            m_server.arg("value").length() == 0)
        {
            m_server.send(400, "text/plain",
                          "A value is required.");
            return;
        }

        const String enteredValue = m_server.arg("value");

        if (strcmp(capability.type, "select") == 0)
        {
            bool validOption = false;
            char optionsCopy[sizeof(capability.options)];

            copyText(optionsCopy, sizeof(optionsCopy), capability.options);

            char* save = nullptr;
            char* item = strtok_r(optionsCopy, ",", &save);

            while (item != nullptr)
            {
                trim(item);
                char* separator = strchr(item, ':');

                if (separator != nullptr)
                {
                    *separator = '\0';
                    char* optionValue = separator + 1;
                    trim(optionValue);

                    if (enteredValue == optionValue)
                    {
                        validOption = true;
                        break;
                    }
                }

                item = strtok_r(nullptr, ",", &save);
            }

            if (!validOption)
            {
                m_server.send(
                    400,
                    "text/plain",
                    "Selected value is not defined by the instrument profile.");
                return;
            }

            snprintf(
                semanticCommand,
                sizeof(semanticCommand),
                "SET %s %s %s",
                instrument.section,
                capability.section,
                enteredValue.c_str());

            copyText(
                formattedValue,
                sizeof(formattedValue),
                enteredValue.c_str());
        }
        else
        {
            double baseValue = 0.0;

            const EngineeringValue::Result parseResult =
                EngineeringValue::parse(
                    enteredValue.c_str(),
                    capability.quantity,
                    capability.baseUnit,
                    baseValue);

            if (parseResult != EngineeringValue::Result::Success)
            {
                char message[128];

                snprintf(
                    message,
                    sizeof(message),
                    "Invalid engineering value: %s.",
                    EngineeringValue::resultText(parseResult));

                m_server.send(400, "text/plain", message);
                return;
            }

            double minimumValue = 0.0;
            double maximumValue = 0.0;
            bool hasMinimum = false;
            bool hasMaximum = false;

            if (capability.minimum[0] != '\0')
            {
                const EngineeringValue::Result minimumResult =
                    EngineeringValue::parse(
                        capability.minimum,
                        capability.quantity,
                        capability.baseUnit,
                        minimumValue);

                if (minimumResult != EngineeringValue::Result::Success)
                {
                    m_server.send(
                        500,
                        "text/plain",
                        "Capability profile has an invalid minimum value.");
                    return;
                }

                hasMinimum = true;
            }

            if (capability.maximum[0] != '\0')
            {
                const EngineeringValue::Result maximumResult =
                    EngineeringValue::parse(
                        capability.maximum,
                        capability.quantity,
                        capability.baseUnit,
                        maximumValue);

                if (maximumResult != EngineeringValue::Result::Success)
                {
                    m_server.send(
                        500,
                        "text/plain",
                        "Capability profile has an invalid maximum value.");
                    return;
                }

                hasMaximum = true;
            }

            if ((hasMinimum && baseValue < minimumValue) ||
                (hasMaximum && baseValue > maximumValue))
            {
                char minimumText[64] = "";
                char maximumText[64] = "";
                char message[192];

                if (hasMinimum)
                {
                    EngineeringValue::format(
                        minimumValue,
                        capability.quantity,
                        capability.baseUnit,
                        minimumText,
                        sizeof(minimumText));
                }

                if (hasMaximum)
                {
                    EngineeringValue::format(
                        maximumValue,
                        capability.quantity,
                        capability.baseUnit,
                        maximumText,
                        sizeof(maximumText));
                }

                if (hasMinimum && hasMaximum)
                {
                    snprintf(
                        message,
                        sizeof(message),
                        "Value must be between %s and %s.",
                        minimumText,
                        maximumText);
                }
                else if (hasMinimum)
                {
                    snprintf(
                        message,
                        sizeof(message),
                        "Value must be at least %s.",
                        minimumText);
                }
                else
                {
                    snprintf(
                        message,
                        sizeof(message),
                        "Value must not exceed %s.",
                        maximumText);
                }

                m_server.send(400, "text/plain", message);
                return;
            }

            snprintf(
                semanticCommand,
                sizeof(semanticCommand),
                "SET %s %s %.12g",
                instrument.section,
                capability.section,
                baseValue);

            const EngineeringValue::Result formatResult =
                EngineeringValue::format(
                    baseValue,
                    capability.quantity,
                    capability.baseUnit,
                    formattedValue,
                    sizeof(formattedValue));

            if (formatResult != EngineeringValue::Result::Success)
            {
                m_server.send(
                    500,
                    "text/plain",
                    "Value parsed, but could not be formatted.");
                return;
            }
        }
    }
    else
    {
        m_server.send(400, "text/plain",
                      "Unknown capability action.");
        return;
    }

    char response[512];
    size_t responseLength = 0;
    response[0] = '\0';

    const CommandExecutor::Result result =
        m_commandExecutor.execute(
            semanticCommand,
            response,
            sizeof(response),
            responseLength,
            5000);

    if (result != CommandExecutor::Result::Success)
    {
        m_server.send(
            500,
            "text/plain",
            commandResultMessage(CommandExecutor::resultText(result)).c_str());
        return;
    }

    if (action == "read")
    {
        if (strcmp(capability.type, "select") == 0)
        {
            // CommandExecutor has already removed any profile-defined raw
            // response prefix/terminator.  Preserve the semantic selection
            // text, trimming only surrounding whitespace for display.
            const char* start = response;

            while (isspace(static_cast<unsigned char>(*start)))
                ++start;

            const char* end = start + strlen(start);

            while (end > start &&
                   isspace(static_cast<unsigned char>(*(end - 1))))
                --end;

            const size_t valueLength =
                static_cast<size_t>(end - start);

            if (valueLength == 0 ||
                valueLength >= sizeof(formattedValue))
            {
                m_server.send(
                    500,
                    "text/plain",
                    "Query returned an invalid selected value.");
                return;
            }

            memcpy(formattedValue, start, valueLength);
            formattedValue[valueLength] = '\0';
        }
        else
        {
            double baseValue = 0.0;

            if (!extractSemanticValue(
                    response,
                    capability,
                    baseValue))
            {
                m_server.send(
                    500,
                    "text/plain",
                    "Query succeeded, but the response value could not be parsed.");
                return;
            }

            const EngineeringValue::Result formatResult =
                EngineeringValue::format(
                    baseValue,
                    capability.quantity,
                    capability.baseUnit,
                    formattedValue,
                    sizeof(formattedValue));

            if (formatResult != EngineeringValue::Result::Success)
            {
                m_server.send(
                    500,
                    "text/plain",
                    "Query succeeded, but the value could not be formatted.");
                return;
            }
        }
    }

    if (action == "execute")
        m_server.send(200, "text/plain", "Success");
    else if (action == "toggle")
    {
        const String state = m_server.arg("state");
        m_server.send(200, "text/plain",
                      state == "off" ? "on" : "off");
    }
    else
        m_server.send(200, "text/plain", formattedValue);
}

bool WebServerManager::loadInstrument(
    const char* section,
    InstrumentDefinition& instrument)
{
    memset(&instrument, 0, sizeof(instrument));

    if (!copyText(
            instrument.section,
            sizeof(instrument.section),
            section))
        return false;

    if (!m_configurationFile.getValue(
            "/config/instruments.cfg",
            section,
            "name",
            instrument.name,
            sizeof(instrument.name)))
        return false;

    if (!m_configurationFile.getValue(
            "/config/instruments.cfg",
            section,
            "profile",
            instrument.profileName,
            sizeof(instrument.profileName)))
        return false;

    char profilePath[160];

    const int pathLength =
        snprintf(
            profilePath,
            sizeof(profilePath),
            "/instruments/%s",
            instrument.profileName);

    if (pathLength < 0 ||
        static_cast<size_t>(pathLength) >= sizeof(profilePath))
        return false;

    if (!m_configurationFile.getValue(
            profilePath,
            "instrument",
            "manufacturer",
            instrument.manufacturer,
            sizeof(instrument.manufacturer)))
        return false;

    if (!m_configurationFile.getValue(
            profilePath,
            "instrument",
            "model",
            instrument.model,
            sizeof(instrument.model)))
        return false;

    if (!m_configurationFile.getValue(
            profilePath,
            "instrument",
            "default_function",
            instrument.defaultFunctionLabel,
            sizeof(instrument.defaultFunctionLabel)))
    {
        copyText(
            instrument.defaultFunctionLabel,
            sizeof(instrument.defaultFunctionLabel),
            "Basic Controls");
    }

    // Display-only connection details from the installed-instrument registry.
    // Missing optional keys do not prevent an instrument from loading.
    const char* registry = "/config/instruments.cfg";
    m_configurationFile.getValue(registry, section, "interface", instrument.interfaceType, sizeof(instrument.interfaceType));
    m_configurationFile.getValue(registry, section, "address", instrument.address, sizeof(instrument.address));
    m_configurationFile.getValue(registry, section, "port", instrument.port, sizeof(instrument.port));
    m_configurationFile.getValue(registry, section, "baud", instrument.baud, sizeof(instrument.baud));
    m_configurationFile.getValue(registry, section, "data_bits", instrument.dataBits, sizeof(instrument.dataBits));
    m_configurationFile.getValue(registry, section, "parity", instrument.parity, sizeof(instrument.parity));
    m_configurationFile.getValue(registry, section, "stop_bits", instrument.stopBits, sizeof(instrument.stopBits));
    m_configurationFile.getValue(registry, section, "host", instrument.host, sizeof(instrument.host));

    return true;
}

bool WebServerManager::loadCapability(
    const InstrumentDefinition& instrument,
    const char* section,
    CapabilityDefinition& capability)
{
    memset(&capability, 0, sizeof(capability));

    if (!copyText(
            capability.section,
            sizeof(capability.section),
            section))
        return false;

    char profilePath[160];

    const int pathLength =
        snprintf(
            profilePath,
            sizeof(profilePath),
            "/instruments/%s",
            instrument.profileName);

    if (pathLength < 0 ||
        static_cast<size_t>(pathLength) >= sizeof(profilePath))
        return false;

    ConfigurationFile::ValueRequest requests[] =
    {
        { "type", capability.type, sizeof(capability.type), true, false },
        { "label", capability.label, sizeof(capability.label), true, false },
        { "quantity", capability.quantity, sizeof(capability.quantity), false, false },
        { "base_unit", capability.baseUnit, sizeof(capability.baseUnit), false, false },
        { "set", capability.setTemplate, sizeof(capability.setTemplate), false, false },
        { "query", capability.queryCommand, sizeof(capability.queryCommand), false, false },
        { "minimum", capability.minimum, sizeof(capability.minimum), false, false },
        { "maximum", capability.maximum, sizeof(capability.maximum), false, false },
        { "response_prefix", capability.responsePrefix, sizeof(capability.responsePrefix), false, false },
        { "response_terminator", capability.responseTerminator, sizeof(capability.responseTerminator), false, false },
        { "response_unit", capability.responseUnit, sizeof(capability.responseUnit), false, false },
        { "options", capability.options, sizeof(capability.options), false, false },
        { "action", capability.actionCommand, sizeof(capability.actionCommand), false, false },
        { "action_on", capability.actionOnCommand, sizeof(capability.actionOnCommand), false, false },
        { "label_on", capability.labelOn, sizeof(capability.labelOn), false, false },
        { "resets_toggle", capability.resetsToggle, sizeof(capability.resetsToggle), false, false },
        { "visible_when", capability.visibleWhen, sizeof(capability.visibleWhen), false, false },
        { "ui", capability.ui, sizeof(capability.ui), false, false }
    };

    if (!m_configurationFile.getValues(
            profilePath,
            section,
            requests,
            sizeof(requests) / sizeof(requests[0])))
        return false;

    if (strcmp(capability.type, "number") == 0)
    {
        if (capability.quantity[0] == '\0' ||
            capability.baseUnit[0] == '\0')
            return false;
    }
    else if (strcmp(capability.type, "select") == 0)
    {
        if (capability.options[0] == '\0')
            return false;
    }
    else if (strcmp(capability.type, "action") == 0)
    {
        if (!capability.canExecute())
            return false;
    }
    else if (strcmp(capability.type, "toggle_action") == 0)
    {
        if (!capability.canExecute() ||
            !capability.canExecuteOn() ||
            capability.labelOn[0] == '\0')
            return false;
    }
    else
    {
        return false;
    }

    if (strcmp(capability.type, "action") != 0 &&
        strcmp(capability.type, "toggle_action") != 0 &&
        !capability.canSet() &&
        !capability.canQuery())
        return false;

    return true;
}

bool WebServerManager::extractSemanticValue(
    const char* response,
    const CapabilityDefinition& capability,
    double& value)
{
    // CommandExecutor already owns instrument-specific response extraction
    // (response_prefix / response_terminator).  The web layer receives the
    // resulting semantic value and must not try to parse the raw instrument
    // response a second time.
    if (response == nullptr)
        return false;

    const char* start = response;

    while (isspace(static_cast<unsigned char>(*start)))
        ++start;

    char* end = nullptr;
    const double parsedValue = strtod(start, &end);

    if (end == start || !isfinite(parsedValue))
        return false;

    while (isspace(static_cast<unsigned char>(*end)))
        ++end;

    // A semantic numeric response must contain only the number plus optional
    // surrounding whitespace.  Any remaining text is unexpected here.
    if (*end != '\0')
        return false;

    // response_unit remains a presentation/engineering-value concern of the
    // web layer.  It describes the unit of the semantic numeric value returned
    // by CommandExecutor, not framing in the raw instrument response.
    if (capability.responseUnit[0] != '\0')
    {
        char valueWithUnit[64];

        const int written =
            snprintf(
                valueWithUnit,
                sizeof(valueWithUnit),
                "%.17g %s",
                parsedValue,
                capability.responseUnit);

        if (written < 0 ||
            static_cast<size_t>(written) >= sizeof(valueWithUnit))
            return false;

        const EngineeringValue::Result conversionResult =
            EngineeringValue::parse(
                valueWithUnit,
                capability.quantity,
                capability.baseUnit,
                value);

        return conversionResult == EngineeringValue::Result::Success;
    }

    value = parsedValue;
    return true;
}

void WebServerManager::renderMacros()
{
    //
    // Discover .cfg macro files in /macros, allow one to be selected, and
    // execute the selected valid macro through the generic MacroExecutor.
    //

    static MacroConfiguration::Macro macro;

    char selectedPath[128] = "/macros/find_signal.cfg";
    bool foundAnyMacro = false;

    String requestedMacro;

    if (m_server.hasArg("macro"))
    {
        requestedMacro = m_server.arg("macro");
        requestedMacro.trim();
    }

    File directory = SD.open("/macros");

    if (!directory || !directory.isDirectory())
    {
        sendChunk(
            "<div class=\"panel\"><h2>Macros</h2>"
            "<div class=\"result\">Unable to open /macros directory.</div>"
            "</div>");
        return;
    }

    //
    // First pass: validate the requested selection against files actually
    // present in /macros.  Never use an arbitrary browser-supplied path.
    //

    File entry = directory.openNextFile();

    while (entry)
    {
        if (!entry.isDirectory())
        {
            const char* rawName = entry.name();
            const char* baseName = strrchr(rawName, '/');
            baseName = (baseName != nullptr) ? baseName + 1 : rawName;

            const size_t nameLength = strlen(baseName);

            if (baseName[0] != '.' &&
                nameLength > 4 &&
                strcmp(baseName + nameLength - 4, ".cfg") == 0)
            {
                foundAnyMacro = true;

                if (requestedMacro.length() > 0 &&
                    requestedMacro == baseName)
                {
                    snprintf(
                        selectedPath,
                        sizeof(selectedPath),
                        "/macros/%s",
                        baseName);
                }
            }
        }

        entry.close();
        entry = directory.openNextFile();
    }

    directory.close();

    if (!foundAnyMacro)
    {
        sendChunk(
            "<div class=\"panel\"><h2>Macros</h2>"
            "<div class=\"result\">No .cfg macro files were found in /macros.</div>"
            "</div>");
        return;
    }

    //
    // If Find Signal is not present, select the first discovered .cfg file.
    //

    if (!SD.exists(selectedPath))
    {
        directory = SD.open("/macros");

        if (directory && directory.isDirectory())
        {
            entry = directory.openNextFile();

            while (entry)
            {
                if (!entry.isDirectory())
                {
                    const char* rawName = entry.name();
                    const char* baseName = strrchr(rawName, '/');
                    baseName = (baseName != nullptr) ? baseName + 1 : rawName;

                    const size_t nameLength = strlen(baseName);

                    if (baseName[0] != '.' &&
                        nameLength > 4 &&
                        strcmp(baseName + nameLength - 4, ".cfg") == 0)
                    {
                        snprintf(
                            selectedPath,
                            sizeof(selectedPath),
                            "/macros/%s",
                            baseName);
                        entry.close();
                        break;
                    }
                }

                entry.close();
                entry = directory.openNextFile();
            }
        }

        directory.close();
    }

    const MacroConfiguration::Result result =
        m_macroConfiguration.load(selectedPath, macro);

    sendChunk("<div class=\"panel\" id=\"macroPanelHost\"><div style=\"display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;margin-bottom:16px\"><h2 style=\"margin:0\">Macros</h2><button id=\"addMacroPanelButton\" type=\"button\" onclick=\"macroAddPanel()\">+ Add Macro Panel</button></div>");

    //
    // Macro selector.  The option label comes from each macro's [macro] name
    // when the file parses successfully; otherwise the filename is shown.
    //

    sendChunk(
        "<form method=\"GET\" action=\"/\" style=\"margin-bottom:14px\">"
        "<label for=\"macroSelect\"><b>Macro</b></label>"
        "<select id=\"macroSelect\" name=\"macro\" "
        "onchange=\"this.form.submit()\" "
        "style=\"box-sizing:border-box;width:100%;padding:10px;"
        "font-size:16px;border:1px solid #aaa;border-radius:5px;"
        "background:white;margin-top:6px\">");

    directory = SD.open("/macros");

    if (directory && directory.isDirectory())
    {
        entry = directory.openNextFile();

        while (entry)
        {
            if (!entry.isDirectory())
            {
                const char* rawName = entry.name();
                const char* baseName = strrchr(rawName, '/');
                baseName = (baseName != nullptr) ? baseName + 1 : rawName;

                const size_t nameLength = strlen(baseName);

                if (baseName[0] != '.' &&
                    nameLength > 4 &&
                    strcmp(baseName + nameLength - 4, ".cfg") == 0)
                {
                    char path[128];

                    snprintf(
                        path,
                        sizeof(path),
                        "/macros/%s",
                        baseName);

                    sendChunk("<option value=\"");
                    sendEscaped(baseName);
                    sendChunk("\"");

                    if (strcmp(path, selectedPath) == 0)
                        sendChunk(" selected");

                    sendChunk(">");

                    // display the filename as the macro
                    // identity, with the .cfg extension removed.
                    char displayName[96];
                    copyText(displayName, sizeof(displayName), baseName);
                    const size_t displayLength = strlen(displayName);
                    if (displayLength > 4 &&
                        strcmp(displayName + displayLength - 4, ".cfg") == 0)
                        displayName[displayLength - 4] = '\0';
                    sendEscaped(displayName);

                    sendChunk("</option>");
                }
            }

            entry.close();
            entry = directory.openNextFile();
        }
    }

    directory.close();

    sendChunk("</select></form>");

    if (result != MacroConfiguration::Result::Success)
    {
        sendChunk("<div class=\"result\">Unable to load ");
        sendEscaped(selectedPath);
        sendChunk(": ");
        sendEscaped(MacroConfiguration::resultText(result));
        sendChunk("</div></div>");
        return;
    }

    sendChunk("<div class=\"instrument-card macro-card\"><h3>");
    sendEscaped(macro.name);
    sendChunk("</h3>");

    if (macro.description[0] != '\0')
    {
        sendChunk("<div class=\"model\">");
        sendEscaped(macro.description);
        sendChunk("</div>");
    }

    sendChunk("<form id=\"macroParameterForm\">");
    sendChunk("<input type=\"hidden\" name=\"macro_file\" value=\"");
    const char* selectedBaseName = strrchr(selectedPath, '/');
    selectedBaseName =
        (selectedBaseName != nullptr) ? selectedBaseName + 1 : selectedPath;
    sendEscaped(selectedBaseName);
    sendChunk("\">");

    if (macro.parameterCount == 0)
    {
        sendChunk("<div class=\"range\">This macro has no parameters.</div>");
    }
    else
    {
    for (size_t i = 0; i < macro.parameterCount; ++i)
    {
        const MacroConfiguration::Parameter& parameter =
            macro.parameters[i];

        sendChunk("<div class=\"capability\"><label for=\"macro_");
        sendEscaped(parameter.name);
        sendChunk("\">");
        sendEscaped(
            parameter.label[0] != '\0'
                ? parameter.label
                : parameter.name);
        sendChunk("</label>");

        if (strcmp(parameter.type, "select") == 0)
        {
            sendChunk("<select id=\"macro_");
            sendEscaped(parameter.name);
            sendChunk("\" name=\"");
            sendEscaped(parameter.name);
            sendChunk("\" style=\"box-sizing:border-box;width:100%;padding:10px;"
                      "font-size:16px;border:1px solid #aaa;border-radius:5px;"
                      "background:white\">");

            char optionsCopy[sizeof(parameter.options)];
            copyText(optionsCopy, sizeof(optionsCopy), parameter.options);

            char* save = nullptr;
            char* item = strtok_r(optionsCopy, ",", &save);

            while (item != nullptr)
            {
                trim(item);

                char* separator = strchr(item, ':');
                const char* optionLabel = item;
                char* optionValue = item;

                if (separator != nullptr)
                {
                    *separator = '\0';
                    optionValue = separator + 1;
                    trim(item);
                    trim(optionValue);
                }

                sendChunk("<option value=\"");
                sendEscaped(optionValue);
                sendChunk("\"");

                if (strcmp(optionValue, parameter.defaultValue) == 0)
                    sendChunk(" selected");

                sendChunk(">");
                sendEscaped(optionLabel);
                sendChunk("</option>");

                item = strtok_r(nullptr, ",", &save);
            }

            sendChunk("</select>");
        }
        else if (strcmp(parameter.type, "boolean") == 0)
        {
            sendChunk("<select id=\"macro_");
            sendEscaped(parameter.name);
            sendChunk("\" name=\"");
            sendEscaped(parameter.name);
            sendChunk("\" style=\"box-sizing:border-box;width:100%;padding:10px;"
                      "font-size:16px;border:1px solid #aaa;border-radius:5px;"
                      "background:white\">"
                      "<option value=\"false\"");

            if (strcmp(parameter.defaultValue, "false") == 0 ||
                strcmp(parameter.defaultValue, "0") == 0)
                sendChunk(" selected");

            sendChunk(">False</option><option value=\"true\"");

            if (strcmp(parameter.defaultValue, "true") == 0 ||
                strcmp(parameter.defaultValue, "1") == 0)
                sendChunk(" selected");

            sendChunk(">True</option></select>");
        }
        else
        {
            sendChunk("<input id=\"macro_");
            sendEscaped(parameter.name);
            sendChunk("\" name=\"");
            sendEscaped(parameter.name);
            sendChunk("\" type=\"text\" value=\"");
            sendEscaped(parameter.defaultValue);
            sendChunk("\">");
        }

        sendChunk("<div class=\"range\">");
        sendEscaped(parameter.name);

        if (parameter.type[0] != '\0')
        {
            sendChunk(" &middot; ");
            sendEscaped(parameter.type);
        }

        if (parameter.quantity[0] != '\0')
        {
            sendChunk(" &middot; ");
            sendEscaped(parameter.quantity);

            if (parameter.baseUnit[0] != '\0')
            {
                sendChunk(" (");
                sendEscaped(parameter.baseUnit);
                sendChunk(")");
            }
        }

        sendChunk("</div></div>");
    }
    }

    char initialProgress[128];

    snprintf(
        initialProgress,
        sizeof(initialProgress),
        "<div id=\"macroProgressText\" class=\"range\" "
        "style=\"margin-top:5px\">0 of %u steps completed (0%%)</div>",
        static_cast<unsigned int>(macro.stepCount));

    if (macro.resultCount > 0)
    {
        sendChunk("<div class=\"macro-results\" style=\"margin-top:14px\"><b>Results</b>");
        for (size_t i = 0; i < macro.resultCount; ++i)
        {
            sendChunk("<div style=\"margin-top:7px\"><label>");
            sendEscaped(macro.results[i].label);
            sendChunk("</label><div style=\"display:flex;gap:8px;align-items:center\"><input type=\"text\" readonly "
                      "data-result-name=\"");
            sendEscaped(macro.results[i].name);
            sendChunk("\" data-result-unit=\"");
            sendEscaped(macro.results[i].unit);
            sendChunk("\" value=\"\" placeholder=\"—\" style=\"flex:1;min-width:0\"></div></div>");
        }
        sendChunk("</div>");
    }

    sendChunk(
        "<button id=\"macroExecuteButton\" type=\"button\" "
        "data-mode=\"execute\" onclick=\"macroButtonAction(this)\">Execute Macro</button>"
        "<div id=\"macroStatusText\" class=\"range\" "
        "style=\"margin-top:10px\"></div>"
        "<div style=\"width:100%;height:18px;background:#e5e5e5;"
        "border-radius:9px;overflow:hidden;margin-top:8px\">"
        "<div id=\"macroProgressBar\" style=\"height:100%;width:0%;"
        "background:#666;transition:width .2s\"></div></div>");

    sendChunk(initialProgress);
    sendChunk("</form>");

    sendChunk(String("<script>const macroBootId=") + String(macroWorkspaceBootId) + ";</script>");
    sendChunk(R"AK3Y(<script>
// v0.86(a): macro panels are browser-side; the executor remains single-instance.
let macroActiveCard=null, macroPollTimer=null, macroMaxPct=0, macroLoading=false;
const macroHost=document.getElementById('macroPanelHost');
const macroOriginalSelector=document.getElementById('macroSelect');
const macroNames=[...macroOriginalSelector.options].map(o=>({value:o.value,label:o.textContent}));
const macroStateKey='ak3y_macro_panels_v086';
function macroSavePanels(){
 if(macroRestoring)return;
 const panels=[...macroGrid.querySelectorAll('.macro-panel')].map(w=>({
  name:w.querySelector('select')?.value||'',
  values:[...(w.querySelector('.macro-card form')?.elements||[])].filter(e=>e.name&&e.name!=='macro_file').map(e=>[e.name,e.value])
 }));
 try{sessionStorage.setItem(macroStateKey,JSON.stringify({boot:macroBootId,panels}));}catch(e){}
 if(typeof snapshotWorkspace==='function')snapshotWorkspace();
}
let macroRestoring=false,macroRestoredFromSession=false;

function macroCards(){return [...macroHost.querySelectorAll('.macro-card')];}
function macroElements(card){return {b:card.querySelector('[data-mode]'),st:card.querySelector('[id="macroStatusText"]'),pt:card.querySelector('[id="macroProgressText"]'),pb:card.querySelector('[id="macroProgressBar"]'),form:card.querySelector('form[id="macroParameterForm"]')};}
function macroBusyButtons(){macroCards().forEach(c=>{let e=macroElements(c);if(e.b)e.b.disabled=!!macroActiveCard&&c!==macroActiveCard;});macroUpdateRemoveButtons();}
function macroFormatResult(raw,unit){
 const value=String(raw??'').trim();
 if(!value||!/^[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?$/.test(value))return {value:raw,unit};
 const n=Number(value);
 if(!Number.isFinite(n))return {value:raw,unit};
 if(unit==='Hz'){
  const abs=Math.abs(n);
  const scale=abs>=1e9?1e9:abs>=1e6?1e6:abs>=1e3?1e3:1;
  const suffix=scale===1e9?'GHz':scale===1e6?'MHz':scale===1e3?'kHz':'Hz';
  // Preserve 1 Hz display resolution across engineering frequency units.
  // GHz: 9 decimals; MHz: 6; kHz: 3; Hz: integer.
  // This is display resolution, not a claim of instrument accuracy.
  const decimals=scale===1e9?9:scale===1e6?6:scale===1e3?3:0;
  return {value:(n/scale).toFixed(decimals),unit:suffix};
 }
 if(unit==='dBm')return {value:Number(n.toPrecision(8)).toString(),unit};
 return {value:raw,unit};
}
function macroShowStatus(s){
 if(!macroActiveCard)return;
 const e=macroElements(macroActiveCard),total=Number(s.total||0),done=Number(s.completed||0),current=Number(s.current||0);
 if(s.results){macroActiveCard.querySelectorAll('[data-result-name]').forEach(input=>{
  const key=input.getAttribute('data-result-name');
  const raw=Object.prototype.hasOwnProperty.call(s.results,key)?s.results[key]:'';
  const originalUnit=input.getAttribute('data-result-unit')||'';
  const display=macroFormatResult(raw,originalUnit);
  input.value=display.value==null?'':String(display.value)+(display.unit?' '+display.unit:'');
 });}
 const pct=total?Math.round(100*done/total):0;macroMaxPct=Math.max(macroMaxPct,pct);
 e.pb.style.width=macroMaxPct+'%';e.pt.textContent=done+' of '+total+' steps completed ('+pct+'%)';
 if(s.state==='running'){e.b.disabled=false;e.b.dataset.mode='abort';e.b.textContent='Abort Macro';e.st.textContent=current?'Executing '+(s.name||'macro')+' — Step '+current+' of '+total:'Executing '+(s.name||'macro')+'...';}
 else{e.b.dataset.mode='execute';e.b.textContent='Execute Macro';e.b.disabled=false;
 e.st.textContent=s.result||(s.state==='success'?'Macro completed successfully.':s.state==='aborted'?'Macro aborted.':'Macro failed.');
 macroActiveCard=null;macroBusyButtons();if(s.sd_changed)e.st.textContent+=' SD-card files may have changed; refresh the SD file list when needed.';}
}
async function macroPoll(){
 try{const r=await fetch('/macro-status',{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);
 const s=await r.json();macroShowStatus(s);if(s.state==='running')macroPollTimer=setTimeout(macroPoll,200);else macroPollTimer=null;
 }catch(e){if(macroActiveCard){macroElements(macroActiveCard).st.textContent='Status request failed: '+e;macroPollTimer=setTimeout(macroPoll,500);}}
}
async function macroButtonAction(button){
 const card=button.closest('.macro-card'),e=macroElements(card);
 if(button.dataset.mode==='abort'){
 button.disabled=true;e.st.textContent='Requesting macro abort...';
 try{const r=await fetch('/macro-abort',{method:'POST',cache:'no-store'});const t=await r.text();if(!r.ok)throw Error(t);e.st.textContent='Abort requested...';}
 catch(err){e.st.textContent='Abort request failed: '+err;button.disabled=false;}
 return;
 }
 if(macroActiveCard){e.st.textContent='Another macro is running.';return;}
 macroActiveCard=card;macroMaxPct=0;macroBusyButtons();e.b.disabled=true;
 card.querySelectorAll('[data-result-name]').forEach(input=>input.value='');
 e.st.textContent='Starting macro...';e.pb.style.width='0%';e.pt.textContent='0 steps completed (0%)';
 try{const r=await fetch('/macro-start',{method:'POST',body:new FormData(e.form)});const t=await r.text();if(!r.ok)throw Error(t);
 e.b.dataset.mode='abort';e.b.textContent='Abort Macro';e.b.disabled=false;macroPoll();}
 catch(err){e.st.textContent='Macro start failed: '+err;e.b.disabled=false;macroActiveCard=null;macroBusyButtons();}
}
function macroUpdateRemoveButtons(){
 if(typeof workspaceUpdateMacroBusyButtons==='function')workspaceUpdateMacroBusyButtons();
 const wrappers=[...macroGrid.querySelectorAll('.macro-panel')];
 const add=document.getElementById('addMacroPanelButton');
 if(add){add.disabled=macroLoading||!!macroActiveCard;add.title=macroLoading?'Wait for macro parameters to finish loading':'';}
 wrappers.forEach(w=>{
  const b=w.querySelector('.macro-remove');
  if(b){b.disabled=!!macroActiveCard||macroLoading;b.title='';}
  const sel=w.querySelector('select');if(sel)sel.disabled=macroLoading||!!macroActiveCard;
 });
}
function macroRemovePanel(button){
 if(macroActiveCard||macroLoading)return;
 const wrapper=button.closest('.macro-panel');if(!wrapper)return;
 if(!window.confirm('Remove this Macro Panel from the current workspace? No macro commands will be executed.'))return;
 wrapper.remove();macroUpdateRemoveButtons();macroScheduleLayout();macroSavePanels();
}
async function macroLoadPanel(wrapper,name){
 if(macroActiveCard||macroLoading)return;
 const status=wrapper.querySelector('.macro-load-status');
 if(!name){wrapper.querySelector('.macro-card')?.remove();status.textContent='Select a macro to load its parameters.';macroScheduleLayout();return;}
 macroLoading=true;macroUpdateRemoveButtons();
 status.textContent='Loading macro parameters... please wait.';
 const selector=wrapper.querySelector('select');
 try{
  const r=await fetch('/?macro='+encodeURIComponent(name),{cache:'no-store'});
  if(!r.ok)throw Error('HTTP '+r.status);
  const html=await r.text(),doc=new DOMParser().parseFromString(html,'text/html');
  const card=doc.querySelector('#macroPanelHost .macro-card');
  const loaded=doc.querySelector('#macroPanelHost #macroSelect');
  if(!card||!loaded||loaded.value!==name)throw Error('Macro response did not match the selection');
  if(!wrapper.isConnected||selector.value!==name)return;
  wrapper.querySelector('.macro-card')?.remove();wrapper.appendChild(card);
  status.textContent='';macroBusyButtons();macroSavePanels();
 }catch(err){status.textContent='Unable to load macro: '+err;}
 finally{macroLoading=false;macroUpdateRemoveButtons();macroScheduleLayout();}
}
function macroMakePanel(selector){
 const wrapper=document.createElement('div');wrapper.className='panel macro-panel';
 wrapper.style.cssText='min-width:0;margin:0;padding:18px;box-sizing:border-box';
 const controls=document.createElement('div');controls.style.cssText='display:flex;gap:10px;align-items:end;flex-wrap:wrap;margin-bottom:12px';
 const choice=document.createElement('div');choice.style.cssText='flex:1;min-width:160px';
 const label=document.createElement('label');label.textContent='Macro';label.style.fontWeight='bold';
 selector.style.cssText='box-sizing:border-box;width:100%;padding:10px;font-size:16px;border:1px solid #aaa;border-radius:5px;background:white;margin-top:6px';
 choice.append(label,selector);
 const remove=document.createElement('button');remove.type='button';remove.className='macro-remove';remove.textContent='Remove Panel';remove.onclick=()=>macroRemovePanel(remove);
 controls.append(choice,remove);
 const status=document.createElement('div');status.className='macro-load-status';status.style.marginBottom='8px';
 wrapper.append(controls,status);macroGrid.appendChild(wrapper);
 selector.removeAttribute('onchange');selector.addEventListener('change',()=>{macroSavePanels();macroLoadPanel(wrapper,selector.value);});
 wrapper.addEventListener('input',macroSavePanels);wrapper.addEventListener('change',macroSavePanels);
 macroUpdateRemoveButtons();macroScheduleLayout();return wrapper;
}
function macroAddPanel(){
 if(macroActiveCard||macroLoading)return;
 const selector=document.createElement('select');
 const placeholder=document.createElement('option');
 placeholder.value='';placeholder.textContent='Select Macro...';placeholder.disabled=true;placeholder.selected=true;
 selector.appendChild(placeholder);
 macroNames.forEach(m=>{if(!m.value)return;const o=document.createElement('option');o.value=m.value;o.textContent=m.label;selector.appendChild(o);});
 const wrapper=macroMakePanel(selector);
 wrapper.querySelector('.macro-load-status').textContent='Select a macro to load its parameters.';
 macroSavePanels();
}
const macroGrid=document.createElement('div');macroGrid.id='macroPanelGrid';
macroGrid.style.cssText='display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,390px),1fr));gap:16px;grid-auto-rows:8px;grid-auto-flow:row dense;align-items:start';
let macroLayoutPending=false;
function macroLayout(){
 macroLayoutPending=false;const boxes=[...macroGrid.children];
 if(getComputedStyle(macroGrid).gridTemplateColumns.split(' ').length<=1){boxes.forEach(b=>b.style.gridRowEnd='auto');return;}
 const css=getComputedStyle(macroGrid),row=parseFloat(css.gridAutoRows)||8,gap=parseFloat(css.rowGap)||16;
 boxes.forEach(b=>{b.style.gridRowEnd='auto';});
 boxes.forEach(b=>{b.style.gridRowEnd='span '+Math.max(1,Math.ceil((b.getBoundingClientRect().height+gap)/(row+gap)));});
}
function macroScheduleLayout(){if(macroLayoutPending)return;macroLayoutPending=true;requestAnimationFrame(macroLayout);}
if(typeof ResizeObserver!=='undefined'){const ro=new ResizeObserver(macroScheduleLayout);
 new MutationObserver(()=>{[...macroGrid.children].forEach(b=>ro.observe(b));macroScheduleLayout();}).observe(macroGrid,{childList:true});}
window.addEventListener('resize',macroScheduleLayout);
const initialForm=macroOriginalSelector.closest('form');
const initialCard=macroHost.querySelector('.macro-card');
macroHost.insertBefore(macroGrid,initialForm);
const first=macroMakePanel(macroOriginalSelector);
if(initialCard)first.appendChild(initialCard);
initialForm.remove();
macroUpdateRemoveButtons();macroScheduleLayout();
// Restore additional panels on a page reload, but discard them after reboot.
const macroInitialRestore=(async()=>{
 let saved=null;try{saved=JSON.parse(sessionStorage.getItem(macroStateKey)||'null');}catch(e){}
 if(!performance.getEntriesByType('navigation').some(n=>n.type==='reload')||!saved||saved.boot!==macroBootId||!Array.isArray(saved.panels)||!saved.panels.length){
  try{sessionStorage.removeItem(macroStateKey);}catch(e){}return;
 }
 macroRestoring=true;macroRestoredFromSession=true;
 try{
  const items=saved.panels.slice(0,12);
  // Reuse the first server-rendered panel; create empty panels for the others.
  for(let i=1;i<items.length;i++){
   const sel=document.createElement('select');sel.add(new Option('Select Macro...',''));
   macroNames.forEach(m=>{if(m.value)sel.add(new Option(m.label,m.value));});
   macroMakePanel(sel);
  }
  const wrappers=[...macroGrid.querySelectorAll('.macro-panel')];
  for(let i=0;i<items.length;i++){
   const w=wrappers[i],item=items[i],sel=w.querySelector('select');
   if(!w||!item||!sel)continue;
   if(i===0&&sel.value===item.name){}
   else if(item.name&&macroNames.some(m=>m.value===item.name)){
    sel.value=item.name;
    await macroLoadPanel(w,item.name);
   }else if(i===0){w.querySelector('.macro-card')?.remove();sel.value='';}
   const form=w.querySelector('.macro-card form');
   if(form&&Array.isArray(item.values))item.values.forEach(([key,value])=>{
    const field=[...form.elements].find(e=>e.name===key);
    if(field)field.value=value;
   });
  }
 }finally{macroRestoring=false;macroUpdateRemoveButtons();macroScheduleLayout();macroSavePanels();}
})();
// Apply saved macro panels without executing any macros or changing macro files.
async function macroApplySavedWorkspace(cfg){
 const count=Number(cfg.workspace?.macro_panel_count||0);
 if(!Number.isInteger(count)||count<1||count>12)return;
 macroRestoring=true;
 try{
  while(macroGrid.children.length>1)macroGrid.lastElementChild.remove();
  for(let i=1;i<count;i++){
   const sel=document.createElement('select');sel.add(new Option('Select Macro...',''));
   macroNames.forEach(m=>{if(m.value)sel.add(new Option(m.label,m.value));});
   macroMakePanel(sel);
  }
  const wrappers=[...macroGrid.querySelectorAll('.macro-panel')];
  for(let i=0;i<count;i++){
   const item=cfg['macro'+(i+1)]||{},w=wrappers[i],sel=w.querySelector('select');
   const name=item.file||'';
   if(name&&macroNames.some(m=>m.value===name)){
    sel.value=name;
    if(i!==0||!w.querySelector('.macro-card')||w.querySelector('.macro-card form [name=macro_file]')?.value!==name){
     await macroLoadPanel(w,name);
    }
   }else{
    sel.value='';w.querySelector('.macro-card')?.remove();
    w.querySelector('.macro-load-status').textContent=name?'Saved macro not found: '+name:'Select a macro to load its parameters.';
   }
   const form=w.querySelector('.macro-card form');
   if(form)for(const field of form.elements){
    if(field.name&&field.name!=='macro_file'&&Object.prototype.hasOwnProperty.call(item,'param_'+field.name))
     field.value=item['param_'+field.name];
   }
  }
 }finally{macroRestoring=false;macroUpdateRemoveButtons();macroScheduleLayout();macroSavePanels();}
}
</script>)AK3Y");

    sendChunk("</div></div>");
}

void WebServerManager::sendPage(
    const char* command,
    const char* resultText,
    const char* response,
    const char* cardInstrument,
    const char* cardCapability,
    const char* cardValue,
    const char* cardResult)
{
    m_htmlBufferUsed = 0;
    m_htmlBufferActive = true;
    const uint32_t guiStart = millis();
    uint32_t guiMark = guiStart;
    auto guiTiming = [&](const char* stage) {
        const uint32_t now = millis();
        Serial.printf("[GUI TIMING] %-22s +%lu ms (total %lu ms)\n",
                      stage, (unsigned long)(now - guiMark),
                      (unsigned long)(now - guiStart));
        guiMark = now;
    };

    //
    // Chunked transfer avoids constructing the entire dynamic page in RAM.
    //

    m_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    m_server.send(
        200,
        "text/html; charset=utf-8",
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>AK3Y Instrument Controller</title>");

    sendChunk(
        "<style>"
        "body{font-family:Arial,Helvetica,sans-serif;margin:40px;background:#f5f5f5;color:#222}"
        ".container{max-width:2100px;margin:auto}"
        ".panel{background:white;padding:28px;border-radius:10px;"
        "box-shadow:0 2px 10px rgba(0,0,0,.12);margin-bottom:24px}"
        "h1{margin-top:0}"
        "table{border-collapse:collapse;width:100%;margin-bottom:8px}"
        "td{padding:8px 4px;border-bottom:1px solid #ddd}"
        "td:first-child{font-weight:bold;width:150px}"
        ".instrument-card{border:1px solid #ccc;border-radius:8px;padding:20px;margin-top:16px}"
        ".instrument-card h3{margin:0 0 4px;font-size:24px}"
        ".model{color:#555;margin-bottom:18px}"
        ".function-selector{margin:12px 0 18px}"
        ".function-selector select{box-sizing:border-box;width:100%;padding:10px;font-size:16px;"
        "border:1px solid #aaa;border-radius:5px;background:white}"
        ".capability{padding:14px 0;border-top:1px solid #e2e2e2}"
        ".capability:first-of-type{border-top:0}"
        ".group-marker{border:1px solid #bbb;border-radius:7px;padding:12px 16px;"
        "margin:16px 0;background:#fafafa}"
        ".group-marker h4{margin:0;font-size:19px}"
        "label{display:block;font-weight:bold;margin-bottom:8px}"
        "input{box-sizing:border-box;width:100%;padding:11px;font-size:16px;"
        "border:1px solid #aaa;border-radius:5px}"
        ".command{font-family:monospace}"
        ".buttons{display:flex;gap:10px}"
        "button{margin-top:12px;padding:10px 18px;font-size:16px;cursor:pointer}"
        ".result{margin-top:12px;padding:12px;background:#f3f3f3;border-radius:5px}"
        ".range{margin-top:8px;color:#666;font-size:14px}"
        /* Compact only instrument capability forms; preserve all form fields and handlers. */
        ".control-panel-grid .capability{padding:9px 0}"
        ".control-panel-grid .capability form{display:flex;flex-wrap:wrap;align-items:center;gap:6px 9px}"
        ".control-panel-grid .capability label{flex:0 0 100%;margin:0;font-size:14px}"
        ".control-panel-grid .capability input[name=value],"
        ".control-panel-grid .capability select[name=value]{flex:1 1 145px;min-width:0;width:auto;"
        "box-sizing:border-box;padding:7px 9px;font-size:15px}"
        ".control-panel-grid .capability .buttons{flex:0 0 auto;gap:6px;align-items:center}"
        ".control-panel-grid .capability button{margin:0;padding:7px 12px;font-size:14px}"
        ".control-panel-grid .capability .range{margin:2px 0 0;font-size:13px;line-height:1.4;"
        "color:#555;flex:0 0 100%;overflow-wrap:anywhere}"
        ".control-panel-grid .capability .result{flex-basis:100%;margin-top:3px}"
        "@media(max-width:1050px){.control-panel-grid .capability input[name=value],"
        ".control-panel-grid .capability select[name=value]{flex-basis:100%}}"
        ".control-panel-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,390px),1fr));"
        "gap:18px;grid-auto-rows:8px;grid-auto-flow:row dense;align-items:start;margin-bottom:24px}"
        ".control-panel-grid>.control-box{min-width:0;margin-bottom:0;padding:20px}"
        ".control-panel-grid .instrument-card{min-width:0;padding:14px}"
        ".control-panel-grid .buttons{flex-wrap:wrap}"
        "@media(max-width:850px){.control-panel-grid{grid-template-columns:minmax(0,1fr);grid-auto-rows:auto;grid-auto-flow:row}.control-panel-grid>.control-box{grid-row-end:auto!important}}"
        "@media(max-width:600px){body{margin:12px}.control-panel-grid>.control-box{padding:16px}}"
        ".control-box-status{margin-top:10px;min-height:20px;color:#444;font-size:14px}"
        ".control-panel-toolbar{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;margin-bottom:14px}"
        ".control-panel-heading{display:flex;align-items:center;justify-content:space-between;gap:10px}"
        ".control-panel-heading h2{margin:0 0 12px}"
        ".remove-control-panel{font-size:14px;padding:6px 10px;margin-bottom:12px}"
        ".control-panel-modal[hidden]{display:none!important}"
        ".control-panel-modal{position:fixed;inset:0;z-index:10000;background:rgba(0,0,0,.5);display:flex;align-items:center;justify-content:center;padding:18px}"
        ".control-panel-dialog{background:#fff;border-radius:10px;padding:24px;max-width:420px;width:100%;box-shadow:0 8px 35px rgba(0,0,0,.3)}"
        ".control-panel-dialog h2{margin-top:0}"
        ".control-panel-dialog .buttons{display:flex;justify-content:flex-end;gap:10px;margin-top:20px}"
        "pre{white-space:pre-wrap;word-wrap:break-word;margin-bottom:0}"
        ".ready{font-weight:bold}"
        ".status-strip{display:flex;flex-wrap:wrap;gap:8px 18px;align-items:center;"
        "padding:13px 18px;background:#fff;border-radius:9px;margin-bottom:14px;"
        "box-shadow:0 2px 8px rgba(0,0,0,.09);font-size:14px}"
        ".tab-nav{display:flex;gap:7px;flex-wrap:wrap;margin-bottom:18px}"
        ".tab-nav button{margin:0;padding:11px 14px;border:1px solid #aaa;"
        "border-radius:7px;background:#fff;font-size:14px}"
        ".tab-nav button[aria-selected=true]{background:#204e7c;color:white;border-color:#204e7c}"
        ".tab-panel[hidden]{display:none!important}"
        "#tab-instruments{visibility:hidden}"
        "</style></head><body><div class=\"container\">"
        "<h1>AK3Y Instrument Controller</h1>"
        "<div class=\"status-strip\" aria-label=\"System status\">"
        "<span><b>Wi-Fi:</b> ");
    sendChunk(m_wifiManager.isConnected() ? "Connected" : "Disconnected");
    sendChunk(" <span id=\"wifi-rssi\" aria-live=\"off\">(");
    if (m_wifiManager.isConnected()) {
        char rssiText[24];
        snprintf(rssiText, sizeof(rssiText), "%ld dBm", static_cast<long>(m_wifiManager.rssi()));
        sendChunk(rssiText);
    } else {
        sendChunk("--");
    }
    sendChunk(")</span></span><span><b>SD:</b> ");
    sendChunk(SD.cardType() != CARD_NONE ? "Ready" : "Unavailable");
    sendChunk("</span><span><b>Clock:</b> ");
    sendChunk(time(nullptr) >= static_cast<time_t>(1735689600) ? "Synchronized" : "Uptime only");
    sendChunk("</span><span><b>Firmware:</b> ");
    sendEscaped(Configuration::Firmware::Version);
    sendChunk("</span></div>"
        "<nav class=\"tab-nav\" role=\"tablist\" aria-label=\"Controller sections\">"
        "<button type=\"button\" role=\"tab\" data-tab=\"instruments\" aria-selected=\"true\" onclick=\"showControllerTab('instruments')\">Instruments</button>"
        "<button type=\"button\" role=\"tab\" data-tab=\"macros\" aria-selected=\"false\" onclick=\"showControllerTab('macros')\">Macros</button>"
        "<button type=\"button\" role=\"tab\" data-tab=\"files\" aria-selected=\"false\" onclick=\"showControllerTab('files')\">File Management</button>"
        "<button type=\"button\" role=\"tab\" data-tab=\"about\" aria-selected=\"false\" onclick=\"showControllerTab('about')\">About</button>"
        "<a href=\"/firmware-update\" style=\"display:inline-flex;align-items:center;padding:9px 14px;border:1px solid #aaa;border-radius:7px;background:#fff;color:#204e7c;text-decoration:none;font-size:14px\">Firmware Update</a>"
        "<a href=\"/wifi-settings\" style=\"display:inline-flex;align-items:center;padding:9px 14px;border:1px solid #aaa;border-radius:7px;background:#fff;color:#204e7c;text-decoration:none;font-size:14px\">Update Wi-Fi</a>"
        "</nav>"
        "<section class=\"tab-panel\" id=\"tab-instruments\" role=\"tabpanel\">");

    sendChunk("<div id=\"controlBoxTemplate\" style=\"display:none\">");

    guiTiming("Initial HTML");
    renderInstruments(
        cardInstrument,
        cardCapability,
        cardValue,
        cardResult);

    guiTiming("Instrument rendering");
    sendChunk("</div>");

    // The number of browser control boxes is configuration-driven.
    // Keep 2 as the safe/default value so an absent or invalid web.cfg preserves
    // the previously verified behavior.
    unsigned int controlBoxCount = 0;
    char configuredControlBoxes[16] = {0};

    if (m_configurationFile.getValue(
            "/config/web.cfg",
            "web",
            "control_boxes",
            configuredControlBoxes,
            sizeof(configuredControlBoxes)))
    {
        const long requestedCount = strtol(configuredControlBoxes, nullptr, 10);

        // A modest upper bound protects the browser/UI from an accidental
        // configuration typo while still allowing useful expansion.
        if (requestedCount >= 1 && requestedCount <= 8)
            controlBoxCount = static_cast<unsigned int>(requestedCount);
    }

    // Stage 1: no implicit configuration. An explicit legacy "default" URL
    // remains readable, preserving existing files and backward compatibility.
    String selectedWorkspaceName = m_server.hasArg("name") ? m_server.arg("name") : "";
    if (!validSavedConfigurationName(selectedWorkspaceName) ||
        (selectedWorkspaceName != "default" &&
         !SD.exists((String("/config/workspaces/") + selectedWorkspaceName + ".cfg").c_str())))
        selectedWorkspaceName = "";
    const String selectedWorkspacePath = selectedWorkspaceName == "default"
        ? String("/config/control_boxes.cfg")
        : selectedWorkspaceName.length()
          ? String("/config/workspaces/") + selectedWorkspaceName + ".cfg"
          : String("");
    if (!selectedWorkspaceName.length()) controlBoxCount = 0;

    // a saved workspace count overrides the initial web.cfg
    // default. Older workspace files without [workspace] retain web.cfg.
    char savedPanelCount[16] = {0};
    if (m_configurationFile.getValue(
            selectedWorkspacePath.c_str(), "workspace", "panel_count",
            savedPanelCount, sizeof(savedPanelCount)))
    {
        char* end = nullptr;
        const long count = strtol(savedPanelCount, &end, 10);
        if (end != savedPanelCount && *end == '\0' && count >= 1 && count <= 8)
            controlBoxCount = static_cast<unsigned int>(count);
    }

    sendChunk("<div class=\"control-panel-toolbar\"><span id=\"controlPanelCount\" aria-live=\"polite\"></span>"
              "<button type=\"button\" id=\"addControlPanelButton\" onclick=\"return addControlPanel();\">+ Add Control Panel</button></div>"
              "<div class=\"control-panel-grid\" id=\"controlPanelGrid\">");

    for (unsigned int box = 1; box <= controlBoxCount; ++box)
    {
        char section[16];
        char savedInstrument[32] = "";
        char savedFunction[48] = "";
        char boxHeader[320];

        snprintf(section, sizeof(section), "box%u", box);

        // optional, read-only workspace selections.
        // Missing entries simply leave the browser at its normal defaults.
        m_configurationFile.getValue(
            selectedWorkspacePath.c_str(),
            section,
            "instrument",
            savedInstrument,
            sizeof(savedInstrument));

        m_configurationFile.getValue(
            selectedWorkspacePath.c_str(),
            section,
            "function",
            savedFunction,
            sizeof(savedFunction));

        snprintf(
            boxHeader,
            sizeof(boxHeader),
            "<div class=\"panel control-box\" data-saved-instrument=\"");

        sendChunk(boxHeader);
        sendEscaped(savedInstrument);
        sendChunk("\" data-saved-function=\"");
        sendEscaped(savedFunction);

        // stream static HTML instead of truncating a 320-byte
        // snprintf buffer. Only the panel number needs formatting.
        sendChunk("\"><div class=\"control-panel-heading\"><h2>Control Panel ");
        snprintf(boxHeader, sizeof(boxHeader), "%u", box);
        sendChunk(boxHeader);
        sendChunk(
            "</h2><button type=\"button\" class=\"remove-control-panel\" onclick=\"return requestRemoveControlPanel(this);\" aria-label=\"Remove this Control Panel\">&#128465; Remove</button></div>"
            "<div class=\"control-box-content\"></div>"
            "<div class=\"buttons\">"
            "<button type=\"button\" onclick=\"return readControlBox(this);\">READ</button>"
            "<button type=\"button\" onclick=\"return sendControlBox(this);\">SEND</button>"
            "</div>"
            "<div class=\"control-box-status\" aria-live=\"polite\"></div>"
            "</div>");
    }

    sendChunk("</div>"); // end responsive control-panel grid
    sendChunk("<div id=\"removeControlPanelModal\" class=\"control-panel-modal\" hidden>"
              "<div class=\"control-panel-dialog\" role=\"alertdialog\" aria-modal=\"true\" aria-labelledby=\"removeControlPanelTitle\" aria-describedby=\"removeControlPanelDescription\">"
              "<h2 id=\"removeControlPanelTitle\">Remove Control Panel?</h2>"
              "<p id=\"removeControlPanelDescription\">This removes the panel from the current workspace. No instrument commands will be sent.</p>"
              "<div class=\"buttons\"><button type=\"button\" id=\"cancelRemoveControlPanel\" onclick=\"return cancelRemoveControlPanel();\">Cancel</button>"
              "<button type=\"button\" onclick=\"return confirmRemoveControlPanel();\">Remove Panel</button></div></div></div>");

    sendChunk(
        "<div class=\"panel\"><h2>Workspace Configuration</h2>"
        "<p>Save the current Control Panel instruments, functions, and displayed parameter values.</p>"
        "<div class=\"range\" style=\"display:flex;align-items:center;gap:12px;flex-wrap:wrap;margin-bottom:16px\">"
        "<label for=\"workspaceNameSelector\"><b>Saved configuration:</b></label>"
        "<select id=\"workspaceNameSelector\" aria-label=\"Saved configuration\" style=\"min-width:220px;max-width:100%;padding:9px 12px;font-size:16px;box-sizing:border-box\"></select>"
        "<button type=\"button\" id=\"workspaceLoadButton\" style=\"margin:0\" onclick=\"return loadNamedWorkspace();\">Load</button>"
        "<button type=\"button\" id=\"workspaceRenameButton\" style=\"margin:0\" onclick=\"return renameWorkspaceConfiguration();\">Rename...</button>"
        "<button type=\"button\" id=\"workspaceDeleteButton\" style=\"margin:0\" onclick=\"return deleteWorkspaceConfiguration();\">Delete...</button>"
        "<span id=\"workspaceSelectionStatus\"></span></div>"
        "<div style=\"display:flex;align-items:center;margin:0 0 12px 0\">"
        "<label for=\"workspaceStartupCheckbox\" style=\"display:inline-flex;align-items:center;gap:12px;"
        "font-size:16px;font-weight:600;margin:0;cursor:pointer\">"
        "<input type=\"checkbox\" id=\"workspaceStartupCheckbox\" "
        "style=\"width:22px;height:22px;min-width:22px;flex:0 0 22px;padding:0;margin:0;cursor:pointer;accent-color:#1673cf\" "
        "onchange=\"return changeStartupWorkspace(this);\">"
        "Load this configuration when the controller opens</label>"
        "<span style=\"margin-left:16px;color:#666;font-size:14px\">* Startup configuration</span></div>"
        "<div class=\"buttons\" style=\"align-items:center\"><button id=\"workspaceSaveButton\" type=\"button\" style=\"margin-top:0\" onclick=\"return saveWorkspaceConfiguration();\">"
        "Save Configuration</button><button type=\"button\" id=\"workspaceNewButton\" style=\"margin-top:0\" onclick=\"return newWorkspace();\">New Workspace</button><button type=\"button\" id=\"workspaceSaveAsButton\" style=\"margin-top:0\" onclick=\"return saveWorkspaceAs();\">Save As...</button><button id=\"workspaceRevertButton\" type=\"button\" style=\"margin-top:0\" onclick=\"return revertWorkspaceConfiguration();\">Revert Configuration</button><span id=\"workspaceDirtyIndicator\" style=\"display:none;margin-left:12px;font-weight:bold;color:#a65c00\">Unsaved Changes</span></div>"
        "<div class=\"range\">Saving or loading the workspace does not send commands to any instrument.</div>"
        "</div>");

    guiTiming("Control boxes/setup");
    sendChunk(
        "<div class=\"panel\"><h2>Advanced Command Console</h2>"
        "<form id=\"commandForm\" method=\"post\" action=\"/command\" "
        "onsubmit=\"return asyncCommand(this);\">"
        "<label for=\"command\">Command</label>"
        "<input class=\"command\" id=\"command\" name=\"command\" type=\"text\" "
        "placeholder=\"QUERY RFGEN frequency\" value=\"");

    sendEscaped(command);

    sendChunk(
        "\"><button type=\"submit\">Execute</button></form>"
        "<div class=\"result\" id=\"commandResult\" style=\"display:none\"></div>");

    if (resultText != nullptr &&
        resultText[0] != '\0')
    {
        sendChunk("<div class=\"result\"><strong>Result:</strong> ");
        sendEscaped(resultText);

        if (response != nullptr &&
            response[0] != '\0')
        {
            sendChunk("<br><br><strong>Response:</strong><pre>");
            sendEscaped(response);
            sendChunk("</pre>");
        }

        sendChunk("</div>");
    }


    sendChunk("</section><section class=\"tab-panel\" id=\"tab-macros\" role=\"tabpanel\" hidden>");
    renderMacros();
    guiTiming("Macro rendering");
    sendChunk("</section><section class=\"tab-panel\" id=\"tab-files\" role=\"tabpanel\" hidden>");

    sendChunk(
        "<div class=\"panel\"><h2>Create SD Card Directory</h2>"
        "<form method=\"post\" action=\"/sd-mkdir\" onsubmit=\"return createSDDirectory(this);\">"
        "<label for=\"sdDirectoryParent\">Parent folder</label>"
        "<select id=\"sdDirectoryParent\" name=\"parent\" style=\"box-sizing:border-box;width:100%;"
        "padding:10px;font-size:16px;border:1px solid #aaa;border-radius:5px;background:white\">"
        "<option value=\"/\">/ (root)</option>");

    File directoryRoot = SD.open("/");
    if (directoryRoot && directoryRoot.isDirectory())
    {
        File entry = directoryRoot.openNextFile();
        while (entry)
        {
            if (entry.isDirectory())
            {
                const char* name = entry.name();
                const char* baseName = strrchr(name, '/');
                baseName = (baseName != nullptr) ? baseName + 1 : name;

                if (baseName[0] != '.')
                {
                    sendChunk("<option value=\"");
                    if (name[0] != '/')
                        sendChunk("/");
                    sendEscaped(name);
                    sendChunk("\">");
                    if (name[0] != '/')
                        sendChunk("/");
                    sendEscaped(name);
                    sendChunk("</option>");
                }
            }

            entry.close();
            entry = directoryRoot.openNextFile();
        }
        directoryRoot.close();
    }

    sendChunk(
        "</select>"
        "<label for=\"sdDirectoryName\" style=\"margin-top:14px\">New directory name</label>"
        "<input id=\"sdDirectoryName\" name=\"dirname\" type=\"text\" required "
        "maxlength=\"79\" placeholder=\"macros\">"
        "<button type=\"submit\">Create Directory</button>"
        "</form>"
        "<div class=\"range\">Creates one directory inside the selected existing folder.</div>"
        "</div>");

    sendChunk(
        "<div class=\"panel\"><h2>SD Card File Upload</h2>"
        "<form id=\"sdUploadForm\" method=\"post\" action=\"/sd-upload\" "
        "enctype=\"multipart/form-data\" onsubmit=\"return confirmSDOverwrite(this);\">"
        "<label for=\"sdFolder\">SD-card folder</label>"
        "<select id=\"sdFolder\" name=\"folder\" style=\"box-sizing:border-box;width:100%;"
        "padding:10px;font-size:16px;border:1px solid #aaa;border-radius:5px;background:white\">"
        "<option value=\"/\">/ (root)</option>");

    File uploadRoot = SD.open("/");
    if (uploadRoot && uploadRoot.isDirectory())
    {
        File entry = uploadRoot.openNextFile();
        while (entry)
        {
            if (entry.isDirectory())
            {
                const char* name = entry.name();
                const char* baseName = strrchr(name, '/');
                baseName = (baseName != nullptr) ? baseName + 1 : name;
                if (baseName[0] == '.')
                {
                    entry.close();
                    entry = uploadRoot.openNextFile();
                    continue;
                }
                sendChunk("<option value=\"");
                if (name[0] != '/')
                    sendChunk("/");
                sendEscaped(name);
                sendChunk("\">");
                if (name[0] != '/')
                    sendChunk("/");
                sendEscaped(name);
                sendChunk("</option>");
            }
            entry.close();
            entry = uploadRoot.openNextFile();
        }
        uploadRoot.close();
    }

    sendChunk(
        "</select>"
        "<label for=\"sdSource\" style=\"margin-top:14px\">File on computer</label>"
        "<input id=\"sdSource\" name=\"upload\" type=\"file\" required "
        "onchange=\"if(this.files.length){document.getElementById('sdName').value=this.files[0].name;}\">"
        "<label for=\"sdName\" style=\"margin-top:14px\">Save on SD card as</label>"
        "<input id=\"sdName\" name=\"filename\" type=\"text\" required "
        "placeholder=\"filename.cfg\">"
        "<button type=\"submit\">Upload to SD Card</button>"
        "</form>"
        "<div class=\"range\">Maximum upload size: 256 KB (262,144 bytes). " 
        "If a file with the same name already exists, "
        "you will be asked to confirm replacement before the upload begins.</div>"
        "</div>");

    // grouped SD-card downloads; hidden entries are omitted.
    sendChunk(
        "<div class=\"panel\" id=\"sdFilePanel\"><div style=\"display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap\"><h2>SD Card Files</h2><button type=\"button\" onclick=\"refreshSDFilePanel().catch(e=>alert(e.message))\">Refresh File List</button></div>"
        "<p>View, edit, download, or delete individual files; remove empty non-system directories.</p>");

    bool foundDownloadFile = false;
    bool rootHeadingShown = false;
    File downloadRoot = SD.open("/");
    if (downloadRoot && downloadRoot.isDirectory())
    {
        File entry = downloadRoot.openNextFile();
        while (entry)
        {
            String entryName = entry.name();
            int entrySlash = entryName.lastIndexOf('/');
            String entryBase = (entrySlash >= 0) ? entryName.substring(entrySlash + 1) : entryName;

            if (!entryBase.startsWith("."))
            {
                if (entry.isDirectory())
                {
                    String directoryPath = entryName;
                    if (!directoryPath.startsWith("/"))
                        directoryPath = String("/") + directoryPath;

                    File directory = SD.open(directoryPath.c_str());
                    if (directory && directory.isDirectory())
                    {
                        // always display the directory, including
                        // empty directories and directories containing hidden files.
                        String normalized = directoryPath; normalized.toLowerCase();
                        bool protectedDirectory = normalized == "/config" || normalized == "/instruments" || normalized == "/macros" || normalized == "/logs" ||
                            normalized.startsWith("/config/") || normalized.startsWith("/instruments/") || normalized.startsWith("/macros/") || normalized.startsWith("/logs/");
                        // Count hidden regular files; never offer bulk removal if
                        // another entry (visible or directory) is present.
                        unsigned hiddenCount = 0;
                        bool otherEntries = false;
                        File scan = directory.openNextFile();
                        while (scan) {
                            String scanName = scan.name();
                            int scanSlash = scanName.lastIndexOf('/');
                            String base = scanName.substring(scanSlash + 1);
                            if (!scan.isDirectory() && base.startsWith(".")) ++hiddenCount;
                            else otherEntries = true;
                            scan.close();
                            scan = directory.openNextFile();
                        }
                        bool directoryEmpty = !hiddenCount && !otherEntries;
                        directory.rewindDirectory();

                        sendChunk("<div style=\"display:flex;align-items:center;gap:10px;margin-top:18px;padding-bottom:5px;border-bottom:1px solid #ddd\"><strong style=\"flex:1;font-size:14px;letter-spacing:.04em\">");
                        String heading = entryBase;
                        heading.toUpperCase();
                        sendEscaped(heading.c_str());
                        if (directoryEmpty) sendChunk(" (empty)");
                        if (hiddenCount) {
                            sendChunk(" (");
                            sendChunk(String(hiddenCount));
                            sendChunk(hiddenCount == 1 ? " hidden file)" : " hidden files)");
                        }
                        sendChunk("</strong>");
                        if (hiddenCount && !otherEntries && !protectedDirectory) {
                            sendChunk("<form method=\"post\" action=\"/sd-remove-hidden\" style=\"margin:0\" onsubmit=\"return confirmSDHiddenDelete(this);\"><input type=\"hidden\" name=\"path\" value=\"");
                            sendEscaped(directoryPath.c_str());
                            sendChunk("\"><button type=\"submit\">Remove Hidden Files</button></form>");
                        }
                        if (directoryEmpty && !protectedDirectory)
                        {
                            sendChunk("<form method=\"post\" action=\"/sd-rmdir\" style=\"margin:0\" onsubmit=\"return confirmSDDirectoryDelete(this);\"><input type=\"hidden\" name=\"path\" value=\"");
                            sendEscaped(directoryPath.c_str());
                            sendChunk("\"><button type=\"submit\">Remove Empty Directory</button></form>");
                        }
                        sendChunk("</div>");
                        File child = directory.openNextFile();
                        while (child)
                        {
                            String childName = child.name();
                            int childSlash = childName.lastIndexOf('/');
                            String childBase = (childSlash >= 0) ? childName.substring(childSlash + 1) : childName;

                            if (!child.isDirectory() && !childBase.startsWith("."))
                            {
                                String fullPath = childName.startsWith("/") ? childName : directoryPath + "/" + childName;
                                sendChunk("<div style=\"display:flex;align-items:center;gap:8px;padding:5px 0;border-bottom:1px solid #f0f0f0\"><span style=\"flex:1;font-family:monospace;font-size:15px;overflow-wrap:anywhere\">");
                                sendEscaped(childBase.c_str());
                                sendChunk("</span><span style=\"min-width:100px;text-align:right;font-family:monospace;font-size:13px;white-space:nowrap;color:#555\">");
                                sendChunk(formatFileBytes(child.size()));
                                sendChunk(" bytes");
                                sendChunk("</span>");
                                if ((fullPath.endsWith(".cfg") || fullPath.endsWith(".txt")) &&
                                    !fullPath.startsWith("/logs/")) {
                                    sendChunk("<form method=\"get\" action=\"/sd-edit\" target=\"_blank\" style=\"margin:0\"><input type=\"hidden\" name=\"path\" value=\"");
                                    sendEscaped(fullPath.c_str());
                                    sendChunk("\"><button type=\"submit\" style=\"margin:0;padding:5px 12px;font-size:14px\">Edit</button></form>");
                                }
                                sendChunk("<form method=\"get\" action=\"/sd-view\" target=\"_blank\" style=\"margin:0\"><input type=\"hidden\" name=\"path\" value=\"");
                                sendEscaped(fullPath.c_str());
                                sendChunk("\"><button type=\"submit\" style=\"margin:0;padding:5px 12px;font-size:14px\">View</button></form><form method=\"get\" action=\"/sd-download\" style=\"margin:0\"><input type=\"hidden\" name=\"path\" value=\"");
                                sendEscaped(fullPath.c_str());
                                sendChunk("\"><button type=\"submit\" style=\"margin:0;padding:5px 12px;font-size:14px\">Download</button></form><form method=\"post\" action=\"/sd-delete\" style=\"margin:0\" onsubmit=\"return confirmSDDelete(this);\"><input type=\"hidden\" name=\"path\" value=\"");
                                sendEscaped(fullPath.c_str());
                                sendChunk("\"><button type=\"submit\" style=\"margin:0;padding:5px 12px;font-size:14px\">Delete</button></form></div>");
                                foundDownloadFile = true;
                            }
                            child.close();
                            child = directory.openNextFile();
                        }
                        directory.close();
                    }
                }
                else
                {
                    if (!rootHeadingShown)
                    {
                        sendChunk("<div style=\"margin-top:18px;padding-bottom:5px;border-bottom:1px solid #ddd;font-weight:bold;font-size:14px;letter-spacing:.04em\">ROOT</div>");
                        rootHeadingShown = true;
                    }

                    String fullPath = entryName;
                    if (!fullPath.startsWith("/"))
                        fullPath = String("/") + fullPath;

                    sendChunk("<div style=\"display:flex;align-items:center;gap:8px;padding:5px 0;border-bottom:1px solid #f0f0f0\"><span style=\"flex:1;font-family:monospace;font-size:15px;overflow-wrap:anywhere\">");
                    sendEscaped(entryBase.c_str());
                                sendChunk("</span><span style=\"min-width:100px;text-align:right;font-family:monospace;font-size:13px;white-space:nowrap;color:#555\">");
                                sendChunk(formatFileBytes(entry.size()));
                                sendChunk(" bytes");
                    sendChunk("</span>");
                                if ((fullPath.endsWith(".cfg") || fullPath.endsWith(".txt")) &&
                                    !fullPath.startsWith("/logs/")) {
                                    sendChunk("<form method=\"get\" action=\"/sd-edit\" target=\"_blank\" style=\"margin:0\"><input type=\"hidden\" name=\"path\" value=\"");
                                    sendEscaped(fullPath.c_str());
                                    sendChunk("\"><button type=\"submit\" style=\"margin:0;padding:5px 12px;font-size:14px\">Edit</button></form>");
                                }
                                sendChunk("<form method=\"get\" action=\"/sd-view\" target=\"_blank\" style=\"margin:0\"><input type=\"hidden\" name=\"path\" value=\"");
                    sendEscaped(fullPath.c_str());
                    sendChunk("\"><button type=\"submit\" style=\"margin:0;padding:5px 12px;font-size:14px\">View</button></form><form method=\"get\" action=\"/sd-download\" style=\"margin:0\"><input type=\"hidden\" name=\"path\" value=\"");
                    sendEscaped(fullPath.c_str());
                    sendChunk("\"><button type=\"submit\" style=\"margin:0;padding:5px 12px;font-size:14px\">Download</button></form><form method=\"post\" action=\"/sd-delete\" style=\"margin:0\" onsubmit=\"return confirmSDDelete(this);\"><input type=\"hidden\" name=\"path\" value=\"");
                    sendEscaped(fullPath.c_str());
                    sendChunk("\"><button type=\"submit\" style=\"margin:0;padding:5px 12px;font-size:14px\">Delete</button></form></div>");
                    foundDownloadFile = true;
                }
            }

            entry.close();
            entry = downloadRoot.openNextFile();
        }
        downloadRoot.close();
    }

    if (!foundDownloadFile)
        sendChunk("<div class=\"range\">No files found on the SD card.</div>");

    sendChunk("<div class=\"range\" style=\"margin-top:14px\">Hidden entries are omitted. Only empty, non-system directories may be removed.</div></div>");
    sendChunk("</section>"); // Close File Management before sibling tabs.
    // Read-only project identification and a snapshot of runtime resources.
    // These pages do not modify instrument or workspace state.
    sendChunk("<section class=\"tab-panel\" id=\"tab-about\" role=\"tabpanel\" hidden>"
              "<div class=\"panel\"><h2>About AK3Y Instrument Controller</h2>"
              "<p>Browser-based controller for IEEE-488 (GPIB), RS-232, and networked instrumentation.</p>"
              "<table><tr><td>Project</td><td>AK3Y Instrument Controller</td></tr>"
              "<tr><td>Author</td><td>Bob Fontana, AK3Y</td></tr>"
              "<tr><td>Firmware version</td><td>");
    sendEscaped(Configuration::Firmware::Version);
    sendChunk("</td></tr><tr><td>Firmware build</td><td>" __DATE__ " " __TIME__ "</td></tr>"
              "</table><p>Instrument profiles and macros are stored on the microSD card.</p>"
              "<h3>System Information</h3>"
              "<p>Resource values are snapshots taken when this page was loaded. Refresh to update them.</p>"
              "<table>");

    char infoBuffer[128];
    snprintf(infoBuffer, sizeof(infoBuffer),
             "<tr><td>Uptime</td><td>%lu seconds</td></tr>",
             static_cast<unsigned long>(millis() / 1000UL));
    sendChunk(infoBuffer);
    snprintf(infoBuffer, sizeof(infoBuffer),
             "<tr><td>Free heap</td><td>%lu bytes</td></tr>",
             static_cast<unsigned long>(ESP.getFreeHeap()));
    sendChunk(infoBuffer);
    snprintf(infoBuffer, sizeof(infoBuffer),
             "<tr><td>Firmware image</td><td>%lu bytes</td></tr>",
             static_cast<unsigned long>(ESP.getSketchSize()));
    sendChunk(infoBuffer);
    snprintf(infoBuffer, sizeof(infoBuffer),
             "<tr><td>Available application space</td><td>%lu bytes</td></tr>",
             static_cast<unsigned long>(ESP.getFreeSketchSpace()));
    sendChunk(infoBuffer);
    snprintf(infoBuffer, sizeof(infoBuffer),
             "<tr><td>Flash capacity</td><td>%lu bytes</td></tr>",
             static_cast<unsigned long>(ESP.getFlashChipSize()));
    sendChunk(infoBuffer);
    sendChunk("<tr><td>Wi-Fi</td><td>");
    sendChunk(m_wifiManager.isConnected() ? "Connected" : "Disconnected");
    sendChunk("</td></tr><tr><td>SSID</td><td>");
    sendEscaped(m_wifiManager.ssid());
    sendChunk("</td></tr><tr><td>Hostname</td><td>");
    sendEscaped(m_wifiManager.hostname());
    sendChunk("</td></tr><tr><td>IP address</td><td>");
    sendChunk(m_wifiManager.ipAddress());
    sendChunk("</td></tr><tr><td>SD card</td><td>");
    sendChunk(SD.cardType() != CARD_NONE ? "Detected" : "Unavailable");
    sendChunk("</td></tr></table></div></section>");

    sendChunk("<section class=\"tab-panel\" id=\"tab-instruments-extra\" hidden>");

    sendChunk(
        "<div class=\"panel ready\">Instrument Controller Ready</div>"
        "<script>"
        "function showControllerTab(name){"
        "var panels=document.querySelectorAll('.tab-panel[role=tabpanel]');"
        "for(var i=0;i<panels.length;i++){panels[i].hidden=(panels[i].id!=='tab-'+name);}"
        "var buttons=document.querySelectorAll('.tab-nav [data-tab]');"
        "for(var j=0;j<buttons.length;j++){buttons[j].setAttribute('aria-selected',buttons[j].getAttribute('data-tab')===name?'true':'false');}"
        "try{sessionStorage.setItem('ak3y_active_tab',name);}catch(e){}"
        "return false;}"
        "(function(){var name='instruments';try{name=sessionStorage.getItem('ak3y_active_tab')||name;}catch(e){}"
        "if(['instruments','macros','files','about'].indexOf(name)<0){name='instruments';}"
        "showControllerTab(name);})();"
        "function refreshWifiRSSI(){"
        "fetch('/wifi-rssi',{cache:'no-store'}).then(function(r){if(!r.ok)throw Error();return r.text();})"
        ".then(function(t){var el=document.getElementById('wifi-rssi');if(el){"
        "var n=Number(t);el.textContent=t==='Disconnected'?'(--)' : "
        "(Number.isFinite(n)?'('+n+' dBm)':'(--)');}})"
        ".catch(function(){var el=document.getElementById('wifi-rssi');if(el)el.textContent='(--)';});}"
        "setInterval(refreshWifiRSSI,10000);"
        "var capabilityBusy=false;"
        // Restored browser selection is not proof of instrument mode.
        "function ensureFunctionEntered(form){"
        "var card=form.closest('[data-instrument-card]');"
        "var sel=card?card.querySelector('[data-function-selector]'):null;"
        "if(!sel){return Promise.resolve();}"
        "var opt=sel.options[sel.selectedIndex];"
        "var always=!!(opt&&opt.getAttribute('data-enter-before-action')==='true');"
        "if(!always&&sel.getAttribute('data-function-activated')==='yes'){return Promise.resolve();}"
        "var fn=sel.value,inst=sel.getAttribute('data-instrument');"
        "var body='instrument='+encodeURIComponent(inst)+'&function='+encodeURIComponent(fn);"
        "return fetch('/function-async',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})"
        ".then(function(r){return r.text().then(function(t){if(!r.ok)throw Error(t);return t;});})"
        ".then(function(t){if(sel.value!==fn)throw Error('Function selection changed');"
        "sel.setAttribute('data-function-activated','yes');});}"
        "function asyncCapability(button,action){"
        "if(capabilityBusy){return false;}"
        "var form=button.form;"
        "if(action==='set'&&!form.reportValidity()){return false;}"
        "capabilityBusy=true;"
        "var buttons=document.querySelectorAll('button');"
        "for(var i=0;i<buttons.length;i++){buttons[i].disabled=true;}"
        "var body='instrument='+encodeURIComponent(form.elements.instrument.value)"
        "+'&capability='+encodeURIComponent(form.elements.capability.value)"
        "+'&action='+encodeURIComponent(action);"
        "if(action==='set'){"
        "body+='&value='+encodeURIComponent(form.elements.value.value);"
        "}"
        "(action==='read'?Promise.resolve():ensureFunctionEntered(form)).then(function(){"
        "return fetch('/capability-async',{method:'POST',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});})"
        ".then(function(r){return r.text().then(function(t){"
        "if(!r.ok){throw new Error(t);}return t;});})"
        ".then(function(t){"
        "if(form.elements.value){form.elements.value.value=t;}"
        "if(action==='set'&&form.elements.value&&form.elements.value.tagName==='SELECT'){updateConditionalVisibility();}"
        "if(action==='execute'){"
        "var target=button.getAttribute('data-resets-toggle');"
        "if(target){"
        "var toggles=document.querySelectorAll('[data-toggle-capability]');"
        "for(var j=0;j<toggles.length;j++){"
        "if(toggles[j].getAttribute('data-toggle-capability')===target){"
        "toggles[j].setAttribute('data-state','off');"
        "toggles[j].textContent=toggles[j].getAttribute('data-label-off');"
        "}}}}"
        "})"
        ".catch(function(e){alert('Capability '+action+' failed: '+e.message);})"
        ".finally(function(){"
        "capabilityBusy=false;"
        "for(var i=0;i<buttons.length;i++){buttons[i].disabled=false;}"
        "});"
        "return false;"
        "}"
        "function asyncToggleCapability(button){"
        "if(capabilityBusy){return false;}"
        "var form=button.form;"
        "capabilityBusy=true;"
        "var buttons=document.querySelectorAll('button');"
        "for(var i=0;i<buttons.length;i++){buttons[i].disabled=true;}"
        "var state=button.getAttribute('data-state')||'off';"
        "var body='instrument='+encodeURIComponent(form.elements.instrument.value)"
        "+'&capability='+encodeURIComponent(form.elements.capability.value)"
        "+'&action=toggle&state='+encodeURIComponent(state);"
        "ensureFunctionEntered(form).then(function(){return fetch('/capability-async',{method:'POST',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});})"
        ".then(function(r){return r.text().then(function(t){"
        "if(!r.ok){throw new Error(t);}return t;});})"
        ".then(function(t){"
        "button.setAttribute('data-state',t);"
        "button.textContent=(t==='on')?"
        "button.getAttribute('data-label-on'):button.getAttribute('data-label-off');"
        "})"
        ".catch(function(e){alert('Capability toggle failed: '+e.message);})"
        ".finally(function(){"
        "capabilityBusy=false;"
        "for(var i=0;i<buttons.length;i++){buttons[i].disabled=false;}"
        "});"
        "return false;"
        "}"
        "function asyncFunctionChange(select){"
        "if(capabilityBusy){select.value=select.getAttribute('data-current-function')||'__basic__';return false;}"
        "var previous=select.getAttribute('data-current-function')||'__basic__';"
        "var requested=select.value;"
        "if(requested===previous){updateConditionalVisibility();return false;}"
        "capabilityBusy=true;select.disabled=true;"
        "var buttons=document.querySelectorAll('button');for(var i=0;i<buttons.length;i++){buttons[i].disabled=true;}"
        "var body='instrument='+encodeURIComponent(select.getAttribute('data-instrument'))+'&function='+encodeURIComponent(requested);"
        "fetch('/function-async',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})"
        ".then(function(r){return r.text().then(function(t){if(!r.ok){throw new Error(t);}return t;});})"
        ".then(function(t){select.setAttribute('data-current-function',t);select.setAttribute('data-function-activated','yes');updateConditionalVisibility();})"
        ".catch(function(e){select.value=previous;updateConditionalVisibility();alert('Function change failed: '+e.message);})"
        ".finally(function(){capabilityBusy=false;select.disabled=false;for(var i=0;i<buttons.length;i++){buttons[i].disabled=false;}});"
        "return false;}"
        "function updateInstrumentVisibility(selector){"
        "if(!selector){return;}"
        "var box=selector.closest('.control-box');"
        "if(!box){return;}"
        "var selected=selector.value;"
        "var cards=box.querySelectorAll('[data-instrument-card]');"
        "for(var i=0;i<cards.length;i++){cards[i].style.display=(cards[i].getAttribute('data-instrument-card')===selected)?'':'none';}"
        "updateConditionalVisibility();"
        "}"
        "function updateConditionalVisibility(){"
        "var cards=document.querySelectorAll('[data-instrument-card]');"
        "for(var c=0;c<cards.length;c++){"
        "var functionSelect=cards[c].querySelector('[data-function-selector]');"
        "var selectedFunction=functionSelect?functionSelect.value:'__basic__';"
        "var items=cards[c].querySelectorAll('.capability[data-function]');"
        "for(var i=0;i<items.length;i++){"
        "var functionOK=items[i].getAttribute('data-function')===selectedFunction;"
        "var conditionOK=true;var rule=items[i].getAttribute('data-visible-when')||'';"
        "if(rule){var k=rule.indexOf(':');if(k>0){var source=rule.substring(0,k),expected=rule.substring(k+1),actual='';"
        "var selects=cards[c].querySelectorAll('[data-select-capability]');"
        "for(var j=0;j<selects.length;j++){if(selects[j].getAttribute('data-select-capability')===source){actual=selects[j].value;break;}}"
        "conditionOK=(actual===expected);}}"
        "items[i].style.display=(functionOK&&conditionOK)?'':'none';"
        "}}}"
        "function iniSafe(v){return String(v||'').replace(/[\\r\\n]/g,' ').trim();}"
        "function parseWorkspaceConfig(text){"
        "var result={};var section='';var lines=text.split(/\\r?\\n/);"
        "for(var i=0;i<lines.length;i++){var line=lines[i].trim();"
        "if(!line||line.charAt(0)===';'||line.charAt(0)==='#'){continue;}"
        "if(line.charAt(0)==='['&&line.charAt(line.length-1)===']'){section=line.substring(1,line.length-1);if(!result[section]){result[section]={};}continue;}"
        "var eq=line.indexOf('=');if(eq>0&&section){result[section][line.substring(0,eq).trim()]=line.substring(eq+1).trim();}}"
        "return result;}"
        // reload-only session snapshot. No instrument I/O.
        "var workspaceName=new URLSearchParams(location.search).get('name')||'';"
        "if(workspaceName&&!/^[A-Za-z0-9_-]{1,40}$/.test(workspaceName)){workspaceName='';}"
        "var workspaceReady=false,workspaceSavedState='',workspaceDirty=false;"
        "var workspaceReload=(function(){try{var n=performance.getEntriesByType('navigation');return n.length?n[0].type==='reload':performance.navigation.type===1;}catch(e){return false;}})();"
        "var workspaceSessionKey='ak3y-control-panels-reload-v081b2b';"
        // compare only the values actually persisted by Save.
        "function captureWorkspace(){return buildWorkspaceConfiguration();}"
        "function captureWorkspaceFields(){var boxes=document.querySelectorAll('#controlPanelGrid>.control-box'),out=[];"
        "for(var b=0;b<boxes.length;b++){var fields=boxes[b].querySelectorAll('input,select,textarea'),values=[];"
        "for(var i=0;i<fields.length;i++){values.push(fields[i].type==='checkbox'?!!fields[i].checked:fields[i].value);}out.push(values);}return JSON.stringify(out);}"
        "function snapshotWorkspace(){if(!workspaceReady){return;}try{sessionStorage.setItem(workspaceSessionKey,captureWorkspaceFields());}catch(e){}"
        "workspaceDirty=captureWorkspace()!==workspaceSavedState;var el=document.getElementById('workspaceDirtyIndicator');if(el){el.style.display=workspaceDirty?'inline':'none';}}"
        "function restoreReloadWorkspace(){if(!workspaceReload){return false;}var raw=null;try{raw=sessionStorage.getItem(workspaceSessionKey);}catch(e){}"
        "if(!raw){return false;}var saved;try{saved=JSON.parse(raw);}catch(e){return false;}"
        "if(!Array.isArray(saved)||saved.length<1||saved.length>8){return false;}"
        "var grid=document.getElementById('controlPanelGrid');if(!grid){return false;}"
        "while(grid.querySelectorAll(':scope>.control-box').length<saved.length){addControlPanel();}"
        "while(grid.querySelectorAll(':scope>.control-box').length>saved.length){grid.lastElementChild.remove();}"
        "var boxes=grid.querySelectorAll(':scope>.control-box');"
        "for(var b=0;b<boxes.length;b++){var fields=boxes[b].querySelectorAll('input,select,textarea'),values=saved[b];"
        "if(!Array.isArray(values)){continue;}for(var i=0;i<fields.length&&i<values.length;i++){"
        "if(fields[i].type==='checkbox'){fields[i].checked=!!values[i];}else{fields[i].value=values[i];}}"
        "var selector=boxes[b].querySelector('[data-instrument-selector]');if(selector){updateInstrumentVisibility(selector);}"
        "var fs=boxes[b].querySelectorAll('[data-function-selector]');for(var j=0;j<fs.length;j++){fs[j].setAttribute('data-current-function',fs[j].value);}}"
        "updateConditionalVisibility();refreshControlPanelNumbers();observeControlPanels();return true;}"
        "document.addEventListener('input',function(e){if(workspaceReady&&e.target.closest('#controlPanelGrid')){snapshotWorkspace();}},true);"
        "document.addEventListener('change',function(e){if(workspaceReady&&e.target.closest('#controlPanelGrid')){setTimeout(snapshotWorkspace,0);}},true);"
        "var workspaceReverting=false;"
        "window.addEventListener('beforeunload',function(e){if(workspaceReverting||!workspaceReady){return;}snapshotWorkspace();"
        "if(workspaceDirty){e.preventDefault();e.returnValue='';}});"
        "function applySavedWorkspaceValues(){"
        "fetch(workspaceName?'/control-box-config?name='+encodeURIComponent(workspaceName):'/control-box-config?name=__none__',{cache:'no-store'})"
        ".then(function(r){if(r.status===404){return '';}if(!r.ok){throw new Error('Unable to load configuration');}return r.text();})"
        ".then(async function(t){var cfg=t?parseWorkspaceConfig(t):{};var boxes=document.querySelectorAll('.control-box');"
        "for(var b=0;b<boxes.length;b++){var s=cfg['box'+(b+1)];if(!s){continue;}"
        "var instrumentSelect=boxes[b].querySelector('[data-instrument-selector]');if(!instrumentSelect){continue;}"
        "var cards=boxes[b].querySelectorAll('[data-instrument-card]');"
        "for(var c=0;c<cards.length;c++){if(cards[c].getAttribute('data-instrument-card')!==instrumentSelect.value){continue;}"
        "var functionSelect=cards[c].querySelector('[data-function-selector]');var fn=functionSelect?functionSelect.value:'__basic__';"
        "var caps=cards[c].querySelectorAll('.capability[data-function]');"
        "for(var i=0;i<caps.length;i++){if(caps[i].getAttribute('data-function')!==fn){continue;}"
        "var cap=caps[i].getAttribute('data-capability');var form=caps[i].querySelector('form');"
        "if(form&&form.elements.value&&Object.prototype.hasOwnProperty.call(s,'value_'+cap)){form.elements.value.value=s['value_'+cap];}}"
        "break;}}updateConditionalVisibility();if(typeof macroInitialRestore!=='undefined')await macroInitialRestore;"
        "if(!workspaceName&&typeof macroGrid!=='undefined'){macroGrid.replaceChildren();macroUpdateRemoveButtons();macroScheduleLayout();}"
        "else if(cfg.workspace&&cfg.workspace.macro_panel_count&&typeof macroApplySavedWorkspace==='function'&&typeof macroRestoredFromSession!=='undefined'&&!macroRestoredFromSession){await macroApplySavedWorkspace(cfg);}"
        "workspaceSavedState=captureWorkspace();workspaceReady=true;"
        "if(workspaceName)restoreReloadWorkspace();snapshotWorkspace();})"
        ".catch(function(e){workspaceSavedState=captureWorkspace();workspaceReady=true;if(workspaceName)restoreReloadWorkspace();snapshotWorkspace();window.alert('Workspace values could not be loaded: '+e.message);});"
        "}"
        "function readControlBox(button){"
        "if(capabilityBusy){return false;}"
        "var box=button.closest('.control-box');if(!box){return false;}"
        "var instrumentSelect=box.querySelector('[data-instrument-selector]');if(!instrumentSelect){return false;}"
        "var instrument=instrumentSelect.value;var cards=box.querySelectorAll('[data-instrument-card]');var card=null;"
        "for(var c=0;c<cards.length;c++){if(cards[c].getAttribute('data-instrument-card')===instrument){card=cards[c];break;}}"
        "if(!card){window.alert('READ failed: selected instrument card was not found.');return false;}"
        "var functionSelect=card.querySelector('[data-function-selector]');var fn=functionSelect?functionSelect.value:'__basic__';"
        "var status=box.querySelector('.control-box-status');if(status){status.textContent='READ in progress...';}"
        "var completed={};var dependencies={};var readCount=0;capabilityBusy=true;"
        "var buttons=document.querySelectorAll('button');for(var b=0;b<buttons.length;b++){buttons[b].disabled=true;}"
        "var caps=card.querySelectorAll('.capability[data-function]');"
        "for(var i=0;i<caps.length;i++){var cap=caps[i];if(cap.getAttribute('data-function')!==fn){continue;}"
        "var rule=cap.getAttribute('data-visible-when')||'';var colon=rule.indexOf(':');"
        "if(colon>0){dependencies[rule.substring(0,colon)]=true;}}"
        "function readableItem(cap){"
        "var section=cap.getAttribute('data-capability');if(!section||completed[section]){return null;}"
        "var form=cap.querySelector('form');if(!form||!form.elements.value){return null;}"
        "var bs=cap.querySelectorAll('button');for(var j=0;j<bs.length;j++){var oc=bs[j].getAttribute('onclick')||'';"
        "if(oc.indexOf(\"asyncCapability(this,'read')\")>=0){return {section:section,form:form};}}return null;}"
        "function nextDependency(){"
        "var all=card.querySelectorAll('.capability[data-function]');"
        "for(var i=0;i<all.length;i++){var cap=all[i];if(cap.getAttribute('data-function')!==fn){continue;}"
        "var section=cap.getAttribute('data-capability');if(!dependencies[section]){continue;}"
        "var item=readableItem(cap);if(item){return item;}completed[section]=true;}return null;}"
        "function nextVisible(){"
        "var all=card.querySelectorAll('.capability[data-function]');"
        "for(var i=0;i<all.length;i++){var cap=all[i];if(cap.getAttribute('data-function')!==fn||cap.style.display==='none'){continue;}"
        "var item=readableItem(cap);if(item){return item;}var section=cap.getAttribute('data-capability');if(section){completed[section]=true;}}return null;}"
        "function finish(){capabilityBusy=false;for(var b=0;b<buttons.length;b++){buttons[b].disabled=false;}}"
        "function doQuery(item,next){completed[item.section]=true;"
        "var body='instrument='+encodeURIComponent(instrument)+'&capability='+encodeURIComponent(item.section)+'&action=read';"
        "fetch('/capability-async',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})"
        ".then(function(r){return r.text().then(function(t){if(!r.ok){throw new Error(t);}return t;});})"
        ".then(function(t){item.form.elements.value.value=t;readCount++;updateConditionalVisibility();next();})"
        ".catch(function(e){if(status){status.textContent='READ failed at '+item.section+'.';}window.alert('READ stopped at '+item.section+' after '+readCount+' successful parameter READ(s): '+e.message);finish();});}"
        "function readVisible(){updateConditionalVisibility();var item=nextVisible();"
        "if(!item){if(status){status.textContent='READ: '+readCount+' parameter(s) read successfully.';}finish();return;}"
        "doQuery(item,readVisible);}"
        "function readDependencies(){var item=nextDependency();if(!item){updateConditionalVisibility();readVisible();return;}"
        "doQuery(item,readDependencies);}"
        "readDependencies();return false;}"
        "function sendControlBox(button){"
        "if(capabilityBusy){return false;}"
        "var box=button.closest('.control-box');if(!box){return false;}"
        "var status=box.querySelector('.control-box-status');if(status){status.textContent='SEND in progress...';}"
        "var instrumentSelect=box.querySelector('[data-instrument-selector]');if(!instrumentSelect){return false;}"
        "var instrument=instrumentSelect.value;var cards=box.querySelectorAll('[data-instrument-card]');var card=null;"
        "for(var c=0;c<cards.length;c++){if(cards[c].getAttribute('data-instrument-card')===instrument){card=cards[c];break;}}"
        "if(!card){window.alert('SEND failed: selected instrument card was not found.');return false;}"
        "var functionSelect=card.querySelector('[data-function-selector]');var fn=functionSelect?functionSelect.value:'__basic__';"
        "var parts=['instrument='+encodeURIComponent(instrument),'function='+encodeURIComponent(fn)];"
        "var caps=card.querySelectorAll('.capability[data-function]');"
        "for(var i=0;i<caps.length;i++){if(caps[i].getAttribute('data-function')!==fn){continue;}"
        "if(caps[i].style.display==='none'){continue;}"
        "var form=caps[i].querySelector('form');if(!form||!form.elements.value){continue;}"
        "parts.push('value_'+encodeURIComponent(caps[i].getAttribute('data-capability'))+'='+encodeURIComponent(form.elements.value.value));}"
        "capabilityBusy=true;var buttons=document.querySelectorAll('button');"
        "for(var i=0;i<buttons.length;i++){buttons[i].disabled=true;}"
        "fetch('/send-control-box',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:parts.join('&')})"
        ".then(function(r){return r.text().then(function(t){if(!r.ok){throw new Error(t);}return t;});})"
        ".then(function(t){if(status){status.textContent='SEND: '+t;}})"
        ".catch(function(e){if(status){status.textContent='SEND failed.';}window.alert('SEND failed: '+e.message);})"
        ".finally(function(){capabilityBusy=false;for(var i=0;i<buttons.length;i++){buttons[i].disabled=false;}});"
        "return false;}"
        // browser-only panel creation and removal.
        "var pendingControlPanelRemoval=null;var previousPanelFocus=null;"
        // dense CSS-grid masonry, with row spans recalculated
        // when instrument/function controls or status messages change height.
        "var panelLayoutObserver=null;var panelLayoutPending=false;"
        "function layoutControlPanels(){"
        "panelLayoutPending=false;var grid=document.getElementById('controlPanelGrid');if(!grid){return;}"
        "var boxes=grid.querySelectorAll(':scope>.control-box');"
        "if(getComputedStyle(grid).gridTemplateColumns.split(' ').length===1){for(var i=0;i<boxes.length;i++){boxes[i].style.gridRowEnd='auto';}document.getElementById('tab-instruments').style.visibility='visible';return;}"
        "var css=getComputedStyle(grid),row=parseFloat(css.gridAutoRows)||8,gap=parseFloat(css.rowGap)||18;"
        "for(var i=0;i<boxes.length;i++){var box=boxes[i];"
        "box.style.gridRowEnd='span '+Math.max(1,Math.ceil((box.getBoundingClientRect().height+gap)/(row+gap)));}"
        "document.getElementById('tab-instruments').style.visibility='visible';"
        "}"
        "function scheduleControlPanelLayout(){if(panelLayoutPending){return;}panelLayoutPending=true;requestAnimationFrame(layoutControlPanels);}"
        "function observeControlPanels(){"
        "if(!panelLayoutObserver&&typeof ResizeObserver!=='undefined'){"
        "panelLayoutObserver=new ResizeObserver(function(){scheduleControlPanelLayout();});}"
        "var boxes=document.querySelectorAll('#controlPanelGrid>.control-box');"
        "if(panelLayoutObserver){panelLayoutObserver.disconnect();for(var i=0;i<boxes.length;i++){panelLayoutObserver.observe(boxes[i]);}}"
        "scheduleControlPanelLayout();}"
        "window.addEventListener('resize',scheduleControlPanelLayout);"
        "function refreshControlPanelNumbers(){"
        "var boxes=document.querySelectorAll('#controlPanelGrid>.control-box');"
        "for(var i=0;i<boxes.length;i++){var h=boxes[i].querySelector('.control-panel-heading h2');if(h){h.textContent='Control Panel '+(i+1);}}"
        "var count=document.getElementById('controlPanelCount');if(count){count.textContent=boxes.length+' Control Panel'+(boxes.length===1?'':'s');}"
        "var add=document.getElementById('addControlPanelButton');if(add){add.disabled=boxes.length>=8;add.title=boxes.length>=8?'Maximum of eight Control Panels':'';}"
        "}"
        "function addControlPanel(){"
        "if(capabilityBusy){return false;}"
        "var grid=document.getElementById('controlPanelGrid'),template=document.getElementById('controlBoxTemplate');"
        "if(!grid||!template||grid.children.length>=8){return false;}"
        "var box=document.createElement('div');box.className='panel control-box';"
        "box.innerHTML='<div class=\"control-panel-heading\"><h2>Control Panel</h2><button type=\"button\" class=\"remove-control-panel\" onclick=\"return requestRemoveControlPanel(this);\" aria-label=\"Remove this Control Panel\">&#128465; Remove</button></div><div class=\"control-box-content\"></div><div class=\"buttons\"><button type=\"button\" onclick=\"return readControlBox(this);\">READ</button><button type=\"button\" onclick=\"return sendControlBox(this);\">SEND</button></div><div class=\"control-box-status\" aria-live=\"polite\"></div>';"
        "box.querySelector('.control-box-content').innerHTML=template.innerHTML;grid.appendChild(box);"
        "var selector=box.querySelector('[data-instrument-selector]');if(selector){"
        "var placeholder=document.createElement('option');placeholder.value='';placeholder.textContent='Select an instrument';"
        "selector.insertBefore(placeholder,selector.firstChild);selector.value='';updateInstrumentVisibility(selector);}"
        "var fs=box.querySelectorAll('[data-function-selector]');for(var i=0;i<fs.length;i++){fs[i].setAttribute('data-current-function',fs[i].value);}"
        "refreshControlPanelNumbers();observeControlPanels();snapshotWorkspace();return false;}"
        "function requestRemoveControlPanel(button){"
        "if(capabilityBusy){return false;}"
        "var box=button.closest('.control-box');if(!box){return false;}"
        "pendingControlPanelRemoval=box;previousPanelFocus=button;"
        "var boxes=Array.prototype.slice.call(document.querySelectorAll('#controlPanelGrid>.control-box'));"
        "var n=boxes.indexOf(box)+1;document.getElementById('removeControlPanelTitle').textContent='Remove Control Panel '+n+'?';"
        "var modal=document.getElementById('removeControlPanelModal');modal.hidden=false;"
        "document.getElementById('cancelRemoveControlPanel').focus();return false;}"
        "function cancelRemoveControlPanel(){"
        "document.getElementById('removeControlPanelModal').hidden=true;pendingControlPanelRemoval=null;"
        "if(previousPanelFocus&&previousPanelFocus.isConnected){previousPanelFocus.focus();}previousPanelFocus=null;return false;}"
        "function confirmRemoveControlPanel(){"
        "if(pendingControlPanelRemoval){pendingControlPanelRemoval.remove();}"
        "pendingControlPanelRemoval=null;previousPanelFocus=null;"
        "document.getElementById('removeControlPanelModal').hidden=true;refreshControlPanelNumbers();"
        "observeControlPanels();snapshotWorkspace();document.getElementById('addControlPanelButton').focus();return false;}"
        "document.addEventListener('keydown',function(e){"
        "var modal=document.getElementById('removeControlPanelModal');if(!modal||modal.hidden){return;}"
        "if(e.key==='Escape'){e.preventDefault();cancelRemoveControlPanel();}"
        "if(e.key==='Tab'){var buttons=modal.querySelectorAll('button');var first=buttons[0],last=buttons[buttons.length-1];"
        "if(e.shiftKey&&document.activeElement===first){e.preventDefault();last.focus();}"
        "else if(!e.shiftKey&&document.activeElement===last){e.preventDefault();first.focus();}}"
        "});"
        "function buildWorkspaceConfiguration(){"
        "var boxes=document.querySelectorAll('#controlPanelGrid>.control-box');"
        "var lines=['[workspace]','panel_count='+boxes.length,''];"
        "for(var b=0;b<boxes.length;b++){var box=boxes[b];var instrumentSelect=box.querySelector('[data-instrument-selector]');"
        "if(!instrumentSelect){continue;}var instrument=instrumentSelect.value;lines.push('[box'+(b+1)+']');"
        "lines.push('instrument='+iniSafe(instrument));"
        "var cards=box.querySelectorAll('[data-instrument-card]');"
        "for(var c=0;c<cards.length;c++){if(cards[c].getAttribute('data-instrument-card')!==instrument){continue;}"
        "var functionSelect=cards[c].querySelector('[data-function-selector]');var fn=functionSelect?functionSelect.value:'__basic__';"
        "lines.push('function='+iniSafe(fn));"
        "var caps=cards[c].querySelectorAll('.capability[data-function]');"
        "for(var i=0;i<caps.length;i++){if(caps[i].getAttribute('data-function')!==fn){continue;}"
        "if(caps[i].style.display==='none'){continue;}"
        "var form=caps[i].querySelector('form');if(!form||!form.elements.value){continue;}"
        "lines.push('value_'+iniSafe(caps[i].getAttribute('data-capability'))+'='+iniSafe(form.elements.value.value));}"
        "break;}lines.push('');}"
        "var macros=document.querySelectorAll('#macroPanelGrid>.macro-panel');"
        "lines.splice(2,0,'macro_panel_count='+macros.length);"
        "for(var m=0;m<macros.length;m++){var w=macros[m],sel=w.querySelector('select');"
        "lines.push('[macro'+(m+1)+']');lines.push('file='+iniSafe(sel?sel.value:''));"
        "var form=w.querySelector('.macro-card form');if(form){Array.from(form.elements).forEach(function(f){"
        "if(f.name&&f.name!=='macro_file'){lines.push('param_'+iniSafe(f.name)+'='+iniSafe(f.value));}});}"
        "lines.push('');}"
        "return lines.join('\\n');}"
        // discard the browser snapshot and reload the authoritative SD configuration.
        "function workspaceUpdateMacroBusyButtons(){var busy=(typeof macroActiveCard!=='undefined'&&!!macroActiveCard);"
        "['workspaceSaveButton','workspaceSaveAsButton','workspaceNewButton','workspaceRevertButton','workspaceRenameButton','workspaceDeleteButton','workspaceStartupCheckbox'].forEach(function(id){var b=document.getElementById(id);"
        "if(b){b.disabled=busy;b.style.opacity=busy?'0.45':'';b.style.cursor=busy?'not-allowed':'';}});}"
        "function workspaceMacroBusy(){return (typeof macroActiveCard!=='undefined'&&!!macroActiveCard)||(typeof macroLoading!=='undefined'&&!!macroLoading);}"
        "var startupWorkspaceName='';"
        "function syncStartupCheckbox(){var cb=document.getElementById('workspaceStartupCheckbox');var sel=document.getElementById('workspaceNameSelector');"
        "if(!cb||!sel)return;cb.checked=!!sel.value&&sel.value===startupWorkspaceName;cb.disabled=!sel.value||workspaceMacroBusy();"
        "Array.from(sel.options).forEach(function(o){if(o.value)o.textContent=o.value+(o.value===startupWorkspaceName?' *':'');});}"
        "function refreshStartupWorkspace(){return fetch('/startup-configuration',{cache:'no-store'})"
        ".then(r=>r.text().then(t=>{if(!r.ok)throw Error(t);return t.trim();}))"
        ".then(n=>{startupWorkspaceName=n;syncStartupCheckbox();}).catch(e=>{document.getElementById('workspaceSelectionStatus').textContent=e.message;});}"
        "function changeStartupWorkspace(cb){if(workspaceMacroBusy()){syncStartupCheckbox();return false;}"
        "var sel=document.getElementById('workspaceNameSelector');var name=cb.checked&&sel?sel.value:'';"
        "if(cb.checked&&!name){cb.checked=false;alert('Select a saved configuration first.');return false;}"
        "cb.disabled=true;fetch('/startup-configuration',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'name='+encodeURIComponent(name)})"
        ".then(r=>r.text().then(t=>{if(!r.ok)throw Error(t);return t;}))"
        ".then(t=>{startupWorkspaceName=name;syncStartupCheckbox();document.getElementById('workspaceSelectionStatus').textContent=t;})"
        ".catch(e=>{syncStartupCheckbox();alert('Startup selection failed: '+e.message);});return false;}"
        "document.addEventListener('change',function(e){if(e.target.id==='workspaceNameSelector')syncStartupCheckbox();});"
        "function refreshWorkspaceNames(){var sel=document.getElementById('workspaceNameSelector');if(!sel)return;"
        "fetch('/saved-configurations',{cache:'no-store'}).then(r=>{if(!r.ok)throw Error('Unable to list configurations');return r.text();})"
        ".then(t=>{sel.replaceChildren();sel.add(new Option('Select a configuration',''));t.split(/\\r?\\n/).filter(Boolean).forEach(n=>sel.add(new Option(n,n)));"
        "if(workspaceName&&![...sel.options].some(o=>o.value===workspaceName)){sel.add(new Option(workspaceName,workspaceName));}"
        "sel.value=workspaceName;syncStartupCheckbox();}).catch(e=>{document.getElementById('workspaceSelectionStatus').textContent=e.message;});}"
        "function renameWorkspaceConfiguration(){"
        "if(workspaceMacroBusy())return false;"
        "var sel=document.getElementById('workspaceNameSelector');var oldName=sel?sel.value:'';"
        "if(!oldName){alert('Select a saved configuration to rename.');return false;}"
        "var newName=prompt('Rename configuration '+oldName+' to:',oldName);"
        "if(newName===null)return false;newName=newName.trim();"
        "if(!/^[A-Za-z0-9_-]{1,40}$/.test(newName)||newName==='default'){alert('Use 1-40 letters, numbers, underscores or hyphens (not default).');return false;}"
        "if(newName===oldName)return false;"
        "if(Array.from(sel.options).some(o=>o.value===newName)){alert('That configuration name already exists.');return false;}"
        "if(!confirm('Rename saved configuration '+oldName+' to '+newName+'?'))return false;"
        "fetch('/rename-saved-configuration',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'old='+encodeURIComponent(oldName)+'&new='+encodeURIComponent(newName)})"
        ".then(r=>r.text().then(t=>{if(!r.ok)throw Error(t);return t;}))"
        ".then(t=>{if(workspaceName===oldName){workspaceName=newName;var u=new URL(location.href);u.searchParams.set('name',newName);history.replaceState(null,'',u.pathname+u.search+u.hash);}"
        "refreshWorkspaceNames();refreshStartupWorkspace();document.getElementById('workspaceSelectionStatus').textContent=t;})"
        ".catch(e=>alert('Rename failed: '+e.message));return false;}"
        "function deleteWorkspaceConfiguration(){"
        "if(workspaceMacroBusy())return false;"
        "var sel=document.getElementById('workspaceNameSelector');var name=sel?sel.value:'';"
        "if(!name){alert('Select a saved configuration to delete.');return false;}"
        "if(!confirm('Permanently delete saved configuration '+name+' from the SD card?\\nThis cannot be undone.'))return false;"
        "fetch('/delete-saved-configuration',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'name='+encodeURIComponent(name)})"
        ".then(r=>r.text().then(t=>{if(!r.ok)throw Error(t);return t;}))"
        ".then(t=>{if(workspaceName===name){workspaceName='';workspaceSavedState='';"
        "var u=new URL(location.href);u.searchParams.delete('name');history.replaceState(null,'',u.pathname+u.search+u.hash);"
        "snapshotWorkspace();}"
        "refreshWorkspaceNames();refreshStartupWorkspace();document.getElementById('workspaceSelectionStatus').textContent=t;})"
        ".catch(e=>alert('Delete failed: '+e.message));return false;}"
        // Browser-only workspace reset: no HTTP request, no instrument I/O, no SD writes.
        "function newWorkspace(){"
        "if(workspaceMacroBusy()||capabilityBusy){alert('Finish or abort the active operation before clearing the workspace.');return false;}"
        "if(workspaceDirty&&!window.confirm('Discard unsaved changes and start a new empty workspace?'))return false;"
        "var grid=document.getElementById('controlPanelGrid');if(grid)grid.replaceChildren();"
        "if(typeof macroGrid!=='undefined'){macroGrid.replaceChildren();macroUpdateRemoveButtons();macroScheduleLayout();macroSavePanels();}"
        "refreshControlPanelNumbers();observeControlPanels();"
        "workspaceName='';var sel=document.getElementById('workspaceNameSelector');if(sel)sel.value='';syncStartupCheckbox();"
        "var u=new URL(location.href);u.searchParams.delete('name');u.searchParams.delete('load');history.replaceState(null,'',u.pathname+u.search+u.hash);"
        "try{sessionStorage.removeItem(workspaceSessionKey);}catch(e){}"
        "workspaceSavedState=captureWorkspace();snapshotWorkspace();"
        "document.getElementById('workspaceSelectionStatus').textContent='New empty workspace (not saved).';"
        "return false;}"
        "function loadNamedWorkspace(){if(workspaceMacroBusy())return false;"
        "var sel=document.getElementById('workspaceNameSelector');if(!sel)return false;var target=sel.value;"
        "if(!target)return false;if(workspaceDirty&&!confirm('Discard unsaved changes and load '+target+'?'))return false;"
        "document.getElementById('workspaceSelectionStatus').textContent='Loading '+target+'...';"
        "workspaceReverting=true;try{sessionStorage.removeItem(workspaceSessionKey);sessionStorage.removeItem(macroStateKey);}catch(e){}"
        "location.assign('/?name='+encodeURIComponent(target)+'&load='+Date.now());return false;}"
        "refreshWorkspaceNames();refreshStartupWorkspace();"
        "function revertWorkspaceConfiguration(){"
        "if(workspaceMacroBusy()){return false;}"
        "if(workspaceDirty&&!window.confirm('Discard unsaved workspace changes and reload the last saved SD-card configuration?')){return false;}"
        "workspaceReverting=true;try{sessionStorage.removeItem(workspaceSessionKey);sessionStorage.removeItem(macroStateKey);}catch(e){}"
        "window.location.reload();return false;}"
        "function saveWorkspaceAs(){"
        "if(workspaceMacroBusy())return false;"
        "var name=prompt('Save workspace as (1-40 letters, numbers, underscore or hyphen):',workspaceName||'');"
        "if(name===null)return false;name=name.trim();"
        "if(!/^[A-Za-z0-9_-]{1,40}$/.test(name)){alert('Use 1-40 letters, numbers, underscores or hyphens.');return false;}"
        "var sel=document.getElementById('workspaceNameSelector');"
        "var exists=!!sel&&Array.from(sel.options).some(o=>o.value===name);"
        "if(exists&&!confirm('Overwrite existing configuration '+name+'?'))return false;"
        "var config=buildWorkspaceConfiguration();"
        "fetch('/save-control-box-config?name='+encodeURIComponent(name)+'&mode='+(exists?'replace':'create'),"
        "{method:'POST',headers:{'Content-Type':'text/plain'},body:config})"
        ".then(r=>r.text().then(t=>{if(!r.ok)throw Error(t);return t;}))"
        ".then(t=>{workspaceName=name;workspaceSavedState=config;snapshotWorkspace();refreshWorkspaceNames();alert(t);})"
        ".catch(e=>alert('Save As failed: '+e.message));return false;}"
        "function saveWorkspaceConfiguration(){"
        "if(workspaceMacroBusy()){return false;}"
        "if(!workspaceName){window.alert('Use Save As to name this workspace before saving.');return false;}"
        "if(!window.confirm('Overwrite existing configuration '+workspaceName+'?\\n\\nThe previously saved version will be replaced.'))return false;"
        "var config=buildWorkspaceConfiguration();"
        "fetch('/save-control-box-config?name='+encodeURIComponent(workspaceName)+'&mode=replace',{method:'POST',headers:{'Content-Type':'text/plain'},body:config})"
        ".then(function(r){return r.text().then(function(t){if(!r.ok){throw new Error(t);}return t;});})"
        ".then(function(t){workspaceSavedState=config;snapshotWorkspace();window.alert(t);})"
        ".catch(function(e){window.alert('Save Configuration failed: '+e.message);});"
        "return false;}"
        // preserve page state while refreshing the SD listing.
        // The lightweight /sd-list endpoint renders only the SD-card panel.
        "function refreshSDFilePanel(){"
        "return Promise.all([fetch('/sd-list?ts='+Date.now(),{cache:'no-store'}),"
        "fetch('/sd-folders?ts='+Date.now(),{cache:'no-store'})])"
        ".then(function(res){if(!res[0].ok||!res[1].ok){throw new Error('SD refresh failed');}"
        "return Promise.all([res[0].text(),res[1].text()]);})"
        ".then(function(parts){"
        "var doc=new DOMParser().parseFromString(parts[0],'text/html');"
        "var fresh=doc.getElementById('sdFilePanel');"
        "var current=document.getElementById('sdFilePanel');"
        "if(!fresh||!current){throw new Error('SD listing missing from server response');}"
        "current.innerHTML=fresh.innerHTML;"
        "var folders=parts[1].split('\\n').filter(function(p){return p.length>0;});"
        "['sdDirectoryParent','sdFolder'].forEach(function(id){"
        "var sel=document.getElementById(id);if(!sel){return;}"
        "var selected=sel.value;sel.replaceChildren();"
        "folders.forEach(function(path){sel.add(new Option(path==='/'?'/ (root)':path,path));});"
        "sel.value=folders.indexOf(selected)>=0?selected:'/';"
        "});"
        "});"
        "}"
        "function confirmSDDelete(form){"
        "var input=form.querySelector('[name=path]');"
        "var path=input?input.value:'';"
        "if(!window.confirm('Are you sure you want to delete:\\n'+path+'?\\n\\nThis cannot be undone.')){return false;}"
        "var button=form.querySelector('button[type=submit]');if(button){button.disabled=true;}"
        "fetch('/sd-delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'path='+encodeURIComponent(path)+'&ajax=1'})"
        ".then(function(r){return r.text().then(function(t){if(!r.ok){throw new Error(t||('HTTP '+r.status));}});})"
        ".then(function(){return refreshSDFilePanel();})"
        ".catch(function(e){window.alert('SD delete/refresh failed: '+e.message+'\\nUse a manual refresh to reconcile the listing.');if(button){button.disabled=false;}});"
        "return false;"
        "}"
        // explicitly confirm hidden-file removal.
        "function confirmSDHiddenDelete(form){"
        "var input=form.querySelector('[name=path]');var path=input?input.value:'';"
        "if(!window.confirm('Permanently delete ALL hidden files in:\\n'+path+'?\\n\\nThis cannot be undone.'))return false;"
        "var button=form.querySelector('button[type=submit]');if(button)button.disabled=true;"
        "fetch('/sd-remove-hidden',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'path='+encodeURIComponent(path)})"
        ".then(function(r){return r.text().then(function(t){if(!r.ok)throw new Error(t||('HTTP '+r.status));});})"
        ".then(function(){return refreshSDFilePanel();})"
        ".catch(function(e){window.alert('Hidden-file removal failed: '+e.message);refreshSDFilePanel();});"
        "return false;}"
        // remove empty directory asynchronously and refresh SD panel.
        "function confirmSDDirectoryDelete(form){"
        "var input=form.querySelector('[name=path]');var path=input?input.value:'';"
        "if(!window.confirm('Remove empty directory:\\n'+path+'?')){return false;}"
        "var button=form.querySelector('button[type=submit]');if(button){button.disabled=true;}"
        "fetch('/sd-rmdir',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'path='+encodeURIComponent(path)})"
        ".then(function(r){return r.text().then(function(t){if(!r.ok){throw new Error(t||('HTTP '+r.status));}});})"
        ".then(function(){return refreshSDFilePanel();})"
        ".catch(function(e){window.alert('Remove directory failed: '+e.message);if(button){button.disabled=false;}});"
        "return false;}"
        // create folders and upload ordinary files without navigation.
        "function createSDDirectory(form){"
        "var button=form.querySelector('button[type=submit]');if(button){button.disabled=true;}"
        "var data=new URLSearchParams(new FormData(form));data.set('ajax','1');"
        "fetch('/sd-mkdir',{method:'POST',body:data})"
        ".then(function(r){return r.text().then(function(t){if(!r.ok){throw new Error(t||('HTTP '+r.status));}return t;});})"
        ".then(function(path){"
        "['sdDirectoryParent','sdFolder'].forEach(function(id){var sel=document.getElementById(id);"
        "if(sel&&!Array.from(sel.options).some(function(o){return o.value===path;})){var opt=new Option(path,path);sel.add(opt);}});"
        "form.querySelector('[name=dirname]').value='';return refreshSDFilePanel();})"
        ".catch(function(e){window.alert('Create directory failed: '+e.message);})"
        ".finally(function(){if(button){button.disabled=false;}});return false;}"
        "function uploadSDFile(form){"
        "var button=form.querySelector('button[type=submit]');if(button){button.disabled=true;}"
        "var data=new FormData(form);data.set('ajax','1');"
        "var folder=String(data.get('folder')||'/');var name=String(data.get('filename')||'');"
        "fetch('/sd-upload',{method:'POST',body:data,redirect:'follow'})"
        ".then(function(r){return r.text().then(function(t){if(!r.ok){var d=new DOMParser().parseFromString(t,'text/html');"
        "throw new Error((d.querySelector('main p')||{}).textContent||t||('HTTP '+r.status));}return t;});})"
        ".then(function(){try{localStorage.setItem('ak3yLastUploadFolder',folder);}catch(e){}"
        "form.reset();var sel=document.getElementById('sdFolder');if(sel){sel.value=folder;}"
        "return refreshSDFilePanel();})"
        ".then(function(){if(/\\.cfg$/i.test(name)||folder==='/macros'||folder==='/instruments'){"
        "window.alert('The uploaded file may change instrument or macro configuration. Reload the page to apply it.');}})"
        ".catch(function(e){window.alert('Upload/refresh failed: '+e.message);})"
        ".finally(function(){if(button){button.disabled=false;}});return false;}"
        "function confirmSDOverwrite(form){"
        "var source=form.querySelector('input[name=upload]');"
        "if(source&&source.files&&source.files.length){"
        "var bytes=source.files[0].size;"
        "if(bytes>262144){"
        "form.removeAttribute('data-approved');"
        "window.alert('Upload cancelled: file is '+bytes.toLocaleString()+' bytes. Maximum is 256 KB (262,144 bytes).');"
        "return false;}}"
        "if(form.getAttribute('data-approved')==='1'){form.removeAttribute('data-approved');uploadSDFile(form);return false;}"
        "var folder=form.querySelector('[name=folder]').value;"
        "var filename=form.querySelector('[name=filename]').value.trim();"
        "if(!filename){return true;}"
        "var body='folder='+encodeURIComponent(folder)+'&filename='+encodeURIComponent(filename);"
        "fetch('/sd-exists',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})"
        ".then(function(r){if(!r.ok){throw new Error('Unable to check SD card');}return r.text();})"
        ".then(function(t){"
        "if(t.trim()==='1'){"
        "var path=(folder==='/'?'':folder)+'/'+filename;"
        "if(!window.confirm('File already exists:\\n'+path+'\\n\\nReplace the existing file?')){return;}"
        "}"
        "var replace=form.querySelector('input[name=replace]');"
        "if(!replace){replace=document.createElement('input');replace.type='hidden';replace.name='replace';form.appendChild(replace);}"
        "replace.value=(t.trim()==='1')?'1':'0';"
        "form.setAttribute('data-approved','1');"
        "form.requestSubmit();"
        "})"
        ".catch(function(e){window.alert('Could not check whether the SD-card file already exists. Upload cancelled.');});"
        "return false;"
        "}"
        "window.addEventListener('load',function(){"
        "var uploadFolder=document.getElementById('sdFolder');"
        "if(uploadFolder){try{var lastFolder=localStorage.getItem('ak3yLastUploadFolder')||'/';"
        "uploadFolder.value=Array.from(uploadFolder.options).some(function(o){return o.value===lastFolder;})?lastFolder:'/';"
        "}catch(e){uploadFolder.value='/';}}"
        "var template=document.getElementById('controlBoxTemplate');"
        "var targets=document.querySelectorAll('.control-box-content');"
        "if(template){for(var k=0;k<targets.length;k++){targets[k].innerHTML=template.innerHTML;}}"
        "var boxes=document.querySelectorAll('.control-box');"
        "for(var b=0;b<boxes.length;b++){"
        "var savedInstrument=boxes[b].getAttribute('data-saved-instrument')||'';"
        "var instrumentSelect=boxes[b].querySelector('[data-instrument-selector]');"
        "if(instrumentSelect&&savedInstrument){"
        "for(var oi=0;oi<instrumentSelect.options.length;oi++){"
        "if(instrumentSelect.options[oi].value===savedInstrument){instrumentSelect.value=savedInstrument;break;}}"
        "}"
        "if(instrumentSelect){updateInstrumentVisibility(instrumentSelect);}"
        "var savedFunction=boxes[b].getAttribute('data-saved-function')||'';"
        "if(savedFunction&&instrumentSelect){"
        "var cards=boxes[b].querySelectorAll('[data-instrument-card]');"
        "for(var c=0;c<cards.length;c++){"
        "if(cards[c].getAttribute('data-instrument-card')===instrumentSelect.value){"
        "var functionSelect=cards[c].querySelector('[data-function-selector]');"
        "if(functionSelect){for(var of=0;of<functionSelect.options.length;of++){"
        "if(functionSelect.options[of].value===savedFunction){functionSelect.value=savedFunction;break;}}}"
        "break;}}"
        "}"
        "}"
        "var fs=document.querySelectorAll('.control-box [data-function-selector]');"
        "for(var i=0;i<fs.length;i++){fs[i].setAttribute('data-current-function',fs[i].value);}"
        "updateConditionalVisibility();"
        "applySavedWorkspaceValues();"
        "refreshControlPanelNumbers();"
        "observeControlPanels();"
        "});"
        "var commandBusy=false;"
        "function asyncCommand(form){"
        "if(commandBusy){return false;}"
        "if(!form.reportValidity()){return false;}"
        "commandBusy=true;"
        "var buttons=document.querySelectorAll('button');"
        "for(var i=0;i<buttons.length;i++){buttons[i].disabled=true;}"
        "var result=document.getElementById('commandResult');"
        "result.style.display='block';"
        "result.textContent='Executing...';"
        "var body='command='+encodeURIComponent(form.elements.command.value);"
        "fetch('/command-async',{method:'POST',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})"
        ".then(function(r){return r.text().then(function(t){"
        "if(!r.ok){throw new Error(t);}return t;});})"
        ".then(function(t){"
        "var p=t.indexOf('\\n');"
        "if(p<0){result.textContent='Result: '+t;}"
        "else{result.textContent='Result: '+t.substring(0,p)+'\\n\\nResponse:\\n'+t.substring(p+1);}"
        "})"
        ".catch(function(e){result.textContent='Result: '+e.message;})"
        ".finally(function(){"
        "commandBusy=false;"
        "for(var i=0;i<buttons.length;i++){buttons[i].disabled=false;}"
        "});"
        "return false;"
        "}"
        "</script>"
        "</div></body></html>");

    //
    // Empty chunk terminates HTTP chunked transfer.
    //

    flushHtmlBuffer();
    m_htmlBufferActive = false;
    guiTiming("Remaining HTML");
    m_server.sendContent("");
    guiTiming("HTTP final chunk");
}

void WebServerManager::renderInstruments(
    const char* cardInstrument,
    const char* cardCapability,
    const char* cardValue,
    const char* cardResult)
{
    //
    // Control Box 1 owns the Instrument selection.
    // The installed-instrument list remains defined by instruments.cfg.
    //

    File file = SD.open(
        "/config/instruments.cfg",
        FILE_READ);

    if (!file)
    {
        sendChunk(
            "<div class=\"result\">"
            "No instruments configured. To add instruments, upload "
            "/config/instruments.cfg and the corresponding .cfg profile files "
            "to /instruments using SD Card File Upload. "
            "Restart the controller after uploading instruments.cfg."
            "</div>");
        return;
    }

    const uint32_t renderingStartMs = millis();
    uint32_t selectorLoadMs = 0;
    uint32_t cardLoadMs = 0;
    uint32_t cardRenderMs = 0;
    unsigned int selectorCount = 0;
    unsigned int cardCount = 0;

    // Temporary heap-backed cache for this page only. If allocation fails,
    // fall back to the original second-pass loader without limiting instruments.
    InstrumentDefinition* cached = nullptr;
    size_t cachedCount = 0;
    size_t cachedCapacity = 0;
    bool cacheComplete = true;

    bool foundAny = false;
    unsigned int configuredSections = 0;
    unsigned int missingProfiles = 0;
    char firstInstrument[32] = "";
    char line[160];

    sendChunk(
        "<div class=\"function-selector\">"
        "<label>Instrument</label>"
        "<select data-instrument-selector "
        "onchange=\"updateInstrumentVisibility(this);\">");

    while (readLine(file, line, sizeof(line)))
    {
        trim(line);

        if (line[0] == '\0' ||
            line[0] == '#' ||
            line[0] == ';')
            continue;

        const size_t length = strlen(line);

        if (length < 3 ||
            line[0] != '[' ||
            line[length - 1] != ']')
            continue;

        line[length - 1] = '\0';
        char* section = line + 1;
        trim(section);
        ++configuredSections;

        // Distinguish missing profile files from other invalid definitions.
        char referencedProfile[80];
        if (m_configurationFile.getValue(
                "/config/instruments.cfg", section, "profile",
                referencedProfile, sizeof(referencedProfile)))
        {
            char referencedPath[160];
            const int n = snprintf(referencedPath, sizeof(referencedPath),
                                   "/instruments/%s", referencedProfile);
            if (n > 0 && static_cast<size_t>(n) < sizeof(referencedPath) &&
                !SD.exists(referencedPath))
                ++missingProfiles;
        }

        InstrumentDefinition instrument;

        const uint32_t loadStartMs = millis();
        const bool loaded = loadInstrument(section, instrument);
        selectorLoadMs += millis() - loadStartMs;
        if (!loaded)
            continue;
        ++selectorCount;

        if (cacheComplete)
        {
            if (cachedCount == cachedCapacity)
            {
                const size_t nextCapacity = cachedCapacity ? cachedCapacity * 2 : 4;
                void* expanded = realloc(cached, nextCapacity * sizeof(InstrumentDefinition));
                if (expanded)
                {
                    cached = static_cast<InstrumentDefinition*>(expanded);
                    cachedCapacity = nextCapacity;
                }
                else
                {
                    cacheComplete = false;
                    free(cached);
                    cached = nullptr;
                    cachedCount = cachedCapacity = 0;
                }
            }
            if (cacheComplete)
                cached[cachedCount++] = instrument;
        }

        if (!foundAny)
            copyText(
                firstInstrument,
                sizeof(firstInstrument),
                instrument.section);

        sendChunk("<option value=\"");
        sendEscaped(instrument.section);
        sendChunk("\">");
        sendEscaped(instrument.name);
        sendChunk("</option>");

        foundAny = true;
    }

    file.close();
    sendChunk("</select></div>");
    const uint32_t selectorEndMs = millis();
    Serial.printf("[GUI DETAIL] Selector pass: %lu ms; loadInstrument: %lu ms; count: %u\n",
                  (unsigned long)(selectorEndMs - renderingStartMs),
                  (unsigned long)selectorLoadMs, selectorCount);

    if (!foundAny)
    {
        if (configuredSections == 0)
            sendChunk(
                "<div class=\"result\"><b>No instruments configured.</b> "
                "Upload /config/instruments.cfg and the corresponding .cfg "
                "profile files to /instruments using SD Card File Upload. "
                "Restart the controller after uploading instruments.cfg."
                "</div>");
        else if (missingProfiles > 0)
            sendChunk(
                "<div class=\"result\"><b>Instrument profiles not found.</b> "
                "One or more profiles referenced by /config/instruments.cfg "
                "are missing. Upload the required .cfg files to /instruments "
                "using SD Card File Upload, then reload the webpage."
                "</div>");
        else
            sendChunk(
                "<div class=\"result\"><b>No usable instruments found.</b> "
                "Check the instrument definitions in /config/instruments.cfg "
                "and the contents of their corresponding profile files."
                "</div>");
        free(cached);
        return;
    }

    if (cacheComplete)
    {
        const uint32_t cardPassStartMs = millis();
        for (size_t i = 0; i < cachedCount; ++i)
        {
            const uint32_t renderStartMs = millis();
            renderInstrument(cached[i], cardInstrument, cardCapability, cardValue, cardResult);
            cardRenderMs += millis() - renderStartMs;
            ++cardCount;
        }
        free(cached);
        const uint32_t cardPassEndMs = millis();
        Serial.printf("[GUI DETAIL] Card pass (cached): %lu ms; loadInstrument: 0 ms; renderInstrument: %lu ms; count: %u\n",
                      (unsigned long)(cardPassEndMs - cardPassStartMs),
                      (unsigned long)cardRenderMs, cardCount);
        Serial.printf("[GUI DETAIL] renderInstruments total: %lu ms; outside two passes: %lu ms\n",
                      (unsigned long)(cardPassEndMs - renderingStartMs),
                      (unsigned long)((cardPassEndMs - renderingStartMs) -
                          (selectorEndMs - renderingStartMs) -
                          (cardPassEndMs - cardPassStartMs)));
        return;
    }

    Serial.println("[GUI DETAIL] Cache allocation failed; using original second pass.");
    file = SD.open(
        "/config/instruments.cfg",
        FILE_READ);

    if (!file)
    {
        sendChunk(
            "<div class=\"result\">"
            "Unable to reopen /config/instruments.cfg."
            "</div>");
        free(cached);
        return;
    }

    const uint32_t cardPassStartMs = millis();
    while (readLine(file, line, sizeof(line)))
    {
        trim(line);

        if (line[0] == '\0' ||
            line[0] == '#' ||
            line[0] == ';')
            continue;

        const size_t length = strlen(line);

        if (length < 3 ||
            line[0] != '[' ||
            line[length - 1] != ']')
            continue;

        line[length - 1] = '\0';
        char* section = line + 1;
        trim(section);

        InstrumentDefinition instrument;

        const uint32_t loadStartMs = millis();
        const bool loaded = loadInstrument(section, instrument);
        cardLoadMs += millis() - loadStartMs;
        if (!loaded)
            continue;
        ++cardCount;

        const uint32_t renderStartMs = millis();
        renderInstrument(
            instrument,
            cardInstrument,
            cardCapability,
            cardValue,
            cardResult);
        cardRenderMs += millis() - renderStartMs;
    }

    file.close();
    const uint32_t cardPassEndMs = millis();
    Serial.printf("[GUI DETAIL] Card pass: %lu ms; loadInstrument: %lu ms; renderInstrument: %lu ms; count: %u\n",
                  (unsigned long)(cardPassEndMs - cardPassStartMs),
                  (unsigned long)cardLoadMs,
                  (unsigned long)cardRenderMs, cardCount);
    Serial.printf("[GUI DETAIL] renderInstruments total: %lu ms; outside two passes: %lu ms\n",
                  (unsigned long)(cardPassEndMs - renderingStartMs),
                  (unsigned long)((cardPassEndMs - renderingStartMs) -
                      (selectorEndMs - renderingStartMs) -
                      (cardPassEndMs - cardPassStartMs)));
}

void WebServerManager::renderInstrument(
    const InstrumentDefinition& instrument,
    const char* cardInstrument,
    const char* cardCapability,
    const char* cardValue,
    const char* cardResult)
{
    sendChunk("<div class=\"instrument-card\" data-instrument-card=\"");
    sendEscaped(instrument.section);
    sendChunk("\"><h3>");
    sendEscaped(instrument.name);
    sendChunk("</h3><div class=\"model\">");
    sendEscaped(instrument.manufacturer);
    sendChunk(" ");
    sendEscaped(instrument.model);
    sendChunk("</div>");

    // Display connection parameters without affecting instrument I/O.
    if (instrument.interfaceType[0])
    {
        sendChunk("<div class=\"model\"><strong>Interface:</strong> ");
        if (strcmp(instrument.interfaceType, "GPIB") == 0)
        {
            sendChunk("GPIB");
            if (instrument.address[0]) { sendChunk(" &mdash; Address "); sendEscaped(instrument.address); }
        }
        else if (strcmp(instrument.interfaceType, "RS232") == 0)
        {
            sendChunk("RS-232");
            if (instrument.port[0]) { sendChunk(" &mdash; Port "); sendEscaped(instrument.port); }
            if (instrument.baud[0]) { sendChunk(" &middot; "); sendEscaped(instrument.baud); sendChunk(" baud"); }
            if (instrument.dataBits[0] || instrument.parity[0] || instrument.stopBits[0])
            {
                sendChunk(" &middot; ");
                sendEscaped(instrument.dataBits);
                const char parity = instrument.parity[0];
                sendChunk(parity == 'e' || parity == 'E' ? "E" :
                          parity == 'o' || parity == 'O' ? "O" :
                          parity == 'm' || parity == 'M' ? "M" :
                          parity == 's' || parity == 'S' ? "S" : "N");
                sendEscaped(instrument.stopBits);
            }
        }
        else if (strcmp(instrument.interfaceType, "TCP") == 0)
        {
            sendChunk("LAN (TCP)");
            if (instrument.host[0]) { sendChunk(" &mdash; "); sendEscaped(instrument.host); }
            if (instrument.port[0]) { sendChunk(":"); sendEscaped(instrument.port); }
        }
        else
            sendEscaped(instrument.interfaceType);
        sendChunk("</div>");
    }

    const unsigned long instrumentStartMs = millis();
    renderCapabilities(
        instrument,
        cardInstrument,
        cardCapability,
        cardValue,
        cardResult);
    const unsigned long instrumentElapsedMs = millis() - instrumentStartMs;
    Serial.printf("[GUI PROFILE] %s (%s): %lu ms\n",
                  instrument.section, instrument.profileName,
                  instrumentElapsedMs);

    sendChunk("</div>");
}

void WebServerManager::renderCapabilities(
    const InstrumentDefinition& instrument,
    const char* cardInstrument,
    const char* cardCapability,
    const char* cardValue,
    const char* cardResult)
{
    char profilePath[160];

    const int pathLength =
        snprintf(
            profilePath,
            sizeof(profilePath),
            "/instruments/%s",
            instrument.profileName);

    if (pathLength < 0 ||
        static_cast<size_t>(pathLength) >= sizeof(profilePath))
    {
        sendChunk("<div class=\"result\">Invalid profile path.</div>");
        return;
    }

    renderFunctionSelector(instrument, profilePath);

    File file = SD.open(profilePath, FILE_READ);
    if (!file)
    {
        sendChunk("<div class=\"result\">Unable to open instrument profile.</div>");
        return;
    }

    bool renderedAny = false;
    char line[192];
    char section[32] = "";
    char sectionType[24] = "";
    char sectionMembers[256] = "";
    char activeGroup[32] = "";
    char activeMembers[256] = "";
    CapabilityDefinition parsedCapability = {};
    bool parsedValid = true;

    auto processSection = [&]()
    {
        if (section[0] == '\0' || strcmp(section, "instrument") == 0)
            return;

        if (strcmp(sectionType, "group") == 0)
        {
            copyText(activeGroup, sizeof(activeGroup), section);
            copyText(activeMembers, sizeof(activeMembers), sectionMembers);
            return;
        }

        // Use fields collected during this sequential profile read.
        // Avoid reopening and rescanning the SD file for every capability.
        CapabilityDefinition& capability = parsedCapability;
        if (!parsedValid || capability.type[0] == '\0' ||
            capability.label[0] == '\0')
            return;
        const bool isNumber = strcmp(capability.type, "number") == 0;
        const bool isSelect = strcmp(capability.type, "select") == 0;
        const bool isAction = strcmp(capability.type, "action") == 0;
        const bool isToggle = strcmp(capability.type, "toggle_action") == 0;
        if (isNumber && (capability.quantity[0] == '\0' || capability.baseUnit[0] == '\0')) return;
        if (isSelect && capability.options[0] == '\0') return;
        if (isAction && !capability.canExecute()) return;
        if (isToggle && (!capability.canExecute() || !capability.canExecuteOn() || capability.labelOn[0] == '\0')) return;
        if (!isNumber && !isSelect && !isAction && !isToggle) return;
        if (!isAction && !isToggle && !capability.canSet() && !capability.canQuery()) return;

        if (strcmp(capability.ui, "false") == 0)
            return;

        const char* functionSection = "__basic__";
        if (activeGroup[0] != '\0' && activeMembers[0] != '\0')
        {
            char membersCopy[sizeof(activeMembers)];
            copyText(membersCopy, sizeof(membersCopy), activeMembers);
            char* save = nullptr;
            char* member = strtok_r(membersCopy, ",", &save);
            while (member != nullptr)
            {
                trim(member);
                if (strcmp(member, section) == 0)
                {
                    functionSection = activeGroup;
                    break;
                }
                member = strtok_r(nullptr, ",", &save);
            }
        }

        renderCapability(
            instrument,
            capability,
            functionSection,
            cardInstrument,
            cardCapability,
            cardValue,
            cardResult);
        renderedAny = true;
    };

    while (readLine(file, line, sizeof(line)))
    {
        trim(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';')
            continue;

        const size_t length = strlen(line);
        if (length >= 3 && line[0] == '[' && line[length - 1] == ']')
        {
            processSection();
            line[length - 1] = '\0';
            copyText(section, sizeof(section), line + 1);
            trim(section);
            sectionType[0] = '\0';
            sectionMembers[0] = '\0';
            memset(&parsedCapability, 0, sizeof(parsedCapability));
            parsedValid = copyText(parsedCapability.section, sizeof(parsedCapability.section), section);
            continue;
        }

        char* equals = strchr(line, '=');
        if (equals == nullptr)
            continue;
        *equals = '\0';
        char* key = line;
        char* value = equals + 1;
        trim(key);
        trim(value);

        if (strcmp(key, "type") == 0)
        {
            copyText(sectionType, sizeof(sectionType), value);
            if (!copyText(parsedCapability.type, sizeof(parsedCapability.type), value)) parsedValid = false;
        }
        else if (strcmp(key, "members") == 0)
            copyText(sectionMembers, sizeof(sectionMembers), value);
        // Same capability fields as loadCapability(), collected without
        // performing a fresh ConfigurationFile::getValues() for each section.
#define GUI_FIELD(k, member) else if (strcmp(key, k) == 0) { if (!copyText(parsedCapability.member, sizeof(parsedCapability.member), value)) parsedValid = false; }
        GUI_FIELD("label", label)
        GUI_FIELD("quantity", quantity)
        GUI_FIELD("base_unit", baseUnit)
        GUI_FIELD("set", setTemplate)
        GUI_FIELD("query", queryCommand)
        GUI_FIELD("minimum", minimum)
        GUI_FIELD("maximum", maximum)
        GUI_FIELD("response_prefix", responsePrefix)
        GUI_FIELD("response_terminator", responseTerminator)
        GUI_FIELD("response_unit", responseUnit)
        GUI_FIELD("options", options)
        GUI_FIELD("action", actionCommand)
        GUI_FIELD("action_on", actionOnCommand)
        GUI_FIELD("label_on", labelOn)
        GUI_FIELD("resets_toggle", resetsToggle)
        GUI_FIELD("visible_when", visibleWhen)
        GUI_FIELD("ui", ui)
#undef GUI_FIELD
    }

    processSection();
    file.close();

    if (!renderedAny)
    {
        sendChunk("<div class=\"result\">No supported capabilities found in this profile.</div>");
    }
}

void WebServerManager::renderFunctionSelector(
    const InstrumentDefinition& instrument,
    const char* profilePath)
{
    sendChunk("<div class=\"function-selector\"><label>Function</label>"
              "<select data-function-selector data-instrument=\"");
    sendEscaped(instrument.section);
    sendChunk("\" onchange=\"return asyncFunctionChange(this);\">"
              "<option value=\"__basic__\">");
    sendEscaped(instrument.defaultFunctionLabel);
    sendChunk("</option>");

    File file = SD.open(profilePath, FILE_READ);
    if (file)
    {
        char line[192];
        char section[32] = "";
        char sectionType[24] = "";
        char sectionLabel[80] = "";
        bool enterBeforeAction = false;

        auto processSection = [&]()
        {
            if (section[0] == '\0' || strcmp(sectionType, "group") != 0 ||
                sectionLabel[0] == '\0')
                return;

            sendChunk("<option value=\"");
            sendEscaped(section);
            if (enterBeforeAction)
                sendChunk("\" data-enter-before-action=\"true");
            sendChunk("\">");
            sendEscaped(sectionLabel);
            sendChunk("</option>");
        };

        while (readLine(file, line, sizeof(line)))
        {
            trim(line);
            if (line[0] == '\0' || line[0] == '#' || line[0] == ';')
                continue;

            const size_t length = strlen(line);
            if (length >= 3 && line[0] == '[' && line[length - 1] == ']')
            {
                processSection();
                line[length - 1] = '\0';
                copyText(section, sizeof(section), line + 1);
                trim(section);
                sectionType[0] = '\0';
                sectionLabel[0] = '\0';
                enterBeforeAction = false;
                continue;
            }

            char* equals = strchr(line, '=');
            if (equals == nullptr)
                continue;
            *equals = '\0';
            char* key = line;
            char* value = equals + 1;
            trim(key);
            trim(value);

            if (strcmp(key, "type") == 0)
                copyText(sectionType, sizeof(sectionType), value);
            else if (strcmp(key, "label") == 0)
                copyText(sectionLabel, sizeof(sectionLabel), value);
            else if (strcmp(key, "enter_before_action") == 0)
                enterBeforeAction = (strcmp(value, "true") == 0);
        }

        processSection();
        file.close();
    }

    sendChunk("</select></div>");
}

void WebServerManager::renderCapability(
    const InstrumentDefinition& instrument,
    const CapabilityDefinition& capability,
    const char* functionSection,
    const char* cardInstrument,
    const char* cardCapability,
    const char* cardValue,
    const char* cardResult)
{
    // Build one capability in heap-backed String storage and send it
    // as a single HTTP chunk.  This preserves capability-at-a-time streaming
    // while avoiding dozens of small Wi-Fi/TCP writes per capability.
    String capabilityHtml;
    capabilityHtml.reserve(3072);

    auto appendEscaped = [&capabilityHtml](const char* text)
    {
        if (text == nullptr)
            return;

        while (*text != '\0')
        {
            switch (*text)
            {
                case '&':  capabilityHtml += "&amp;";  break;
                case '<':  capabilityHtml += "&lt;";   break;
                case '>':  capabilityHtml += "&gt;";   break;
                case '"': capabilityHtml += "&quot;"; break;
                case '\'': capabilityHtml += "&#39;"; break;
                default:   capabilityHtml += *text;    break;
            }
            ++text;
        }
    };

    const bool selected =
        cardInstrument != nullptr &&
        cardCapability != nullptr &&
        strcmp(cardInstrument, instrument.section) == 0 &&
        strcmp(cardCapability, capability.section) == 0;

    capabilityHtml += String("<div class=\"capability\" data-function=\"");
    appendEscaped(functionSection);
    capabilityHtml += String("\" data-capability=\"");
    appendEscaped(capability.section);
    capabilityHtml += String("\"");
    if (capability.visibleWhen[0] != '\0')
    {
        capabilityHtml += String(" data-visible-when=\"");
        appendEscaped(capability.visibleWhen);
        capabilityHtml += String("\"");
    }
    capabilityHtml += String("><form method=\"post\" action=\"/capability\" onsubmit=\"return false;\">"
              "<input type=\"hidden\" name=\"instrument\" value=\"");

    appendEscaped(instrument.section);

    capabilityHtml += String(
        "\"><input type=\"hidden\" name=\"capability\" value=\"");

    appendEscaped(capability.section);

    capabilityHtml += String("\">");

    if (strcmp(capability.type, "toggle_action") == 0)
    {
        capabilityHtml += String("<div class=\"buttons\"><button type=\"button\" "
                  "data-toggle-capability=\"");
        appendEscaped(capability.section);
        capabilityHtml += String("\" data-state=\"off\" data-label-off=\"");
        appendEscaped(capability.label);
        capabilityHtml += String("\" data-label-on=\"");
        appendEscaped(capability.labelOn);
        capabilityHtml += String("\" onclick=\"return asyncToggleCapability(this);\">");
        appendEscaped(capability.label);
        capabilityHtml += String("</button></div>");
    }
    else if (strcmp(capability.type, "action") == 0)
    {
        capabilityHtml += String("<div class=\"buttons\"><button type=\"button\" ");
        if (capability.resetsToggle[0] != '\0')
        {
            capabilityHtml += String("data-resets-toggle=\"");
            appendEscaped(capability.resetsToggle);
            capabilityHtml += String("\" ");
        }
        capabilityHtml += String("onclick=\"return asyncCapability(this,'execute');\">");
        appendEscaped(capability.label);
        capabilityHtml += String("</button></div>");
    }
    else
    {
        capabilityHtml += String("<label>");
        appendEscaped(capability.label);

        if (strcmp(capability.type, "select") == 0)
        {
        capabilityHtml += String("</label><select name=\"value\" data-select-capability=\"");
        appendEscaped(capability.section);
        capabilityHtml += String("\" onchange=\"updateConditionalVisibility();\">");

        char optionsCopy[sizeof(capability.options)];
        copyText(optionsCopy, sizeof(optionsCopy), capability.options);

        char* save = nullptr;
        char* item = strtok_r(optionsCopy, ",", &save);

        while (item != nullptr)
        {
            trim(item);
            char* separator = strchr(item, ':');

            if (separator != nullptr)
            {
                *separator = '\0';
                char* optionLabel = item;
                char* optionValue = separator + 1;
                trim(optionLabel);
                trim(optionValue);

                capabilityHtml += String("<option value=\"");
                appendEscaped(optionValue);
                capabilityHtml += String("\"");

                if (selected &&
                    cardValue != nullptr &&
                    strcmp(cardValue, optionValue) == 0)
                    capabilityHtml += String(" selected");

                capabilityHtml += String(">");
                appendEscaped(optionLabel);
                capabilityHtml += String("</option>");
            }

            item = strtok_r(nullptr, ",", &save);
        }

        capabilityHtml += String("</select>");
    }
    else
    {
        capabilityHtml += String(
            "</label><input name=\"value\" type=\"text\" value=\"");

        if (selected && cardValue != nullptr)
            appendEscaped(cardValue);

        capabilityHtml += String(
            "\" placeholder=\"Enter value with optional engineering unit\">");
    }

    capabilityHtml += String("<div class=\"buttons\">");

    if (capability.canSet())
    {
        capabilityHtml += String(
            "<button type=\"button\" name=\"action\" value=\"set\" "
            "onclick=\"return asyncCapability(this,'set');\">Set</button>");
    }

    if (capability.canQuery())
    {
        capabilityHtml += String(
            "<button type=\"button\" name=\"action\" value=\"read\" "
            "formnovalidate onclick=\"return asyncCapability(this,'read');\">"
            "Read</button>");
    }

    capabilityHtml += String("</div>");

    if (strcmp(capability.type, "number") == 0 &&
        (capability.minimum[0] != '\0' ||
         capability.maximum[0] != '\0'))
    {
        capabilityHtml += String("<div class=\"range\">Profile range: ");

        if (capability.minimum[0] != '\0')
            appendEscaped(capability.minimum);
        else
            capabilityHtml += String("?");

        capabilityHtml += String(" to ");

        if (capability.maximum[0] != '\0')
            appendEscaped(capability.maximum);
        else
            capabilityHtml += String("?");

        capabilityHtml += String(" ");
        appendEscaped(capability.baseUnit);
        capabilityHtml += String("</div>");
    }


    }

    if (selected &&
        cardResult != nullptr &&
        cardResult[0] != '\0')
    {
        capabilityHtml += String(
            "<div class=\"result\"><strong>Result:</strong> ");
        appendEscaped(cardResult);
        capabilityHtml += String("</div>");
    }

    capabilityHtml += String("</form></div>");

    sendChunk(capabilityHtml);
}

void WebServerManager::flushHtmlBuffer()
{
    if (m_htmlBufferUsed == 0)
        return;
    m_server.sendContent(m_htmlBuffer, m_htmlBufferUsed);
    m_htmlBufferUsed = 0;
}

void WebServerManager::appendHtmlBuffer(const char* data, size_t length)
{
    if (data == nullptr || length == 0)
        return;
    if (!m_htmlBufferActive)
    {
        m_server.sendContent(data, length);
        return;
    }
    while (length > 0)
    {
        size_t available = sizeof(m_htmlBuffer) - m_htmlBufferUsed;
        if (available == 0)
        {
            flushHtmlBuffer();
            available = sizeof(m_htmlBuffer);
        }
        const size_t count = (length < available) ? length : available;
        memcpy(m_htmlBuffer + m_htmlBufferUsed, data, count);
        m_htmlBufferUsed += count;
        data += count;
        length -= count;
        if (m_htmlBufferUsed == sizeof(m_htmlBuffer))
            flushHtmlBuffer();
    }
}

void WebServerManager::sendChunk(const String& text)
{
    appendHtmlBuffer(text.c_str(), text.length());
}

void WebServerManager::sendChunk(const char* text)
{
    if (text != nullptr)
        appendHtmlBuffer(text, strlen(text));
}

void WebServerManager::sendEscaped(const char* text)
{
    if (text == nullptr)
        return;

    char plain[96];
    size_t plainLength = 0;

    while (*text != '\0')
    {
        const char* entity = nullptr;

        switch (*text)
        {
            case '&':  entity = "&amp;";  break;
            case '<':  entity = "&lt;";   break;
            case '>':  entity = "&gt;";   break;
            case '"':  entity = "&quot;"; break;
            case '\'': entity = "&#39;";  break;
            default: break;
        }

        if (entity != nullptr)
        {
            if (plainLength > 0)
            {
                plain[plainLength] = '\0';
                sendChunk(plain);
                plainLength = 0;
            }

            sendChunk(entity);
        }
        else
        {
            if (plainLength + 1 >= sizeof(plain))
            {
                plain[plainLength] = '\0';
                sendChunk(plain);
                plainLength = 0;
            }

            plain[plainLength++] = *text;
        }

        ++text;
    }

    if (plainLength > 0)
    {
        plain[plainLength] = '\0';
        sendChunk(plain);
    }
}

void WebServerManager::handleControlBoxConfig()
{
    String path = "/config/control_boxes.cfg";
    if (m_server.hasArg("name")) {
        const String name = m_server.arg("name");
        if (!validSavedConfigurationName(name)) {
            m_server.send(400, "text/plain", "Invalid configuration name.");
            return;
        }
        if (name != "default") path = "/config/workspaces/" + name + ".cfg";
    }
    File file = SD.open(path.c_str(), FILE_READ);

    if (!file)
    {
        m_server.send(404, "text/plain", "");
        return;
    }

    m_server.streamFile(file, "text/plain");
    file.close();
}

void WebServerManager::handleSendControlBox()
{
    if (!m_server.hasArg("instrument") ||
        !m_server.hasArg("function"))
    {
        m_server.send(400, "text/plain",
                      "Invalid SEND request.");
        return;
    }

    const String instrumentSection = m_server.arg("instrument");
    const String functionSection = m_server.arg("function");

    InstrumentDefinition instrument;
    if (!instrumentSection.length() ||
        !loadInstrument(instrumentSection.c_str(), instrument))
    {
        m_server.send(400, "text/plain",
                      "SEND aborted before instrument I/O: invalid instrument.");
        return;
    }

    //
    // Pass 1: validate every active settable value before issuing ANY
    // instrument command.  No GPIB activity is permitted in this pass.
    //
    for (int argument = 0; argument < m_server.args(); ++argument)
    {
        const String name = m_server.argName(argument);
        if (!name.startsWith("value_"))
            continue;

        const String capabilitySection = name.substring(6);
        const String enteredValue = m_server.arg(argument);

        CapabilityDefinition capability;
        if (!loadCapability(
                instrument,
                capabilitySection.c_str(),
                capability))
        {
            char message[224];
            snprintf(message, sizeof(message),
                     "SEND aborted before instrument I/O: unknown parameter '%s'.",
                     capabilitySection.c_str());
            m_server.send(400, "text/plain", message);
            return;
        }

        // Read-only fields and executable actions are not part of SEND.
        if (!capability.canSet())
            continue;

        if (enteredValue.length() == 0)
        {
            char message[224];
            snprintf(message, sizeof(message),
                     "SEND aborted before instrument I/O. %s: a value is required.",
                     capability.label);
            m_server.send(400, "text/plain", message);
            return;
        }

        if (strcmp(capability.type, "select") == 0)
        {
            bool validOption = false;
            char optionsCopy[sizeof(capability.options)];
            copyText(optionsCopy, sizeof(optionsCopy), capability.options);

            char* save = nullptr;
            char* item = strtok_r(optionsCopy, ",", &save);

            while (item != nullptr)
            {
                trim(item);
                char* separator = strchr(item, ':');

                if (separator != nullptr)
                {
                    *separator = '\0';
                    char* optionValue = separator + 1;
                    trim(optionValue);

                    if (enteredValue == optionValue)
                    {
                        validOption = true;
                        break;
                    }
                }

                item = strtok_r(nullptr, ",", &save);
            }

            if (!validOption)
            {
                char message[256];
                snprintf(message, sizeof(message),
                         "SEND aborted before instrument I/O. %s: selected value '%s' is not defined by the profile.",
                         capability.label, enteredValue.c_str());
                m_server.send(400, "text/plain", message);
                return;
            }
        }
        else
        {
            double baseValue = 0.0;
            const EngineeringValue::Result parseResult =
                EngineeringValue::parse(
                    enteredValue.c_str(),
                    capability.quantity,
                    capability.baseUnit,
                    baseValue);

            if (parseResult != EngineeringValue::Result::Success)
            {
                char message[256];
                snprintf(message, sizeof(message),
                         "SEND aborted before instrument I/O. %s: invalid value '%s' (%s).",
                         capability.label,
                         enteredValue.c_str(),
                         EngineeringValue::resultText(parseResult));
                m_server.send(400, "text/plain", message);
                return;
            }

            double minimumValue = 0.0;
            double maximumValue = 0.0;
            bool hasMinimum = false;
            bool hasMaximum = false;

            if (capability.minimum[0] != '\0')
            {
                if (EngineeringValue::parse(
                        capability.minimum,
                        capability.quantity,
                        capability.baseUnit,
                        minimumValue) != EngineeringValue::Result::Success)
                {
                    m_server.send(
                        500, "text/plain",
                        "SEND aborted before instrument I/O: profile has an invalid minimum.");
                    return;
                }
                hasMinimum = true;
            }

            if (capability.maximum[0] != '\0')
            {
                if (EngineeringValue::parse(
                        capability.maximum,
                        capability.quantity,
                        capability.baseUnit,
                        maximumValue) != EngineeringValue::Result::Success)
                {
                    m_server.send(
                        500, "text/plain",
                        "SEND aborted before instrument I/O: profile has an invalid maximum.");
                    return;
                }
                hasMaximum = true;
            }

            if ((hasMinimum && baseValue < minimumValue) ||
                (hasMaximum && baseValue > maximumValue))
            {
                char minimumText[64] = "";
                char maximumText[64] = "";
                char message[320];

                if (hasMinimum)
                    EngineeringValue::format(
                        minimumValue,
                        capability.quantity,
                        capability.baseUnit,
                        minimumText,
                        sizeof(minimumText));

                if (hasMaximum)
                    EngineeringValue::format(
                        maximumValue,
                        capability.quantity,
                        capability.baseUnit,
                        maximumText,
                        sizeof(maximumText));

                if (hasMinimum && hasMaximum)
                    snprintf(message, sizeof(message),
                             "SEND aborted before instrument I/O. %s: value '%s' must be between %s and %s.",
                             capability.label, enteredValue.c_str(),
                             minimumText, maximumText);
                else if (hasMinimum)
                    snprintf(message, sizeof(message),
                             "SEND aborted before instrument I/O. %s: value '%s' must be at least %s.",
                             capability.label, enteredValue.c_str(),
                             minimumText);
                else
                    snprintf(message, sizeof(message),
                             "SEND aborted before instrument I/O. %s: value '%s' must not exceed %s.",
                             capability.label, enteredValue.c_str(),
                             maximumText);

                m_server.send(400, "text/plain", message);
                return;
            }
        }
    }

    //
    // Validation passed completely.  Enter the selected function first.
    // This is required, for example, to put the Marconi into SWEPT mode
    // before applying sweep parameters.  Only now may instrument I/O begin.
    //
    char semanticCommand[224];
    int commandLength = snprintf(
        semanticCommand,
        sizeof(semanticCommand),
        "ENTER %s %s",
        instrument.section,
        functionSection.c_str());

    if (commandLength < 0 ||
        static_cast<size_t>(commandLength) >= sizeof(semanticCommand))
    {
        m_server.send(400, "text/plain",
                      "SEND aborted before instrument I/O: function command is too long.");
        return;
    }

    char response[512];
    size_t responseLength = 0;
    response[0] = '\0';

    CommandExecutor::Result result =
        m_commandExecutor.execute(
            semanticCommand,
            response,
            sizeof(response),
            responseLength,
            5000);

    if (result != CommandExecutor::Result::Success)
    {
        char message[256];
        snprintf(message, sizeof(message),
                 "SEND stopped: function entry failed (%s). No parameter SETs were attempted.",
                 commandResultMessage(CommandExecutor::resultText(result)).c_str());
        m_server.send(500, "text/plain", message);
        return;
    }

    //
    // Pass 2: all values are known valid.  Apply settable parameters in
    // browser order and stop immediately if any transaction fails.
    //
    unsigned sentCount = 0;

    for (int argument = 0; argument < m_server.args(); ++argument)
    {
        const String name = m_server.argName(argument);
        if (!name.startsWith("value_"))
            continue;

        const String capabilitySection = name.substring(6);
        const String enteredValue = m_server.arg(argument);

        CapabilityDefinition capability;
        if (!loadCapability(
                instrument,
                capabilitySection.c_str(),
                capability))
        {
            m_server.send(
                500, "text/plain",
                "SEND stopped: profile changed after validation.");
            return;
        }

        if (!capability.canSet())
            continue;

        if (strcmp(capability.type, "select") == 0)
        {
            commandLength = snprintf(
                semanticCommand,
                sizeof(semanticCommand),
                "SET %s %s %s",
                instrument.section,
                capability.section,
                enteredValue.c_str());
        }
        else
        {
            double baseValue = 0.0;
            if (EngineeringValue::parse(
                    enteredValue.c_str(),
                    capability.quantity,
                    capability.baseUnit,
                    baseValue) != EngineeringValue::Result::Success)
            {
                m_server.send(
                    500, "text/plain",
                    "SEND stopped: value changed after validation.");
                return;
            }

            commandLength = snprintf(
                semanticCommand,
                sizeof(semanticCommand),
                "SET %s %s %.12g",
                instrument.section,
                capability.section,
                baseValue);
        }

        if (commandLength < 0 ||
            static_cast<size_t>(commandLength) >= sizeof(semanticCommand))
        {
            char message[224];
            snprintf(message, sizeof(message),
                     "SEND stopped at %s: command is too long.",
                     capability.label);
            m_server.send(500, "text/plain", message);
            return;
        }

        response[0] = '\0';
        responseLength = 0;

        result = m_commandExecutor.execute(
            semanticCommand,
            response,
            sizeof(response),
            responseLength,
            5000);

        if (result != CommandExecutor::Result::Success)
        {
            char message[256];
            snprintf(message, sizeof(message),
                     "SEND stopped at %s after %u successful parameter SET(s): %s.",
                     capability.label,
                     sentCount,
                     commandResultMessage(CommandExecutor::resultText(result)).c_str());
            m_server.send(500, "text/plain", message);
            return;
        }

        ++sentCount;
    }

    char message[192];
    snprintf(message, sizeof(message),
             "Control Box SEND completed successfully: %u parameter(s) applied.",
             sentCount);
    m_server.send(200, "text/plain", message);
}

void WebServerManager::handleSaveControlBoxConfig()
{
    if (m_macroPending || m_macroRunning)
    {
        m_server.send(409, "text/plain", "Workspace cannot be saved while a macro is executing.");
        return;
    }

    String configPath = "/config/control_boxes.cfg";
    String temporaryPath = "/config/.control_boxes.tmp";
    String backupPath = "/config/.control_boxes.bak";
    if (m_server.hasArg("name")) {
        const String name = m_server.arg("name");
        if (!validSavedConfigurationName(name)) {
            m_server.send(400, "text/plain", "Invalid configuration name.");
            return;
        }
        if (name != "default") {
            if (!SD.exists("/config/workspaces") &&
                !SD.mkdir("/config/workspaces")) {
                m_server.send(500, "text/plain", "Cannot create workspaces directory.");
                return;
            }
            configPath = "/config/workspaces/" + name + ".cfg";
            temporaryPath = "/config/workspaces/." + name + ".tmp";
            backupPath = "/config/workspaces/." + name + ".bak";
        }
    }
    // Save As must never overwrite an existing file without an explicit mode.
    if (m_server.hasArg("mode")) {
        const String mode = m_server.arg("mode");
        if (!m_server.hasArg("name") || m_server.arg("name") == "default" ||
            (mode != "create" && mode != "replace")) {
            m_server.send(400, "text/plain", "Invalid Save As request.");
            return;
        }
        const bool exists = SD.exists(configPath.c_str());
        if (mode == "create" && exists) {
            m_server.send(409, "text/plain", "Configuration already exists. No file overwritten.");
            return;
        }
        if (mode == "replace" && !exists) {
            m_server.send(409, "text/plain", "Configuration no longer exists. No file changed.");
            return;
        }
    }
    const char* ConfigPath = configPath.c_str();
    const char* TemporaryPath = temporaryPath.c_str();
    const char* BackupPath = backupPath.c_str();

    const String configuration = m_server.arg("plain");
    if (configuration.length() == 0 || configuration.length() > 16384)
    {
        m_server.send(400, "text/plain", "Invalid or oversized workspace configuration.");
        return;
    }

    // Validate the entire proposed configuration before writing the SD card.
    // This performs profile parsing only -- no instrument I/O.
    String currentSection;
    String currentInstrument;
    int declaredPanelCount = -1;
    unsigned int seenPanels = 0;
    unsigned int seenMacroPanels = 0;
    int declaredMacroCount = -1;
    String currentMacro;
    size_t lineStart = 0;

    while (lineStart < configuration.length())
    {
        int foundEnd = configuration.indexOf('\n', lineStart);
        size_t lineEnd = (foundEnd < 0) ? configuration.length()
                                       : static_cast<size_t>(foundEnd);
        String line = configuration.substring(lineStart, lineEnd);
        line.trim();

        if (line.length() && line.charAt(0) != ';' && line.charAt(0) != '#')
        {
            if (line.charAt(0) == '[' && line.charAt(line.length()-1) == ']')
            {
                currentSection = line.substring(1, line.length()-1);
                currentSection.trim();
                currentInstrument = "";
                currentMacro = "";
                // Saved panels must be consecutive, with no duplicates.
                if (currentSection.startsWith("box"))
                {
                    const String expected = String("box") + String(seenPanels + 1);
                    if (currentSection != expected || seenPanels >= 8)
                    {
                        m_server.send(400, "text/plain",
                                      "Configuration not saved: panel sections must be consecutive (box1..box8).");
                        return;
                    }
                    ++seenPanels;
                }
                else if (currentSection.startsWith("macro"))
                {
                    const String expected = String("macro") + String(seenMacroPanels + 1);
                    if (currentSection != expected || seenMacroPanels >= 12)
                    {
                        m_server.send(400, "text/plain", "Macro sections must be consecutive (macro1..macro12).");
                        return;
                    }
                    ++seenMacroPanels;
                }
                else if (currentSection != "workspace")
                {
                    m_server.send(400, "text/plain",
                                  "Configuration not saved: unknown section.");
                    return;
                }
            }
            else
            {
                int equals = line.indexOf('=');
                if (equals <= 0 || currentSection.length() == 0)
                {
                    m_server.send(400, "text/plain",
                                  "Configuration not saved: invalid configuration syntax.");
                    return;
                }

                String key=line.substring(0,equals);
                String value=line.substring(equals+1);
                key.trim(); value.trim();

                if (currentSection == "workspace")
                {
                    if (key != "panel_count" && key != "macro_panel_count")
                    {
                        m_server.send(400, "text/plain", "Unknown workspace metadata.");
                        return;
                    }
                    if ((key == "panel_count" && declaredPanelCount >= 0) ||
                        (key == "macro_panel_count" && declaredMacroCount >= 0))
                    {
                        m_server.send(400, "text/plain",
                                      "Configuration not saved: invalid workspace metadata.");
                        return;
                    }
                    char* end = nullptr;
                    long count = strtol(value.c_str(), &end, 10);
                    if (end == value.c_str() || *end != '\0' ||
                        count < (key == "macro_panel_count" ? 0 : 1) ||
                        count > (key == "macro_panel_count" ? 12 : 8))
                    {
                        m_server.send(400, "text/plain",
                                      "Configuration not saved: invalid panel count (instruments 1..8, macros 0..12).");
                        return;
                    }
                    if (key == "macro_panel_count") declaredMacroCount = static_cast<int>(count);
                    else declaredPanelCount = static_cast<int>(count);
                }
                else if (currentSection.startsWith("macro"))
                {
                    if (key == "file")
                    {
                        if (currentMacro.length())
                        {
                            m_server.send(400,"text/plain","Duplicate macro filename.");return;
                        }
                        if (value.length())
                        {
                            if (value.length() > 95 || value.indexOf('/') >= 0 ||
                                value.indexOf('\\') >= 0 || value.indexOf("..") >= 0 ||
                                !value.endsWith(".cfg") || !SD.exists((String("/macros/")+value).c_str()))
                            {
                                m_server.send(400,"text/plain","Unknown or invalid macro filename.");return;
                            }
                        }
                        currentMacro = value.length() ? value : "__empty__";
                    }
                    else if (key.startsWith("param_"))
                    {
                        if (!currentMacro.length() || currentMacro == "__empty__" ||
                            key.length() <= 6 || key.length() > 100 || value.length() > 256)
                        {
                            m_server.send(400,"text/plain","Invalid macro parameter override.");return;
                        }
                    }
                    else
                    {
                        m_server.send(400,"text/plain","Unknown macro configuration key.");return;
                    }
                }
                else if (key == "instrument")
                {
                    InstrumentDefinition instrument;
                    if (!value.length() || !loadInstrument(value.c_str(), instrument))
                    {
                        char message[192];
                        snprintf(message,sizeof(message),
                                 "Configuration not saved. %s: invalid instrument '%s'.",
                                 currentSection.c_str(),value.c_str());
                        m_server.send(400,"text/plain",message);
                        return;
                    }
                    currentInstrument=value;
                }
                else if (key.startsWith("value_"))
                {
                    if (!currentInstrument.length())
                    {
                        m_server.send(400,"text/plain",
                                      "Configuration not saved: parameter appears before instrument.");
                        return;
                    }

                    String capabilitySection=key.substring(6);
                    InstrumentDefinition instrument;
                    CapabilityDefinition capability;
                    if (!loadInstrument(currentInstrument.c_str(),instrument) ||
                        !loadCapability(instrument,capabilitySection.c_str(),capability))
                    {
                        char message[224];
                        snprintf(message,sizeof(message),
                                 "Configuration not saved. %s: unknown parameter '%s'.",
                                 currentSection.c_str(),capabilitySection.c_str());
                        m_server.send(400,"text/plain",message);
                        return;
                    }

                    // an empty workspace field is valid.
                    // Save its blank value for restoration; validate only
                    // populated fields. SEND retains its own strict checks.
                    if (value.length() == 0)
                    {
                        // No instrument value has been entered or read yet.
                    }
                    else if (strcmp(capability.type,"number")==0)
                    {
                        double baseValue=0.0;
                        EngineeringValue::Result parseResult=EngineeringValue::parse(
                            value.c_str(),capability.quantity,capability.baseUnit,baseValue);
                        if (parseResult != EngineeringValue::Result::Success)
                        {
                            char message[256];
                            snprintf(message,sizeof(message),
                                     "Configuration not saved. %s - %s: invalid value '%s' (%s).",
                                     currentSection.c_str(),capability.label,value.c_str(),
                                     EngineeringValue::resultText(parseResult));
                            m_server.send(400,"text/plain",message);
                            return;
                        }

                        double minimumValue=0.0, maximumValue=0.0;
                        bool hasMinimum=false, hasMaximum=false;
                        if (capability.minimum[0])
                        {
                            if (EngineeringValue::parse(capability.minimum,capability.quantity,
                                capability.baseUnit,minimumValue)!=EngineeringValue::Result::Success)
                            {
                                m_server.send(500,"text/plain",
                                    "Configuration not saved: profile has invalid minimum.");
                                return;
                            }
                            hasMinimum=true;
                        }
                        if (capability.maximum[0])
                        {
                            if (EngineeringValue::parse(capability.maximum,capability.quantity,
                                capability.baseUnit,maximumValue)!=EngineeringValue::Result::Success)
                            {
                                m_server.send(500,"text/plain",
                                    "Configuration not saved: profile has invalid maximum.");
                                return;
                            }
                            hasMaximum=true;
                        }

                        if ((hasMinimum && baseValue<minimumValue) ||
                            (hasMaximum && baseValue>maximumValue))
                        {
                            char minText[64]="", maxText[64]="", message[320];
                            if (hasMinimum) EngineeringValue::format(minimumValue,
                                capability.quantity,capability.baseUnit,minText,sizeof(minText));
                            if (hasMaximum) EngineeringValue::format(maximumValue,
                                capability.quantity,capability.baseUnit,maxText,sizeof(maxText));

                            if (hasMinimum && hasMaximum)
                                snprintf(message,sizeof(message),
                                    "Configuration not saved. %s - %s: value '%s' must be between %s and %s.",
                                    currentSection.c_str(),capability.label,value.c_str(),minText,maxText);
                            else if (hasMinimum)
                                snprintf(message,sizeof(message),
                                    "Configuration not saved. %s - %s: value '%s' must be at least %s.",
                                    currentSection.c_str(),capability.label,value.c_str(),minText);
                            else
                                snprintf(message,sizeof(message),
                                    "Configuration not saved. %s - %s: value '%s' must not exceed %s.",
                                    currentSection.c_str(),capability.label,value.c_str(),maxText);

                            m_server.send(400,"text/plain",message);
                            return;
                        }
                    }
                    else if (strcmp(capability.type,"select")==0)
                    {
                        char options[sizeof(capability.options)];
                        copyText(options,sizeof(options),capability.options);
                        bool valid=false;
                        char* save=nullptr;
                        char* item=strtok_r(options,",",&save);
                        while (item)
                        {
                            trim(item);
                            char* separator=strchr(item,':');
                            if (separator)
                            {
                                *separator='\0';
                                char* optionValue=separator+1;
                                trim(optionValue);
                                if (value==optionValue) { valid=true; break; }
                            }
                            item=strtok_r(nullptr,",",&save);
                        }
                        if (!valid)
                        {
                            char message[256];
                            snprintf(message,sizeof(message),
                                "Configuration not saved. %s - %s: selected value '%s' is not defined by the profile.",
                                currentSection.c_str(),capability.label,value.c_str());
                            m_server.send(400,"text/plain",message);
                            return;
                        }
                    }
                }
            }
        }
        lineStart=lineEnd+1;
    }

    if (declaredPanelCount >= 0 &&
        static_cast<unsigned int>(declaredPanelCount) != seenPanels)
    {
        m_server.send(400, "text/plain",
                      "Configuration not saved: panel_count does not match the saved panels.");
        return;
    }

    if (declaredMacroCount >= 0 &&
        static_cast<unsigned int>(declaredMacroCount) != seenMacroPanels)
    {
        m_server.send(400, "text/plain", "macro_panel_count does not match macro sections.");
        return;
    }

    // Validation passed. Only now replace the saved configuration.
    SD.remove(TemporaryPath);
    File temporary=SD.open(TemporaryPath,FILE_WRITE);
    if (!temporary)
    {
        m_server.send(500,"text/plain","Could not create temporary configuration file.");
        return;
    }

    size_t written=temporary.print(configuration);
    temporary.close();
    if (written != configuration.length())
    {
        SD.remove(TemporaryPath);
        m_server.send(500,"text/plain","Could not write complete configuration file.");
        return;
    }

    SD.remove(BackupPath);
    bool replacing=SD.exists(ConfigPath);
    if (replacing && !SD.rename(ConfigPath,BackupPath))
    {
        SD.remove(TemporaryPath);
        m_server.send(500,"text/plain","Could not preserve existing configuration.");
        return;
    }

    if (!SD.rename(TemporaryPath,ConfigPath))
    {
        if (replacing) SD.rename(BackupPath,ConfigPath);
        SD.remove(TemporaryPath);
        m_server.send(500,"text/plain","Could not install saved configuration.");
        return;
    }

    SD.remove(BackupPath);
    m_server.send(200,"text/plain","Configuration saved successfully.");
}

void WebServerManager::handleSDCreateDirectory()
{
    if (!m_server.hasArg("parent") || !m_server.hasArg("dirname"))
    {
        m_server.send(400, "text/plain", "Invalid request.");
        return;
    }

    String parent = m_server.arg("parent");
    String dirname = m_server.arg("dirname");
    parent.trim();
    dirname.trim();

    if (parent.length() == 0)
        parent = "/";

    if (!parent.startsWith("/") ||
        parent.indexOf("..") >= 0 ||
        parent.indexOf('\\') >= 0 ||
        dirname.length() == 0 ||
        dirname.length() > 79 ||
        dirname.indexOf('/') >= 0 ||
        dirname.indexOf('\\') >= 0 ||
        dirname == "." ||
        dirname == ".." ||
        dirname.startsWith("."))
    {
        m_server.send(400, "text/plain", "Invalid SD-card directory name.");
        return;
    }

    if (parent != "/" && parent.endsWith("/"))
        parent.remove(parent.length() - 1);

    File parentDirectory = SD.open(parent.c_str());
    const bool validParent = parentDirectory && parentDirectory.isDirectory();
    if (parentDirectory)
        parentDirectory.close();

    if (!validParent)
    {
        m_server.send(400, "text/plain", "Selected parent directory does not exist.");
        return;
    }

    char path[192];
    const int pathLength = (parent == "/")
        ? snprintf(path, sizeof(path), "/%s", dirname.c_str())
        : snprintf(path, sizeof(path), "%s/%s", parent.c_str(), dirname.c_str());

    if (pathLength < 0 || static_cast<size_t>(pathLength) >= sizeof(path))
    {
        m_server.send(400, "text/plain", "Directory path is too long.");
        return;
    }

    if (SD.exists(path))
    {
        m_server.send(409, "text/plain", "Directory or file already exists.");
        return;
    }

    if (!SD.mkdir(path))
    {
        m_server.send(500, "text/plain", "Could not create SD-card directory.");
        return;
    }

    if (m_server.hasArg("ajax") && m_server.arg("ajax") == "1")
    {
        m_server.send(200, "text/plain", path);
        return;
    }

    char message[512];
    snprintf(
        message,
        sizeof(message),
        "<!DOCTYPE html><html><head>"
        "<meta charset=\"utf-8\">"
        "<meta http-equiv=\"refresh\" content=\"1;url=/\">"
        "<title>Directory Created</title>"
        "</head><body>"
        "<h2>Directory created</h2>"
        "<p>%s</p>"
        "<p>Returning to the Instrument Controller...</p>"
        "</body></html>",
        path);

    m_server.send(200, "text/html", message);
}

void WebServerManager::handleSDExists()
{
    if (!m_server.hasArg("folder") || !m_server.hasArg("filename"))
    {
        m_server.send(400, "text/plain", "Invalid request.");
        return;
    }

    String folder = m_server.arg("folder");
    String filename = m_server.arg("filename");
    folder.trim();
    filename.trim();

    if (folder.length() == 0)
        folder = "/";

    if (!folder.startsWith("/") ||
        folder.indexOf("..") >= 0 ||
        folder.indexOf('\\') >= 0 ||
        filename.length() == 0 ||
        filename.length() > 79 ||
        filename.indexOf('/') >= 0 ||
        filename.indexOf('\\') >= 0 ||
        filename == "." ||
        filename == "..")
    {
        m_server.send(400, "text/plain", "Invalid SD-card folder or filename.");
        return;
    }

    if (folder != "/" && folder.endsWith("/"))
        folder.remove(folder.length() - 1);

    char destination[192];
    const int pathLength = (folder == "/")
        ? snprintf(destination, sizeof(destination), "/%s", filename.c_str())
        : snprintf(destination, sizeof(destination), "%s/%s", folder.c_str(), filename.c_str());

    if (pathLength < 0 || static_cast<size_t>(pathLength) >= sizeof(destination))
    {
        m_server.send(400, "text/plain", "Invalid destination filename.");
        return;
    }

    m_server.send(200, "text/plain", SD.exists(destination) ? "1" : "0");
}

void WebServerManager::handleSDUploadData()
{
    static constexpr size_t MaximumUploadSize = 256 * 1024;
    static constexpr const char* TemporaryPath = "/.ak3y-upload.tmp";

    HTTPUpload& upload = m_server.upload();

    if (upload.status == UPLOAD_FILE_START)
    {
        m_sdUploadOK = false;
        m_sdUploadBytes = 0;
        m_sdUploadFailure = UploadFailure::None;

        if (m_sdUploadFile)
            m_sdUploadFile.close();

        SD.remove(TemporaryPath);
        m_sdUploadFile = SD.open(TemporaryPath, FILE_WRITE);

        if (m_sdUploadFile)
            m_sdUploadOK = true;
        else
            m_sdUploadFailure = UploadFailure::OpenFailed;

        return;
    }

    if (upload.status == UPLOAD_FILE_WRITE)
    {
        if (!m_sdUploadOK || !m_sdUploadFile)
            return;

        if (m_sdUploadBytes + upload.currentSize > MaximumUploadSize)
        {
            m_sdUploadOK = false;
            m_sdUploadFailure = UploadFailure::TooLarge;
            m_sdUploadFile.close();
            SD.remove(TemporaryPath);
            return;
        }

        const size_t written =
            m_sdUploadFile.write(upload.buf, upload.currentSize);

        if (written != upload.currentSize)
        {
            m_sdUploadOK = false;
            m_sdUploadFailure = UploadFailure::WriteFailed;
            m_sdUploadFile.close();
            SD.remove(TemporaryPath);
            return;
        }

        m_sdUploadBytes += written;
        return;
    }

    if (upload.status == UPLOAD_FILE_END)
    {
        if (m_sdUploadFile)
            m_sdUploadFile.close();

        if (m_sdUploadBytes == 0)
        {
            if (m_sdUploadFailure == UploadFailure::None)
                m_sdUploadFailure = UploadFailure::Empty;
            m_sdUploadOK = false;
            SD.remove(TemporaryPath);
        }

        return;
    }

    if (upload.status == UPLOAD_FILE_ABORTED)
    {
        m_sdUploadFailure = UploadFailure::Aborted;
        if (m_sdUploadFile)
            m_sdUploadFile.close();

        m_sdUploadOK = false;
        m_sdUploadBytes = 0;
        SD.remove(TemporaryPath);
    }
}

void WebServerManager::sendSDUploadError(int status, const char* reason)
{
    m_server.sendHeader("Cache-Control", "no-store");
    m_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    m_server.send(status, "text/html; charset=utf-8",
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Upload Failed</title>"
        "<style>body{font-family:Arial,sans-serif;margin:32px;color:#222}"
        "main{max-width:560px;margin:auto}p{line-height:1.5}"
        "a{display:inline-block;padding:12px 18px;background:#1769aa;"
        "color:white;text-decoration:none;border-radius:5px}</style>"
        "</head><body><main><h2>Upload Failed</h2><p>");
    sendEscaped(reason);
    sendChunk("</p><p><a href=\"/\">Return to Instrument Controller</a></p>"
              "</main></body></html>");
    m_server.sendContent("");
}

void WebServerManager::handleSDUpload()
{
    static constexpr const char* TemporaryPath = "/.ak3y-upload.tmp";
    static constexpr const char* BackupPath = "/.ak3y-upload.bak";

    if (!m_sdUploadOK || !m_server.hasArg("folder") ||
        !m_server.hasArg("filename"))
    {
        SD.remove(TemporaryPath);
        const char* reason = "The file could not be uploaded.";
        switch (m_sdUploadFailure)
        {
            case UploadFailure::TooLarge:
                reason = "Maximum upload size is 256 KB.";
                break;
            case UploadFailure::OpenFailed:
                reason = "Unable to create the temporary file on the SD card.";
                break;
            case UploadFailure::WriteFailed:
                reason = "Unable to write the complete file to the SD card.";
                break;
            case UploadFailure::Empty:
                reason = "The uploaded file was empty.";
                break;
            case UploadFailure::Aborted:
                reason = "The upload was interrupted.";
                break;
            default:
                break;
        }
        sendSDUploadError(400, reason);
        return;
    }

    String folder = m_server.arg("folder");
    String filename = m_server.arg("filename");
    folder.trim();
    filename.trim();

    if (folder.length() == 0)
        folder = "/";

    if (!folder.startsWith("/") ||
        folder.indexOf("..") >= 0 ||
        folder.indexOf('\\') >= 0 ||
        filename.length() == 0 ||
        filename.length() > 79 ||
        filename.indexOf('/') >= 0 ||
        filename.indexOf('\\') >= 0 ||
        filename == "." ||
        filename == "..")
    {
        SD.remove(TemporaryPath);
        m_sdUploadOK = false;
        sendSDUploadError(400, "Invalid SD-card folder or filename.");
        return;
    }

    if (folder != "/" && folder.endsWith("/"))
        folder.remove(folder.length() - 1);

    File directory = SD.open(folder.c_str());
    const bool validDirectory = directory && directory.isDirectory();
    if (directory)
        directory.close();

    if (!validDirectory)
    {
        SD.remove(TemporaryPath);
        m_sdUploadOK = false;
        sendSDUploadError(400, "Selected SD-card folder does not exist.");
        return;
    }

    char destination[192];
    int pathLength = 0;

    if (folder == "/")
    {
        pathLength = snprintf(destination, sizeof(destination), "/%s", filename.c_str());
        if (pathLength < 0 || static_cast<size_t>(pathLength) >= sizeof(destination))
        {
            SD.remove(TemporaryPath);
            m_sdUploadOK = false;
            sendSDUploadError(400, "Invalid destination filename.");
            return;
        }
    }
    else
    {
        pathLength = snprintf(destination, sizeof(destination), "%s/%s",
                              folder.c_str(), filename.c_str());
        if (pathLength < 0 || static_cast<size_t>(pathLength) >= sizeof(destination))
        {
            SD.remove(TemporaryPath);
            m_sdUploadOK = false;
            sendSDUploadError(400, "Invalid destination filename.");
            return;
        }
    }

    SD.remove(BackupPath);
    const bool replacing = SD.exists(destination);
    const bool replacementApproved =
        m_server.hasArg("replace") && m_server.arg("replace") == "1";

    if (replacing && !replacementApproved)
    {
        SD.remove(TemporaryPath);
        m_sdUploadOK = false;
        sendSDUploadError(409, "Upload cancelled: destination file already exists.");
        return;
    }

    if (replacing && !SD.rename(destination, BackupPath))
    {
        SD.remove(TemporaryPath);
        m_sdUploadOK = false;
        sendSDUploadError(500, "Could not preserve the existing SD-card file.");
        return;
    }

    if (!SD.rename(TemporaryPath, destination))
    {
        if (replacing)
            SD.rename(BackupPath, destination);

        SD.remove(TemporaryPath);
        m_sdUploadOK = false;
        sendSDUploadError(500, "Could not install the uploaded file.");
        return;
    }

    SD.remove(BackupPath);

    //
    // The SD-card contents may affect instruments, macros, or other
    // configuration displayed on the main page.  After a successful upload,
    // redirect immediately to "/" so the browser regenerates the page from
    // the newly updated SD card without requiring a manual refresh.
    //
    m_sdUploadOK = false;
    if (m_server.hasArg("ajax") && m_server.arg("ajax") == "1")
    {
        m_server.send(200, "text/plain", "Upload complete.");
        return;
    }
    m_server.sendHeader("Location", "/", true);
    m_server.send(303, "text/plain", "");
}

void WebServerManager::handleFunctionAsync()
{
    if (!m_server.hasArg("instrument") ||
        !m_server.hasArg("function"))
    {
        m_server.send(400, "text/plain", "Invalid request");
        return;
    }

    const String instrument=m_server.arg("instrument");
    const String function=m_server.arg("function");
    if (instrument.length()==0 || function.length()==0)
    {
        m_server.send(400, "text/plain", "Invalid request");
        return;
    }

    char semanticCommand[128];
    const int n=snprintf(semanticCommand,sizeof(semanticCommand),
                         "ENTER %s %s",instrument.c_str(),function.c_str());
    if (n<0 || static_cast<size_t>(n)>=sizeof(semanticCommand))
    {
        m_server.send(400, "text/plain", "Function command too long");
        return;
    }

    char response[2];
    size_t responseLength=0;
    const CommandExecutor::Result result=m_commandExecutor.execute(
        semanticCommand,response,sizeof(response),responseLength,1000);

    if (result!=CommandExecutor::Result::Success)
    {
        m_server.send(500, "text/plain", commandResultMessage(CommandExecutor::resultText(result)).c_str());
        return;
    }

    m_server.send(200, "text/plain", function);
}

void WebServerManager::handleNotFound()
{
    m_server.send(
        404,
        "text/plain",
        "Not Found");
}

}   // namespace AK3Y
