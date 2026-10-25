/******************************************************************************
 * File: CommandExecutor.h
 * Project: AK3Y Instrument Controller
 * Purpose: Instrument command interface
 * Description: Declares command execution, validation, result codes, and transport-error reporting.
 * Note: This file header describes responsibilities; release history is in Git.
 ******************************************************************************/

#pragma once
#include <stddef.h>
#include <stdint.h>
#include <Instrument.h>
#include <InstrumentConfiguration.h>

namespace AK3Y {
class ConfigurationFile;
class IEEE488;
class RS232;
class TCPInstrument;
class SDCard;

class CommandExecutor {
public:
    enum class Result : uint8_t {
        Success=0, InvalidArgument, InvalidSyntax, ConfigurationError,
        UnsupportedInterface, InvalidAddress, ProfileError, MissingTemplate,
        MissingValueToken, CommandTooLong, ResponseBufferTooSmall, TransferError,
        SynchronizationError, InvalidSynchronizationTimeout, ResponseParseError
    };

    CommandExecutor(ConfigurationFile& configurationFile, SDCard& sdCard,
                    IEEE488& ieee488, RS232& rs232, TCPInstrument& tcpInstrument);

    Result execute(const char* command, char* response, size_t responseSize,
                   size_t& responseLength, uint32_t timeoutMilliseconds=1000);

    Result validate(const char* command);

    static const char* resultText(Result result);

    // Details of the most recent transport failure in execute().
    // Cleared at the beginning of each execute() call.
    enum class Transport : uint8_t { None, GPIB, RS232, TCP };
    struct TransportDiagnostic {
        Transport transport;
        int resultCode;
        const char* instrumentName;
        uint8_t gpibAddress;
        uint8_t serialPort;
        uint16_t tcpPort;
    };
    const TransportDiagnostic& lastTransportDiagnostic() const;
    const char* lastTransportErrorText() const;

    // Optional runtime diagnostics sink; no dependency on Console.
    // Called synchronously at the end of execute().
    using DiagnosticCallback = void (*)(bool recovered,
                                        const TransportDiagnostic& diagnostic,
                                        const char* errorText,
                                        void* context);
    void setDiagnosticCallback(DiagnosticCallback callback, void* context);



private:
    TransportDiagnostic m_lastTransportDiagnostic = {Transport::None, 0, nullptr, 0, 0, 0};
    char m_lastInstrumentName[64] = {};
    DiagnosticCallback m_diagnosticCallback = nullptr;
    void* m_diagnosticContext = nullptr;
    bool m_pendingRecovery = false;
    char m_failedInstrumentName[64] = {};
    TransportDiagnostic m_failedDiagnostic = {Transport::None, 0, nullptr, 0, 0, 0};
    void reportExecutionDiagnostic(Result result);
    Result executeCore(const char* command, char* response, size_t responseSize,
                       size_t& responseLength, uint32_t timeoutMilliseconds);

    void recordTransportFailure(const InstrumentConfiguration::Definition& definition,
                                Transport transport, int resultCode);
    ConfigurationFile& m_configurationFile;
    InstrumentConfiguration m_instrumentConfiguration;
    Instrument m_gpibInstrument;
    RS232& m_rs232;
    TCPInstrument& m_tcpInstrument;

    Result configureInstrument(const char* instrumentName,
                               InstrumentConfiguration::Definition& definition,
                               char* profilePath, size_t profilePathSize);

    Result writeInstrument(const InstrumentConfiguration::Definition& definition,
                           const char* message, uint32_t timeoutMilliseconds);

    Result queryInstrument(const InstrumentConfiguration::Definition& definition,
                           const char* message, char* response,
                           size_t responseSize, size_t& responseLength,
                           uint32_t timeoutMilliseconds);

    Result loadTransportFraming(
        const InstrumentConfiguration::Definition& definition,
        const char* commandKey, const char* responseKey,
        uint8_t* commandTerminator, size_t& commandTerminatorLength,
        uint8_t* responseTerminator, size_t& responseTerminatorLength);

    Result loadSerialFraming(
        const InstrumentConfiguration::Definition& definition,
        uint8_t* commandTerminator, size_t& commandTerminatorLength,
        uint8_t* responseTerminator, size_t& responseTerminatorLength);

    Result loadTCPFraming(
        const InstrumentConfiguration::Definition& definition,
        uint8_t* commandTerminator, size_t& commandTerminatorLength,
        uint8_t* responseTerminator, size_t& responseTerminatorLength);

    static bool decodeTerminator(
        const char* text, uint8_t* bytes, size_t capacity, size_t& length);

    Result executeSet(const char* instrumentName, const char* capability,
                      const char* value, uint32_t timeoutMilliseconds);
    Result executeQuery(const char* instrumentName, const char* capability,
                        char* response, size_t responseSize,
                        size_t& responseLength, uint32_t timeoutMilliseconds);
    Result executeAction(const char* instrumentName, const char* capability,
                         uint32_t timeoutMilliseconds);
    Result executeActionOn(const char* instrumentName, const char* capability,
                           uint32_t timeoutMilliseconds);
    Result executeFunctionEntry(const char* instrumentName,
                                const char* functionSection,
                                uint32_t timeoutMilliseconds);
    Result executeWait(const char* instrumentName,
                       const char* condition);
};
}
