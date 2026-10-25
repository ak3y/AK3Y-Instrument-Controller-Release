/*
 * AK3Y Instrument Controller
 * File: TCPInstrument.h
 * Purpose: TCP instrument transport interface
 * Responsibilities: Declares TCP command transmission, terminator-based queries, transfer results, and activity counting.
 * Dependencies: Standard integer and size types
 *
 * This header documents the current module responsibilities.
 */

#pragma once
#include <stddef.h>
#include <stdint.h>

namespace AK3Y {

class TCPInstrument {
public:
    enum class TransferResult : uint8_t {
        Success=0,
        InvalidArgument,
        ConnectionFailed,
        Timeout,
        BufferTooSmall,
        TransferError
    };

    TCPInstrument();

    TransferResult write(
        const char* host,
        uint16_t port,
        const uint8_t* data,
        size_t length,
        uint32_t timeoutMilliseconds=1000);

    TransferResult queryUntil(
        const char* host,
        uint16_t port,
        const uint8_t* request,
        size_t requestLength,
        uint8_t* response,
        size_t responseCapacity,
        const uint8_t* terminator,
        size_t terminatorLength,
        size_t& responseLength,
        uint32_t timeoutMilliseconds=1000);

    uint32_t activityCount() const;

private:
    uint32_t m_activityCount;
};

}
