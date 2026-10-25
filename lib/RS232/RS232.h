/*
 * AK3Y Instrument Controller
 * File: RS232.h
 * Purpose: Dual-port RS-232 interface
 * Responsibilities: Declares serial-port configuration, transfer results, diagnostics, and read/write/query operations for the two hardware UARTs.
 * Dependencies: Arduino HardwareSerial
 *
 * This header documents the current module responsibilities.
 */

#pragma once
#include <Arduino.h>

namespace AK3Y {

class Console;

class RS232 {
public:
    enum class Parity { None, Even, Odd };

    enum class TransferResult
    {
        Success,
        NotInitialized,
        InvalidPort,
        InvalidArgument,
        Timeout,
        BufferTooSmall
    };

    struct PortConfiguration {
        uint32_t baudRate;
        uint8_t dataBits;
        Parity parity;
        uint8_t stopBits;
    };

    explicit RS232(Console& console);

    bool initialize();
    bool runDiagnostics();
    bool runExternalLoopbackDiagnostics();

    bool configurePort(
        uint8_t portNumber,
        const PortConfiguration& configuration);

    PortConfiguration portConfiguration(uint8_t portNumber) const;

    uint32_t activityCount(uint8_t portNumber) const;

    TransferResult write(
        uint8_t portNumber,
        const uint8_t* data,
        size_t length);

    TransferResult read(
        uint8_t portNumber,
        uint8_t* data,
        size_t capacity,
        size_t expectedLength,
        size_t& receivedLength,
        uint32_t timeoutMilliseconds = 1000);

    TransferResult query(
        uint8_t portNumber,
        const uint8_t* request,
        size_t requestLength,
        uint8_t* response,
        size_t responseCapacity,
        size_t expectedResponseLength,
        size_t& responseLength,
        uint32_t timeoutMilliseconds = 1000);

    //
    // variable-length receive/query terminated by an arbitrary
    // byte sequence.  The returned response includes the terminator.
    //
    TransferResult readUntil(
        uint8_t portNumber,
        uint8_t* data,
        size_t capacity,
        const uint8_t* terminator,
        size_t terminatorLength,
        size_t& receivedLength,
        uint32_t timeoutMilliseconds = 1000);

    TransferResult queryUntil(
        uint8_t portNumber,
        const uint8_t* request,
        size_t requestLength,
        uint8_t* response,
        size_t responseCapacity,
        const uint8_t* terminator,
        size_t terminatorLength,
        size_t& responseLength,
        uint32_t timeoutMilliseconds = 1000);

private:
    bool configureHardwarePort(
        HardwareSerial& serial,
        uint8_t rxPin,
        uint8_t txPin,
        const PortConfiguration& configuration);

    bool validConfiguration(
        const PortConfiguration& configuration) const;

    uint32_t serialMode(
        const PortConfiguration& configuration) const;

    HardwareSerial* port(uint8_t portNumber);

    bool testTransport(
        uint8_t portNumber,
        const char* portName,
        const char* testMessage);

    bool testTerminatedTransport(
        uint8_t portNumber,
        const char* portName);

    Console& m_console;
    HardwareSerial m_port1;
    HardwareSerial m_port2;
    PortConfiguration m_port1Configuration;
    PortConfiguration m_port2Configuration;
    bool m_initialized;
    uint32_t m_port1ActivityCount;
    uint32_t m_port2ActivityCount;
};

}
