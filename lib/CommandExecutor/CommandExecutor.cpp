/******************************************************************************
 * File: CommandExecutor.cpp
 * Project: AK3Y Instrument Controller
 * Purpose: Instrument command dispatch
 * Description: Implements profile-driven command validation and execution across GPIB, RS-232, and TCP transports.
 * Note: This file header describes responsibilities; release history is in Git.
 ******************************************************************************/

#include "CommandExecutor.h"
#include <ConfigurationFile.h>
#include <IEEE488.h>
#include <RS232.h>
#include <TCPInstrument.h>
#include <SDCard.h>
#include <stdio.h>
#include <string.h>
#include <Arduino.h>

namespace AK3Y {
namespace {
constexpr const char* ValueToken="{value}";
}

CommandExecutor::CommandExecutor(
    ConfigurationFile& configurationFile,
    SDCard& sdCard,
    IEEE488& ieee488,
    RS232& rs232,
    TCPInstrument& tcpInstrument)
    : m_configurationFile(configurationFile),
      m_instrumentConfiguration(sdCard,configurationFile),
      m_gpibInstrument(ieee488,0),
      m_rs232(rs232),
      m_tcpInstrument(tcpInstrument)
{
}

CommandExecutor::Result CommandExecutor::configureInstrument(
    const char* name,
    InstrumentConfiguration::Definition& definition,
    char* profilePath,
    size_t profilePathSize)
{
    // Preserve the instrument identity for any subsequent transport failure.
    snprintf(m_lastInstrumentName, sizeof(m_lastInstrumentName), "%s", name ? name : "");

    if (!name || !profilePath || profilePathSize==0)
        return Result::InvalidArgument;

    if (!m_instrumentConfiguration.load(name,definition))
        return Result::ConfigurationError;

    if (definition.interfaceType==InstrumentConfiguration::Interface::GPIB)
    {
        if (!m_gpibInstrument.setAddress(definition.address))
            return Result::InvalidAddress;
    }
    else if (definition.interfaceType!=InstrumentConfiguration::Interface::RS232 &&
             definition.interfaceType!=InstrumentConfiguration::Interface::TCP)
    {
        return Result::UnsupportedInterface;
    }

    int n=snprintf(
        profilePath,profilePathSize,"/instruments/%s",definition.profile);

    if (n<0 || static_cast<size_t>(n)>=profilePathSize)
        return Result::ProfileError;

    return Result::Success;
}

bool CommandExecutor::decodeTerminator(
    const char* text,
    uint8_t* bytes,
    size_t capacity,
    size_t& length)
{
    length=0;

    if (!text || !bytes)
        return false;

    if (strcmp(text,"NONE")==0)
        return true;

    if (strcmp(text,"CR")==0)
    {
        if (capacity<1) return false;
        bytes[0]='\r';
        length=1;
        return true;
    }

    if (strcmp(text,"LF")==0)
    {
        if (capacity<1) return false;
        bytes[0]='\n';
        length=1;
        return true;
    }

    if (strcmp(text,"CRLF")==0)
    {
        if (capacity<2) return false;
        bytes[0]='\r';
        bytes[1]='\n';
        length=2;
        return true;
    }

    return false;
}

CommandExecutor::Result CommandExecutor::loadTransportFraming(
    const InstrumentConfiguration::Definition& definition,
    const char* commandKey,
    const char* responseKey,
    uint8_t* commandTerminator,
    size_t& commandTerminatorLength,
    uint8_t* responseTerminator,
    size_t& responseTerminatorLength)
{
    commandTerminatorLength=0;
    responseTerminatorLength=0;

    char profilePath[128];
    int n=snprintf(
        profilePath,sizeof(profilePath),
        "/instruments/%s",definition.profile);

    if (n<0 || static_cast<size_t>(n)>=sizeof(profilePath))
        return Result::ProfileError;

    char commandText[16];
    char responseText[16];

    if (!m_configurationFile.getValue(
            profilePath,"instrument",commandKey,
            commandText,sizeof(commandText)) ||
        !m_configurationFile.getValue(
            profilePath,"instrument",responseKey,
            responseText,sizeof(responseText)))
        return Result::ConfigurationError;

    if (!decodeTerminator(
            commandText,commandTerminator,2,commandTerminatorLength) ||
        !decodeTerminator(
            responseText,responseTerminator,2,responseTerminatorLength))
        return Result::ConfigurationError;

    // queryUntil() requires a real response terminator.  A command may use
    // NONE, but a text query must have a bounded response in this milestone.
    if (responseTerminatorLength==0)
        return Result::ConfigurationError;

    return Result::Success;
}

CommandExecutor::Result CommandExecutor::loadSerialFraming(
    const InstrumentConfiguration::Definition& definition,
    uint8_t* commandTerminator,
    size_t& commandTerminatorLength,
    uint8_t* responseTerminator,
    size_t& responseTerminatorLength)
{
    return loadTransportFraming(
        definition,
        "serial_command_terminator",
        "serial_response_terminator",
        commandTerminator,commandTerminatorLength,
        responseTerminator,responseTerminatorLength);
}

CommandExecutor::Result CommandExecutor::loadTCPFraming(
    const InstrumentConfiguration::Definition& definition,
    uint8_t* commandTerminator,
    size_t& commandTerminatorLength,
    uint8_t* responseTerminator,
    size_t& responseTerminatorLength)
{
    return loadTransportFraming(
        definition,
        "tcp_command_terminator",
        "tcp_response_terminator",
        commandTerminator,commandTerminatorLength,
        responseTerminator,responseTerminatorLength);
}

CommandExecutor::Result CommandExecutor::writeInstrument(
    const InstrumentConfiguration::Definition& definition,
    const char* message,
    uint32_t timeout)
{
    (void)timeout;

    if (!message)
        return Result::InvalidArgument;

    if (definition.interfaceType==InstrumentConfiguration::Interface::GPIB)
    {
        const IEEE488::TransferResult tr = m_gpibInstrument.write(message,timeout);
        if (tr != IEEE488::TransferResult::Success)
            recordTransportFailure(definition, Transport::GPIB, static_cast<int>(tr));
        return tr == IEEE488::TransferResult::Success ? Result::Success : Result::TransferError;
    }

    if (definition.interfaceType==InstrumentConfiguration::Interface::TCP)
    {
        uint8_t commandTerminator[2];
        uint8_t responseTerminator[2];
        size_t commandTerminatorLength=0;
        size_t responseTerminatorLength=0;

        Result framingResult=loadTCPFraming(
            definition,
            commandTerminator,commandTerminatorLength,
            responseTerminator,responseTerminatorLength);

        if (framingResult!=Result::Success)
            return framingResult;

        const size_t messageLength=strlen(message);
        if (messageLength+commandTerminatorLength>160)
            return Result::CommandTooLong;

        uint8_t framedMessage[160];
        memcpy(framedMessage,message,messageLength);
        if (commandTerminatorLength)
            memcpy(framedMessage+messageLength,commandTerminator,commandTerminatorLength);

        TCPInstrument::TransferResult tr=m_tcpInstrument.write(
            definition.host,definition.tcpPort,framedMessage,
            messageLength+commandTerminatorLength,timeout);
        if (tr != TCPInstrument::TransferResult::Success)
            recordTransportFailure(definition, Transport::TCP, static_cast<int>(tr));

        return tr==TCPInstrument::TransferResult::Success
            ? Result::Success : Result::TransferError;
    }

    if (definition.interfaceType!=InstrumentConfiguration::Interface::RS232)
        return Result::UnsupportedInterface;

    uint8_t commandTerminator[2];
    uint8_t responseTerminator[2];
    size_t commandTerminatorLength=0;
    size_t responseTerminatorLength=0;

    Result framingResult=loadSerialFraming(
        definition,
        commandTerminator,commandTerminatorLength,
        responseTerminator,responseTerminatorLength);

    if (framingResult!=Result::Success)
        return framingResult;

    const size_t messageLength=strlen(message);

    if (messageLength+commandTerminatorLength>160)
        return Result::CommandTooLong;

    uint8_t framedMessage[160];
    memcpy(framedMessage,message,messageLength);

    if (commandTerminatorLength)
        memcpy(
            framedMessage+messageLength,
            commandTerminator,
            commandTerminatorLength);

    RS232::TransferResult tr=m_rs232.write(
        definition.port,
        framedMessage,
        messageLength+commandTerminatorLength);

    if (tr != RS232::TransferResult::Success)
        recordTransportFailure(definition, Transport::RS232, static_cast<int>(tr));

    return tr==RS232::TransferResult::Success
        ? Result::Success : Result::TransferError;
}

CommandExecutor::Result CommandExecutor::queryInstrument(
    const InstrumentConfiguration::Definition& definition,
    const char* message,
    char* response,
    size_t responseSize,
    size_t& responseLength,
    uint32_t timeout)
{
    responseLength=0;

    if (!message || !response || responseSize==0)
        return Result::InvalidArgument;

    if (definition.interfaceType==InstrumentConfiguration::Interface::GPIB)
    {
        IEEE488::TransferResult tr=m_gpibInstrument.query(
            message,response,responseSize,responseLength,timeout);

        if (tr != IEEE488::TransferResult::Success)
            recordTransportFailure(definition, Transport::GPIB, static_cast<int>(tr));

        if (tr==IEEE488::TransferResult::BufferTooSmall)
            return Result::ResponseBufferTooSmall;

        return tr==IEEE488::TransferResult::Success
            ? Result::Success : Result::TransferError;
    }

    if (definition.interfaceType==InstrumentConfiguration::Interface::TCP)
    {
        uint8_t commandTerminator[2];
        uint8_t responseTerminator[2];
        size_t commandTerminatorLength=0;
        size_t responseTerminatorLength=0;

        Result framingResult=loadTCPFraming(
            definition,
            commandTerminator,commandTerminatorLength,
            responseTerminator,responseTerminatorLength);

        if (framingResult!=Result::Success)
            return framingResult;

        const size_t messageLength=strlen(message);
        if (messageLength+commandTerminatorLength>160)
            return Result::CommandTooLong;
        if (responseSize<2)
            return Result::ResponseBufferTooSmall;

        uint8_t framedMessage[160];
        memcpy(framedMessage,message,messageLength);
        if (commandTerminatorLength)
            memcpy(framedMessage+messageLength,commandTerminator,commandTerminatorLength);

        TCPInstrument::TransferResult tr=m_tcpInstrument.queryUntil(
            definition.host,definition.tcpPort,
            framedMessage,messageLength+commandTerminatorLength,
            reinterpret_cast<uint8_t*>(response),responseSize-1,
            responseTerminator,responseTerminatorLength,
            responseLength,timeout);
        if (tr != TCPInstrument::TransferResult::Success)
            recordTransportFailure(definition, Transport::TCP, static_cast<int>(tr));

        if (tr==TCPInstrument::TransferResult::BufferTooSmall)
            return Result::ResponseBufferTooSmall;
        if (tr!=TCPInstrument::TransferResult::Success)
            return Result::TransferError;

        response[responseLength]='\0';
        return Result::Success;
    }

    if (definition.interfaceType!=InstrumentConfiguration::Interface::RS232)
        return Result::UnsupportedInterface;

    uint8_t commandTerminator[2];
    uint8_t responseTerminator[2];
    size_t commandTerminatorLength=0;
    size_t responseTerminatorLength=0;

    Result framingResult=loadSerialFraming(
        definition,
        commandTerminator,commandTerminatorLength,
        responseTerminator,responseTerminatorLength);

    if (framingResult!=Result::Success)
        return framingResult;

    const size_t messageLength=strlen(message);

    if (messageLength+commandTerminatorLength>160)
        return Result::CommandTooLong;

    uint8_t framedMessage[160];
    memcpy(framedMessage,message,messageLength);

    if (commandTerminatorLength)
        memcpy(
            framedMessage+messageLength,
            commandTerminator,
            commandTerminatorLength);

    // Reserve one byte for a C-string terminator because all existing
    // CommandExecutor consumers treat the returned response as text.
    if (responseSize<2)
        return Result::ResponseBufferTooSmall;

    RS232::TransferResult tr=m_rs232.queryUntil(
        definition.port,
        framedMessage,
        messageLength+commandTerminatorLength,
        reinterpret_cast<uint8_t*>(response),
        responseSize-1,
        responseTerminator,
        responseTerminatorLength,
        responseLength,
        timeout);

    if (tr != RS232::TransferResult::Success)
        recordTransportFailure(definition, Transport::RS232, static_cast<int>(tr));

    if (tr==RS232::TransferResult::BufferTooSmall)
        return Result::ResponseBufferTooSmall;

    if (tr!=RS232::TransferResult::Success)
        return Result::TransferError;

    response[responseLength]='\0';
    return Result::Success;
}

CommandExecutor::Result CommandExecutor::executeSet(
    const char* name,
    const char* capability,
    const char* value,
    uint32_t timeout)
{
    char path[128],templ[96];
    InstrumentConfiguration::Definition definition;

    Result r=configureInstrument(name,definition,path,sizeof(path));
    if (r!=Result::Success) return r;

    if (!m_configurationFile.getValue(
            path,capability,"set",templ,sizeof(templ)))
        return Result::MissingTemplate;

    const char* token=strstr(templ,ValueToken);
    if (!token) return Result::MissingValueToken;

    size_t prefix=static_cast<size_t>(token-templ);
    const char* suffix=token+strlen(ValueToken);

    char command[128];
    int n=snprintf(
        command,sizeof(command),"%.*s%s%s",
        static_cast<int>(prefix),templ,value,suffix);

    if (n<0 || static_cast<size_t>(n)>=sizeof(command))
        return Result::CommandTooLong;

    return writeInstrument(definition,command,timeout);
}

CommandExecutor::Result CommandExecutor::executeQuery(
    const char* name,
    const char* capability,
    char* response,
    size_t responseSize,
    size_t& responseLength,
    uint32_t timeout)
{
    char path[128],query[96];
    InstrumentConfiguration::Definition definition;

    Result r=configureInstrument(name,definition,path,sizeof(path));
    if (r!=Result::Success) return r;

    if (!m_configurationFile.getValue(
            path,capability,"query",query,sizeof(query)))
        return Result::MissingTemplate;

    r=queryInstrument(
        definition,query,response,responseSize,responseLength,timeout);

    if (r!=Result::Success)
        return r;

    // Optional profile-driven extraction of the semantic
    // value from a compound instrument response.  Profiles that define
    // neither key retain the existing raw-response behavior.
    char prefix[96];
    char terminator[32];

    const bool hasPrefix=m_configurationFile.getValue(
        path,capability,"response_prefix",prefix,sizeof(prefix));

    const bool hasTerminator=m_configurationFile.getValue(
        path,capability,"response_terminator",terminator,sizeof(terminator));

    if (!hasPrefix && !hasTerminator)
        return Result::Success;

    const char* first=response;

    if (hasPrefix && prefix[0]!='\0')
    {
        first=strstr(response,prefix);
        if (!first)
            return Result::ResponseParseError;

        first+=strlen(prefix);
    }

    const char* last=response+strlen(response);

    if (hasTerminator && terminator[0]!='\0')
    {
        const char* found=strstr(first,terminator);
        if (!found)
            return Result::ResponseParseError;

        last=found;
    }

    // Strip surrounding whitespace but otherwise preserve the instrument's
    // representation of the value.
    while (first<last &&
           (*first==' ' || *first=='\t' ||
            *first=='\r' || *first=='\n'))
        ++first;

    while (last>first &&
           (last[-1]==' ' || last[-1]=='\t' ||
            last[-1]=='\r' || last[-1]=='\n'))
        --last;

    const size_t valueLength=static_cast<size_t>(last-first);

    if (valueLength+1>responseSize)
        return Result::ResponseBufferTooSmall;

    memmove(response,first,valueLength);
    response[valueLength]='\0';
    responseLength=valueLength;

    return Result::Success;
}

CommandExecutor::Result CommandExecutor::executeAction(
    const char* name,
    const char* capability,
    uint32_t timeout)
{
    char path[128],command[128];
    InstrumentConfiguration::Definition definition;

    Result r=configureInstrument(name,definition,path,sizeof(path));
    if (r!=Result::Success) return r;

    if (!m_configurationFile.getValue(
            path,capability,"action",command,sizeof(command)))
        return Result::MissingTemplate;

    return writeInstrument(definition,command,timeout);
}

CommandExecutor::Result CommandExecutor::executeActionOn(
    const char* name,
    const char* capability,
    uint32_t timeout)
{
    char path[128],command[128];
    InstrumentConfiguration::Definition definition;

    Result r=configureInstrument(name,definition,path,sizeof(path));
    if (r!=Result::Success) return r;

    if (!m_configurationFile.getValue(
            path,capability,"action_on",command,sizeof(command)))
        return Result::MissingTemplate;

    return writeInstrument(definition,command,timeout);
}

CommandExecutor::Result CommandExecutor::executeFunctionEntry(
    const char* name,
    const char* functionSection,
    uint32_t timeout)
{
    char path[128],command[128];
    InstrumentConfiguration::Definition definition;

    Result r=configureInstrument(name,definition,path,sizeof(path));
    if (r!=Result::Success) return r;

    const char* section=functionSection;
    const char* key="enter";

    if (strcmp(functionSection,"__basic__")==0)
    {
        section="instrument";
        key="default_enter";
    }

    if (!m_configurationFile.getValue(
            path,section,key,command,sizeof(command)))
        return Result::Success;

    if (command[0]=='\0')
        return Result::Success;

    return writeInstrument(definition,command,timeout);
}


CommandExecutor::Result CommandExecutor::executeWait(
    const char* name,
    const char* condition)
{
    if (!name || !condition)
        return Result::InvalidArgument;

    if (strcmp(condition,"ready")!=0)
        return Result::InvalidSyntax;

    char path[128];
    InstrumentConfiguration::Definition definition;

    Result r=configureInstrument(name,definition,path,sizeof(path));
    if (r!=Result::Success) return r;

    char query[96];
    char expected[32];
    char timeoutText[16];

    if (!m_configurationFile.getValue(
            path,"instrument","ready_query",query,sizeof(query)) ||
        !m_configurationFile.getValue(
            path,"instrument","ready_response",expected,sizeof(expected)) ||
        !m_configurationFile.getValue(
            path,"instrument","ready_timeout_ms",timeoutText,sizeof(timeoutText)))
        return Result::ConfigurationError;

    char* end=nullptr;
    const unsigned long parsedTimeout=strtoul(timeoutText,&end,10);

    while (end && (*end==' ' || *end=='\t'))
        ++end;

    if (end==timeoutText || !end || *end!='\0' ||
        parsedTimeout==0UL || parsedTimeout>120000UL)
        return Result::InvalidSynchronizationTimeout;

    char response[64];
    size_t responseLength=0;

    r=queryInstrument(
        definition,query,response,sizeof(response),responseLength,
        static_cast<uint32_t>(parsedTimeout));

    if (r!=Result::Success)
        return r;

    char* first=response;
    while (*first==' ' || *first=='\r' || *first=='\n' || *first=='\t')
        ++first;

    char* last=first+strlen(first);
    while (last>first &&
           (last[-1]==' ' || last[-1]=='\r' ||
            last[-1]=='\n' || last[-1]=='\t'))
        --last;
    *last='\0';

    if (strcmp(first,expected)!=0)
        return Result::SynchronizationError;

    return Result::Success;
}

CommandExecutor::Result CommandExecutor::validate(const char* command)
{
    if (!command)
        return Result::InvalidArgument;

    char copy[160];
    const size_t len=strlen(command);
    if (len>=sizeof(copy))
        return Result::CommandTooLong;

    memcpy(copy,command,len+1);

    char* save=nullptr;
    char* verb=strtok_r(copy," \t",&save);
    char* name=strtok_r(nullptr," \t",&save);
    char* capability=strtok_r(nullptr," \t",&save);

    if (!verb || !name || !capability)
        return Result::InvalidSyntax;

    char path[128];
    InstrumentConfiguration::Definition definition;
    Result r=configureInstrument(name,definition,path,sizeof(path));
    if (r!=Result::Success)
        return r;

    auto validateFraming = [&]() -> Result
    {
        if (definition.interfaceType==InstrumentConfiguration::Interface::GPIB)
            return Result::Success;

        uint8_t commandTerminator[2];
        uint8_t responseTerminator[2];
        size_t commandTerminatorLength=0;
        size_t responseTerminatorLength=0;

        if (definition.interfaceType==InstrumentConfiguration::Interface::RS232)
            return loadSerialFraming(
                definition,
                commandTerminator,commandTerminatorLength,
                responseTerminator,responseTerminatorLength);

        if (definition.interfaceType==InstrumentConfiguration::Interface::TCP)
            return loadTCPFraming(
                definition,
                commandTerminator,commandTerminatorLength,
                responseTerminator,responseTerminatorLength);

        return Result::UnsupportedInterface;
    };

    if (strcmp(verb,"SET")==0)
    {
        char* value=strtok_r(nullptr," \t",&save);
        char* extra=strtok_r(nullptr," \t",&save);
        if (!value || extra)
            return Result::InvalidSyntax;

        char templ[96];
        if (!m_configurationFile.getValue(path,capability,"set",templ,sizeof(templ)) ||
            templ[0]=='\0')
            return Result::MissingTemplate;

        const char* token=strstr(templ,ValueToken);
        if (!token)
            return Result::MissingValueToken;

        const size_t prefix=static_cast<size_t>(token-templ);
        const char* suffix=token+strlen(ValueToken);
        char rendered[128];
        const int n=snprintf(rendered,sizeof(rendered),"%.*s%s%s",
            static_cast<int>(prefix),templ,value,suffix);
        if (n<0 || static_cast<size_t>(n)>=sizeof(rendered))
            return Result::CommandTooLong;

        return validateFraming();
    }

    if (strcmp(verb,"QUERY")==0)
    {
        if (strtok_r(nullptr," \t",&save))
            return Result::InvalidSyntax;

        char query[96];
        if (!m_configurationFile.getValue(path,capability,"query",query,sizeof(query)) ||
            query[0]=='\0')
            return Result::MissingTemplate;

        return validateFraming();
    }

    if (strcmp(verb,"ACTION")==0 || strcmp(verb,"ACTIONON")==0)
    {
        if (strtok_r(nullptr," \t",&save))
            return Result::InvalidSyntax;

        const char* key=(strcmp(verb,"ACTION")==0) ? "action" : "action_on";
        char action[128];
        if (!m_configurationFile.getValue(path,capability,key,action,sizeof(action)) ||
            action[0]=='\0')
            return Result::MissingTemplate;

        return validateFraming();
    }

    if (strcmp(verb,"ENTER")==0)
    {
        if (strtok_r(nullptr," \t",&save))
            return Result::InvalidSyntax;

        // ENTER deliberately permits a missing/empty entry command.
        // executeFunctionEntry() treats that as a successful no-op.
        return validateFraming();
    }

    if (strcmp(verb,"WAIT")==0)
    {
        if (strtok_r(nullptr," \t",&save))
            return Result::InvalidSyntax;
        if (strcmp(capability,"ready")!=0)
            return Result::InvalidSyntax;

        char query[96];
        char expected[32];
        char timeoutText[16];
        if (!m_configurationFile.getValue(
                path,"instrument","ready_query",query,sizeof(query)) ||
            !m_configurationFile.getValue(
                path,"instrument","ready_response",expected,sizeof(expected)) ||
            !m_configurationFile.getValue(
                path,"instrument","ready_timeout_ms",timeoutText,sizeof(timeoutText)) ||
            query[0]=='\0' || expected[0]=='\0')
            return Result::ConfigurationError;

        char* end=nullptr;
        const unsigned long parsedTimeout=strtoul(timeoutText,&end,10);
        while (end && (*end==' ' || *end=='\t'))
            ++end;
        if (end==timeoutText || !end || *end!='\0' ||
            parsedTimeout==0UL || parsedTimeout>120000UL)
            return Result::InvalidSynchronizationTimeout;

        return validateFraming();
    }

    return Result::InvalidSyntax;
}

void CommandExecutor::setDiagnosticCallback(DiagnosticCallback callback, void* context)
{
    m_diagnosticCallback = callback;
    m_diagnosticContext = context;
}

CommandExecutor::Result CommandExecutor::execute(
    const char* command, char* response, size_t responseSize,
    size_t& responseLength, uint32_t timeout)
{
    const Result result = executeCore(command, response, responseSize,
                                      responseLength, timeout);
    reportExecutionDiagnostic(result);
    return result;
}

void CommandExecutor::reportExecutionDiagnostic(Result result)
{
    const TransportDiagnostic& d = m_lastTransportDiagnostic;
    if (d.transport != Transport::None && result != Result::Success)
    {
        // Store a copy: the active diagnostic is cleared by the next execute().
        snprintf(m_failedInstrumentName, sizeof(m_failedInstrumentName),
                 "%s", m_lastInstrumentName);
        m_failedDiagnostic = d;
        m_failedDiagnostic.instrumentName = m_failedInstrumentName;
        m_pendingRecovery = true;
        if (m_diagnosticCallback)
            m_diagnosticCallback(false, d, lastTransportErrorText(),
                                 m_diagnosticContext);
    }
    else if (result == Result::Success && m_pendingRecovery &&
             m_lastInstrumentName[0] != '\0' &&
             strcmp(m_lastInstrumentName, m_failedInstrumentName) == 0)
    {
        m_pendingRecovery = false;
        if (m_diagnosticCallback)
            m_diagnosticCallback(true, m_failedDiagnostic, nullptr,
                                 m_diagnosticContext);
    }
}

CommandExecutor::Result CommandExecutor::executeCore(
    const char* command,
    char* response,
    size_t responseSize,
    size_t& responseLength,
    uint32_t timeout)
{
    m_lastTransportDiagnostic = {Transport::None, 0, nullptr, 0, 0, 0};
    m_lastInstrumentName[0] = '\0';
    responseLength=0;

    if (response && responseSize)
        response[0]='\0';

    if (!command)
        return Result::InvalidArgument;

    char copy[160];
    size_t len=strlen(command);

    if (len>=sizeof(copy))
        return Result::CommandTooLong;

    memcpy(copy,command,len+1);

    char* save=nullptr;
    char* verb=strtok_r(copy," \t",&save);
    char* name=strtok_r(nullptr," \t",&save);
    char* capability=strtok_r(nullptr," \t",&save);

    if (!verb || !name || !capability)
        return Result::InvalidSyntax;

    if (strcmp(verb,"SET")==0)
    {
        char* value=strtok_r(nullptr," \t",&save);
        char* extra=strtok_r(nullptr," \t",&save);

        if (!value || extra)
            return Result::InvalidSyntax;

        return executeSet(name,capability,value,timeout);
    }

    if (strcmp(verb,"QUERY")==0)
    {
        char* extra=strtok_r(nullptr," \t",&save);

        if (extra)
            return Result::InvalidSyntax;

        if (!response || !responseSize)
            return Result::InvalidArgument;

        return executeQuery(
            name,capability,response,responseSize,responseLength,timeout);
    }

    if (strcmp(verb,"ACTION")==0)
    {
        if (strtok_r(nullptr," \t",&save))
            return Result::InvalidSyntax;

        return executeAction(name,capability,timeout);
    }

    if (strcmp(verb,"ACTIONON")==0)
    {
        if (strtok_r(nullptr," \t",&save))
            return Result::InvalidSyntax;

        return executeActionOn(name,capability,timeout);
    }

    if (strcmp(verb,"ENTER")==0)
    {
        if (strtok_r(nullptr," \t",&save))
            return Result::InvalidSyntax;

        return executeFunctionEntry(name,capability,timeout);
    }

    if (strcmp(verb,"WAIT")==0)
    {
        if (strtok_r(nullptr," \t",&save))
            return Result::InvalidSyntax;

        // Synchronization uses the profile timeout, independent of the
        // ordinary per-command timeout supplied to execute().
        return executeWait(name,capability);
    }

    return Result::InvalidSyntax;
}

const char* CommandExecutor::resultText(Result r)
{
    switch(r)
    {
        case Result::Success: return "Success";
        case Result::InvalidArgument: return "Invalid argument";
        case Result::InvalidSyntax: return "Invalid syntax";
        case Result::ConfigurationError: return "Configuration error";
        case Result::UnsupportedInterface: return "Unsupported interface";
        case Result::InvalidAddress: return "Invalid address";
        case Result::ProfileError: return "Profile error";
        case Result::MissingTemplate: return "Missing command template";
        case Result::MissingValueToken: return "Missing {value} token";
        case Result::CommandTooLong: return "Command too long";
        case Result::ResponseBufferTooSmall: return "Response buffer too small";
        case Result::TransferError: return "Instrument transfer error";
        case Result::SynchronizationError: return "Instrument synchronization response mismatch";
        case Result::InvalidSynchronizationTimeout: return "Invalid synchronization timeout";
        case Result::ResponseParseError: return "Instrument response parse error";
    }

    return "Unknown";
}
}

namespace AK3Y {
void CommandExecutor::recordTransportFailure(
    const InstrumentConfiguration::Definition& definition,
    Transport transport, int resultCode)
{
    m_lastTransportDiagnostic = {transport, resultCode, m_lastInstrumentName,
                                 definition.address, definition.port, definition.tcpPort};
}

const CommandExecutor::TransportDiagnostic& CommandExecutor::lastTransportDiagnostic() const
{
    return m_lastTransportDiagnostic;
}

const char* CommandExecutor::lastTransportErrorText() const
{
    const auto& d = m_lastTransportDiagnostic;
    switch (d.transport) {
        case Transport::GPIB:
            return IEEE488::transferResultText(static_cast<IEEE488::TransferResult>(d.resultCode));
        case Transport::RS232:
            switch (static_cast<RS232::TransferResult>(d.resultCode)) {
                case RS232::TransferResult::Success: return "Success";
                case RS232::TransferResult::NotInitialized: return "RS-232 not initialized";
                case RS232::TransferResult::InvalidPort: return "Invalid RS-232 port";
                case RS232::TransferResult::InvalidArgument: return "RS-232 invalid argument or write failure";
                case RS232::TransferResult::Timeout: return "RS-232 response timeout";
                case RS232::TransferResult::BufferTooSmall: return "RS-232 response buffer too small";
            }
            return "Unknown RS-232 error";
        case Transport::TCP:
            switch (static_cast<TCPInstrument::TransferResult>(d.resultCode)) {
                case TCPInstrument::TransferResult::Success: return "Success";
                case TCPInstrument::TransferResult::InvalidArgument: return "TCP invalid argument";
                case TCPInstrument::TransferResult::ConnectionFailed: return "TCP connection failed";
                case TCPInstrument::TransferResult::Timeout: return "TCP response timeout";
                case TCPInstrument::TransferResult::BufferTooSmall: return "TCP response buffer too small";
                case TCPInstrument::TransferResult::TransferError: return "TCP transfer error or connection closed";
            }
            return "Unknown TCP error";
        default: return "No transport error recorded";
    }
}
} // namespace AK3Y
