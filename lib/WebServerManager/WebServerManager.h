/******************************************************************************
 * File: WebServerManager.h
 * Project: AK3Y Instrument Controller
 *
 * Purpose:
 *   Declares the HTTP server subsystem for browser-based instrument control.
 *
 * Responsibilities:
 *   Defines web request handlers, instrument and capability descriptors,
 *   rendering helpers, SD-card operations, and macro execution state.
 *
 * Dependencies:
 *   Subsystem, CommandExecutor, WebServer, filesystem interfaces, and the
 *   collaborating configuration, macro, Wi-Fi, activity, and console services.
 ******************************************************************************/

#pragma once

#include <Subsystem.h>
#include <CommandExecutor.h>
#include <WebServer.h>
#include <FS.h>
#include <MacroConfiguration.h>

namespace AK3Y
{

class ActivityMonitor;
class CommandExecutor;
class ConfigurationFile;
class Console;
class MacroConfiguration;
class MacroExecutor;
class WiFiManager;

class WebServerManager : public Subsystem
{
public:
    WebServerManager(
        WiFiManager& wifiManager,
        ConfigurationFile& configurationFile,
        CommandExecutor& commandExecutor,
        MacroConfiguration& macroConfiguration,
        MacroExecutor& macroExecutor,
        ActivityMonitor& activityMonitor,
        Console& console);

    bool initialize() override;
    void update();

private:
    struct InstrumentDefinition
    {
        char section[32];
        char name[80];
        char profileName[96];
        char manufacturer[80];
        char model[80];
        char defaultFunctionLabel[80];
        char interfaceType[16];
        char address[16];
        char port[16];
        char baud[16];
        char dataBits[8];
        char parity[16];
        char stopBits[8];
        char host[96];
    };

    struct CapabilityDefinition
    {
        char section[32];
        char type[24];
        char label[80];
        char quantity[32];
        char baseUnit[16];
        char minimum[32];
        char maximum[32];
        char setTemplate[128];
        char queryCommand[128];
        char responsePrefix[96];
        char responseTerminator[16];
        char responseUnit[16];
        char options[192];
        char actionCommand[128];
        char actionOnCommand[128];
        char labelOn[80];
        char resetsToggle[32];
        char visibleWhen[80];
        char ui[8];

        bool canSet() const { return setTemplate[0] != '\0'; }
        bool canQuery() const { return queryCommand[0] != '\0'; }
        bool canExecute() const { return actionCommand[0] != '\0'; }
        bool canExecuteOn() const { return actionOnCommand[0] != '\0'; }
    };

    static void runtimeDiagnosticCallback(
        bool recovered,
        const CommandExecutor::TransportDiagnostic& diagnostic,
        const char* errorText,
        void* context);

    void handleRoot();
    void handleCommand();
    void handleCommandAsync();
    void handleCapability();
    void handleCapabilityAsync();
    void handleFunctionAsync();
    void handleControlBoxConfig();
    void handleSaveControlBoxConfig();
    void handleSendControlBox();
    void handleSDCreateDirectory();
    void handleSDView();
    void handleSDDelete();
    void handleSDExists();
    void handleSDUpload();
    void handleSDUploadData();
    void sendSDUploadError(int status, const char* reason);
    void handleMacroStart();
    void handleMacroAbort();
    void handleMacroStatus();
    void executePendingMacro();

    static void macroProgressCallback(
        size_t completedSteps,
        size_t totalSteps,
        size_t currentStep,
        void* context);

    static bool macroCancellationCallback(void* context);

    void updateMacroProgress(
        size_t completedSteps,
        size_t totalSteps,
        size_t currentStep);
    void handleNotFound();

    void sendPage(
        const char* command,
        const char* resultText,
        const char* response,
        const char* cardInstrument = "",
        const char* cardCapability = "",
        const char* cardValue = "",
        const char* cardResult = "");

    String commandResultMessage(const char* generalResult);
    void flushHtmlBuffer();
    void appendHtmlBuffer(const char* data, size_t length);
    void sendChunk(const String& text);
    void sendChunk(const char* text);
    void sendEscaped(const char* text);

    void renderMacros();

    void renderInstruments(
        const char* cardInstrument,
        const char* cardCapability,
        const char* cardValue,
        const char* cardResult);

    void renderInstrument(
        const InstrumentDefinition& instrument,
        const char* cardInstrument,
        const char* cardCapability,
        const char* cardValue,
        const char* cardResult);

    void renderCapabilities(
        const InstrumentDefinition& instrument,
        const char* cardInstrument,
        const char* cardCapability,
        const char* cardValue,
        const char* cardResult);

    void renderCapability(
        const InstrumentDefinition& instrument,
        const CapabilityDefinition& capability,
        const char* functionSection,
        const char* cardInstrument,
        const char* cardCapability,
        const char* cardValue,
        const char* cardResult);

    void renderFunctionSelector(
        const InstrumentDefinition& instrument,
        const char* profilePath);

    bool loadInstrument(
        const char* section,
        InstrumentDefinition& instrument);

    bool loadCapability(
        const InstrumentDefinition& instrument,
        const char* section,
        CapabilityDefinition& capability);

    bool extractSemanticValue(
        const char* response,
        const CapabilityDefinition& capability,
        double& value);

    WiFiManager& m_wifiManager;
    ConfigurationFile& m_configurationFile;
    CommandExecutor& m_commandExecutor;
    MacroConfiguration& m_macroConfiguration;
    MacroExecutor& m_macroExecutor;
    ActivityMonitor& m_activityMonitor;
    Console& m_console;

    static constexpr size_t WebMacroParameterLimit = 16;

    bool m_macroPending = false;
    bool m_macroRunning = false;
    bool m_macroFinished = false;
    bool m_macroSucceeded = false;
    bool m_macroAborted = false;
    bool m_macroAbortRequested = false;
    bool m_macroChangesSD = false;
    size_t m_macroOverrideCount = 0;
    size_t m_macroCompletedSteps = 0;
    size_t m_macroTotalSteps = 0;
    size_t m_macroCurrentStep = 0;
    char m_macroFile[128] = "";
    char m_macroName[80] = "";
    char m_macroResult[128] = "";
    struct MacroResultValue { char name[32]; char value[64]; bool available; };
    MacroResultValue m_macroResultValues[MacroConfiguration::MaxResults] = {};
    size_t m_macroResultCount = 0;
    static void macroResultCallback(const char* name, const char* value, void* context);
    void updateMacroResult(const char* name, const char* value);
    char m_macroOverrideNames[WebMacroParameterLimit][32] = {};
    char m_macroOverrideValues[WebMacroParameterLimit][64] = {};
    File m_sdUploadFile;
    bool m_sdUploadOK = false;
    enum class UploadFailure { None, OpenFailed, TooLarge, WriteFailed, Empty, Aborted };
    UploadFailure m_sdUploadFailure = UploadFailure::None;
    size_t m_sdUploadBytes = 0;
    bool m_otaOK = false;
    bool m_otaFinalized = false;
    bool m_otaRestartPending = false;
    uint32_t m_otaRestartAt = 0;
    size_t m_otaBytes = 0;
    size_t m_otaExpected = 0;
    String m_otaError;
    bool m_timeSyncLogged = false;
    bool m_wifiSetupPending = false;
    uint32_t m_wifiSetupAt = 0;
    static constexpr size_t HtmlBufferCapacity = 2048;
    char m_htmlBuffer[HtmlBufferCapacity];
    size_t m_htmlBufferUsed = 0;
    bool m_htmlBufferActive = false;
    WebServer m_server;
};

}   // namespace AK3Y
