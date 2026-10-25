/******************************************************************************
 * File: IEEE488.h
 * Project: AK3Y Instrument Controller
 * Description: Declares IEEE-488 protocol services layered above GPIB.
 *
 * Responsibilities:
 *   - Execute Talker and Listener byte-transfer handshakes.
 *   - Apply bounded protocol timeouts and report explicit transfer results.
 *   - Send and receive command/data bytes and manage primary addressing.
 *   - Provide instrument-independent write and query transactions.
 *
 * This layer is independent of MCP23S17, ESP32 GPIO, PCB implementation,
 * SCPI syntax, and instrument-specific commands.
 *
 * Author: Bob Fontana, AK3Y
 ******************************************************************************/

#pragma once

#include <stdint.h>
#include <stddef.h>

#include "GPIB.h"

namespace AK3Y
{

class IEEE488
{
public:
    enum class TransferResult
    {
        Success,
        NotInitialized,
        ElectricalInterfaceError,
        InvalidAddress,
        TimeoutWaitingForReady,
        TimeoutWaitingForAccept,
        TimeoutWaitingForHandshakeReset,
        TimeoutWaitingForDAV,
        TimeoutWaitingForDAVRelease,
        BufferTooSmall
    };

    explicit IEEE488(
        GPIB& gpib);

    bool initialize();

    uint32_t activityCount() const;

    /*
     * Compatibility entry point retained for existing diagnostics.
     *
     * This performs one Talker byte handshake without changing ATN.
     * Existing diagnostics can therefore continue to use it unchanged.
     */
    TransferResult sendByte(
        uint8_t data,
        bool assertEOI = false,
        uint32_t timeoutMilliseconds = 1000);

    /*
     * Command bytes are transferred with ATN asserted.
     */
    TransferResult sendCommandByte(
        uint8_t command,
        uint32_t timeoutMilliseconds = 1000);

    /*
     * Data bytes are transferred with ATN released.
     */
    TransferResult sendDataByte(
        uint8_t data,
        bool assertEOI = false,
        uint32_t timeoutMilliseconds = 1000);

    /*
     * IEEE-488 universal addressing commands.
     */
    TransferResult unlisten(
        uint32_t timeoutMilliseconds = 1000);

    TransferResult untalk(
        uint32_t timeoutMilliseconds = 1000);

    /*
     * Primary addresses are restricted to 0 through 30.
     */
    TransferResult addressListener(
        uint8_t address,
        uint32_t timeoutMilliseconds = 1000);

    TransferResult addressTalker(
        uint8_t address,
        uint32_t timeoutMilliseconds = 1000);

    /*
     * Address an instrument as Talker and atomically prepare this controller
     * as a Listener before ATN is released.  This avoids losing the first
     * response byte from a fast instrument.
     */
    TransferResult beginReceiveFromTalker(
        uint8_t address,
        uint32_t timeoutMilliseconds = 1000);

    /*
     * Receive one data byte as an IEEE-488 Listener.  endOfMessage is true
     * when EOI was asserted with that byte.
     */
    TransferResult receiveDataByte(
        uint8_t& data,
        bool& endOfMessage,
        uint32_t timeoutMilliseconds = 1000);

    /*
     * Send one complete program message to an addressed Listener.
     * EOI is asserted with the final message byte.
     */
    TransferResult write(
        uint8_t address,
        const char* message,
        uint32_t timeoutMilliseconds = 1000);

    /*
     * Send a query, turn the bus around, and receive the response through EOI.
     * responseSize includes space for the terminating NUL.  responseLength
     * receives the number of bytes returned by the instrument, excluding NUL.
     */
    TransferResult query(
        uint8_t address,
        const char* message,
        char* response,
        size_t responseSize,
        size_t& responseLength,
        uint32_t timeoutMilliseconds = 1000);

    static const char* transferResultText(
        TransferResult result);

private:
    TransferResult transferByte(
        uint8_t data,
        bool assertEOI,
        uint32_t timeoutMilliseconds);

    bool timedOut(
        uint32_t startMilliseconds,
        uint32_t timeoutMilliseconds) const;

private:
    GPIB& m_gpib;
    bool  m_initialized;
    uint32_t m_activityCount;
};

}   // namespace AK3Y
