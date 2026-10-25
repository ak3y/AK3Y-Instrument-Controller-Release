/*
 * AK3Y Instrument Controller
 * File: RS232.cpp
 * Purpose: Dual-port RS-232 implementation
 * Responsibilities: Configures the two ESP32 hardware UARTs; provides byte-oriented read, write, and query operations; tracks port activity and supports serial diagnostics.
 * Dependencies: RS232.h, Configuration, Console, Arduino HardwareSerial
 *
 * This header documents the current module responsibilities.
 */

#include "RS232.h"
#include <Configuration.h>
#include <Console.h>
#include <string.h>

namespace AK3Y {
namespace {
constexpr uint32_t DiagnosticTimeoutMs = 500;
constexpr RS232::PortConfiguration DefaultConfiguration =
    {9600, 8, RS232::Parity::None, 1};
}

RS232::RS232(Console& console)
    : m_console(console), m_port1(1), m_port2(2),
      m_port1Configuration(DefaultConfiguration),
      m_port2Configuration(DefaultConfiguration), m_initialized(false),
      m_port1ActivityCount(0), m_port2ActivityCount(0) {}

bool RS232::initialize() {
    if (!configureHardwarePort(m_port1, Configuration::Pins::RS232::Port1RX,
            Configuration::Pins::RS232::Port1TX, m_port1Configuration)) return false;
    if (!configureHardwarePort(m_port2, Configuration::Pins::RS232::Port2RX,
            Configuration::Pins::RS232::Port2TX, m_port2Configuration)) return false;
    m_initialized = true;
    return true;
}

bool RS232::configurePort(uint8_t portNumber, const PortConfiguration& c) {
    if (!m_initialized || !validConfiguration(c)) return false;
    if (portNumber == 1) {
        if (!configureHardwarePort(m_port1, Configuration::Pins::RS232::Port1RX,
                Configuration::Pins::RS232::Port1TX, c)) return false;
        m_port1Configuration = c; return true;
    }
    if (portNumber == 2) {
        if (!configureHardwarePort(m_port2, Configuration::Pins::RS232::Port2RX,
                Configuration::Pins::RS232::Port2TX, c)) return false;
        m_port2Configuration = c; return true;
    }
    return false;
}

RS232::PortConfiguration RS232::portConfiguration(uint8_t portNumber) const {
    return portNumber == 2 ? m_port2Configuration : m_port1Configuration;
}

uint32_t RS232::activityCount(uint8_t portNumber) const {
    if (portNumber == 1) return m_port1ActivityCount;
    if (portNumber == 2) return m_port2ActivityCount;
    return 0;
}

bool RS232::configureHardwarePort(HardwareSerial& serial, uint8_t rxPin, uint8_t txPin,
                                  const PortConfiguration& c) {
    if (!validConfiguration(c)) return false;
    serial.end();
    serial.begin(c.baudRate, serialMode(c), rxPin, txPin);
    return true;
}

bool RS232::validConfiguration(const PortConfiguration& c) const {
    return c.baudRate != 0 && c.dataBits >= 5 && c.dataBits <= 8 &&
           (c.stopBits == 1 || c.stopBits == 2);
}

uint32_t RS232::serialMode(const PortConfiguration& c) const {
#define PICK(bits) \
    if (c.parity == Parity::Even) return c.stopBits == 2 ? SERIAL_##bits##E2 : SERIAL_##bits##E1; \
    if (c.parity == Parity::Odd)  return c.stopBits == 2 ? SERIAL_##bits##O2 : SERIAL_##bits##O1; \
    return c.stopBits == 2 ? SERIAL_##bits##N2 : SERIAL_##bits##N1
    if (c.dataBits == 5) { PICK(5); }
    if (c.dataBits == 6) { PICK(6); }
    if (c.dataBits == 7) { PICK(7); }
    { PICK(8); }
#undef PICK
}

HardwareSerial* RS232::port(uint8_t portNumber) {
    if (portNumber == 1) return &m_port1;
    if (portNumber == 2) return &m_port2;
    return nullptr;
}

RS232::TransferResult RS232::write(
    uint8_t portNumber,
    const uint8_t* data,
    size_t length) {
    if (!m_initialized) return TransferResult::NotInitialized;
    HardwareSerial* serial = port(portNumber);
    if (serial == nullptr) return TransferResult::InvalidPort;
    if (data == nullptr || length == 0) return TransferResult::InvalidArgument;

    while (serial->available() > 0) serial->read();

    size_t written = serial->write(data, length);
    serial->flush();

    if (written == length) {
        if (portNumber == 1) ++m_port1ActivityCount;
        else if (portNumber == 2) ++m_port2ActivityCount;
    }

    return written == length ?
        TransferResult::Success :
        TransferResult::InvalidArgument;
}

RS232::TransferResult RS232::read(
    uint8_t portNumber,
    uint8_t* data,
    size_t capacity,
    size_t expectedLength,
    size_t& receivedLength,
    uint32_t timeoutMilliseconds) {
    receivedLength = 0;

    if (!m_initialized) return TransferResult::NotInitialized;
    HardwareSerial* serial = port(portNumber);
    if (serial == nullptr) return TransferResult::InvalidPort;
    if (data == nullptr || capacity == 0 || expectedLength == 0)
        return TransferResult::InvalidArgument;
    if (expectedLength > capacity)
        return TransferResult::BufferTooSmall;

    uint32_t startMs = millis();

    while (receivedLength < expectedLength) {
        while (serial->available() > 0 && receivedLength < expectedLength)
            data[receivedLength++] = static_cast<uint8_t>(serial->read());

        if (receivedLength >= expectedLength)
            return TransferResult::Success;

        if (static_cast<uint32_t>(millis() - startMs) >= timeoutMilliseconds)
            return TransferResult::Timeout;

        yield();
    }

    return TransferResult::Success;
}

RS232::TransferResult RS232::query(
    uint8_t portNumber,
    const uint8_t* request,
    size_t requestLength,
    uint8_t* response,
    size_t responseCapacity,
    size_t expectedResponseLength,
    size_t& responseLength,
    uint32_t timeoutMilliseconds) {
    responseLength = 0;

    TransferResult result =
        write(portNumber, request, requestLength);

    if (result != TransferResult::Success)
        return result;

    return read(
        portNumber,
        response,
        responseCapacity,
        expectedResponseLength,
        responseLength,
        timeoutMilliseconds);
}

RS232::TransferResult RS232::readUntil(
    uint8_t portNumber,
    uint8_t* data,
    size_t capacity,
    const uint8_t* terminator,
    size_t terminatorLength,
    size_t& receivedLength,
    uint32_t timeoutMilliseconds) {
    receivedLength = 0;

    if (!m_initialized)
        return TransferResult::NotInitialized;

    HardwareSerial* serial = port(portNumber);

    if (serial == nullptr)
        return TransferResult::InvalidPort;

    if (data == nullptr || capacity == 0 ||
        terminator == nullptr || terminatorLength == 0)
        return TransferResult::InvalidArgument;

    if (terminatorLength > capacity)
        return TransferResult::BufferTooSmall;

    uint32_t startMs = millis();

    for (;;) {
        while (serial->available() > 0) {
            if (receivedLength >= capacity)
                return TransferResult::BufferTooSmall;

            data[receivedLength++] =
                static_cast<uint8_t>(serial->read());

            if (receivedLength >= terminatorLength &&
                memcmp(
                    data + receivedLength - terminatorLength,
                    terminator,
                    terminatorLength) == 0)
                return TransferResult::Success;
        }

        if (static_cast<uint32_t>(millis() - startMs) >= timeoutMilliseconds)
            return TransferResult::Timeout;

        yield();
    }
}

RS232::TransferResult RS232::queryUntil(
    uint8_t portNumber,
    const uint8_t* request,
    size_t requestLength,
    uint8_t* response,
    size_t responseCapacity,
    const uint8_t* terminator,
    size_t terminatorLength,
    size_t& responseLength,
    uint32_t timeoutMilliseconds) {
    responseLength = 0;

    TransferResult result =
        write(portNumber, request, requestLength);

    if (result != TransferResult::Success)
        return result;

    return readUntil(
        portNumber,
        response,
        responseCapacity,
        terminator,
        terminatorLength,
        responseLength,
        timeoutMilliseconds);
}

bool RS232::runDiagnostics() {
    //
    // Production startup diagnostic.
    //
    // Deliberately non-invasive: do not transmit data and do not require
    // loopback jumpers or an attached instrument. Initialization has
    // already configured both UARTs; here we verify that the RS-232 service
    // is initialized and that both stored port configurations remain valid.
    //
    if (!m_initialized)
        return false;

    if (!validConfiguration(m_port1Configuration))
        return false;

    if (!validConfiguration(m_port2Configuration))
        return false;

    return true;
}

bool RS232::runExternalLoopbackDiagnostics() {
    if (!m_initialized) return false;

    m_console.println();
    m_console.println("RS-232 External Loopback Test - v0.71(b)");
    m_console.println("----------------------------------------");
    m_console.println("DB9 pins 2 and 3 must be jumpered on both ports.");

    const PortConfiguration configs[] = {
        {9600, 8, Parity::None, 1},
        {19200, 7, Parity::Even, 1},
        {38400, 8, Parity::Odd, 2}
    };
    const char* names[] = {"9600 8-N-1", "19200 7-E-1", "38400 8-O-2"};
    bool success = true;

    for (size_t i = 0; i < 3; ++i) {
        if (!configurePort(1, configs[i]) || !configurePort(2, configs[i])) {
            success = false;
            break;
        }

        m_console.printf("Configuration: %s\n", names[i]);

        if (!testTransport(1, "RS232 Port 1", "AK3Y-RS232-PORT1"))
            success = false;

        if (!testTransport(2, "RS232 Port 2", "AK3Y-RS232-PORT2"))
            success = false;

        if (!testTerminatedTransport(1, "Port 1 CR/LF"))
            success = false;

        if (!testTerminatedTransport(2, "Port 2 CR/LF"))
            success = false;
    }

    if (!configurePort(1, DefaultConfiguration) ||
        !configurePort(2, DefaultConfiguration))
        success = false;

    m_console.println("----------------------------------------");
    return success;
}

bool RS232::testTransport(
    uint8_t portNumber,
    const char* portName,
    const char* testMessage) {
    const size_t length = strlen(testMessage);
    uint8_t received[32];
    size_t receivedLength = 0;

    TransferResult result =
        query(
            portNumber,
            reinterpret_cast<const uint8_t*>(testMessage),
            length,
            received,
            sizeof(received),
            length,
            receivedLength,
            DiagnosticTimeoutMs);

    const bool success =
        result == TransferResult::Success &&
        receivedLength == length &&
        memcmp(received, testMessage, length) == 0;

    m_console.printf(
        "%-20s%s  TX=%u RX=%u\n",
        portName,
        success ? "PASS" : "FAIL",
        static_cast<unsigned>(length),
        static_cast<unsigned>(receivedLength));

    return success;
}

bool RS232::testTerminatedTransport(
    uint8_t portNumber,
    const char* portName) {
    static const uint8_t message[] =
        "AK3Y-TERMINATED-TEST\r\n";

    static const uint8_t terminator[] =
        {'\r', '\n'};

    uint8_t received[40];
    size_t receivedLength = 0;

    const size_t messageLength = sizeof(message) - 1;

    TransferResult result =
        queryUntil(
            portNumber,
            message,
            messageLength,
            received,
            sizeof(received),
            terminator,
            sizeof(terminator),
            receivedLength,
            DiagnosticTimeoutMs);

    const bool success =
        result == TransferResult::Success &&
        receivedLength == messageLength &&
        memcmp(received, message, messageLength) == 0;

    m_console.printf(
        "%-20s%s  RX=%u TERM=CR/LF\n",
        portName,
        success ? "PASS" : "FAIL",
        static_cast<unsigned>(receivedLength));

    return success;
}

}
