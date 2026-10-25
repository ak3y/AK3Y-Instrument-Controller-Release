/******************************************************************************
 * File: Instrument.cpp
 * Project: AK3Y Instrument Controller
 * Description: Implements generic message operations for a GPIB-addressed instrument.
 * Responsibilities: Delegates writes and queries to IEEE488 without interpreting instrument commands.
 * Author: Bob Fontana, AK3Y
 ******************************************************************************/

#include "Instrument.h"

namespace AK3Y
{

Instrument::Instrument(
    IEEE488& ieee488,
    uint8_t address)
    :
    m_ieee488(ieee488),
    m_address(
        address <= MaximumPrimaryAddress
            ? address
            : MaximumPrimaryAddress)
{
}

IEEE488::TransferResult Instrument::write(
    const char* message,
    uint32_t timeoutMilliseconds)
{
    return m_ieee488.write(
        m_address,
        message,
        timeoutMilliseconds);
}

IEEE488::TransferResult Instrument::query(
    const char* message,
    char* response,
    size_t responseSize,
    size_t& responseLength,
    uint32_t timeoutMilliseconds)
{
    return m_ieee488.query(
        m_address,
        message,
        response,
        responseSize,
        responseLength,
        timeoutMilliseconds);
}

uint8_t Instrument::address() const
{
    return m_address;
}

bool Instrument::setAddress(
    uint8_t address)
{
    if (address > MaximumPrimaryAddress)
    {
        return false;
    }

    m_address = address;
    return true;
}

}   // namespace AK3Y
