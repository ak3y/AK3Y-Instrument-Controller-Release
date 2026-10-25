/******************************************************************************
 * File: Instrument.h
 * Project: AK3Y Instrument Controller
 * Description: Declares a generic GPIB-addressed instrument abstraction.
 * Responsibilities: Associates an IEEE-488 primary address with write and query operations; contains no instrument-specific command logic.
 * Author: Bob Fontana, AK3Y
 ******************************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "IEEE488.h"

namespace AK3Y
{

class Instrument
{
public:
    static constexpr uint8_t MaximumPrimaryAddress = 30;

    Instrument(
        IEEE488& ieee488,
        uint8_t address);

    IEEE488::TransferResult write(
        const char* message,
        uint32_t timeoutMilliseconds = 1000);

    IEEE488::TransferResult query(
        const char* message,
        char* response,
        size_t responseSize,
        size_t& responseLength,
        uint32_t timeoutMilliseconds = 1000);

    uint8_t address() const;

    bool setAddress(
        uint8_t address);

private:
    IEEE488& m_ieee488;
    uint8_t  m_address;
};

}   // namespace AK3Y
