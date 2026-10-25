/*
 * AK3Y Instrument Controller
 * File: TCPInstrument.cpp
 * Purpose: TCP instrument transport implementation
 * Responsibilities: Uses Wi-Fi TCP client connections to send instrument commands and receive responses, with transfer status and activity accounting.
 * Dependencies: TCPInstrument.h, Arduino WiFiClient
 *
 * This header documents the current module responsibilities.
 */

#include "TCPInstrument.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <string.h>

namespace AK3Y {
namespace {

bool endsWith(
    const uint8_t* data,
    size_t dataLength,
    const uint8_t* suffix,
    size_t suffixLength)
{
    if (!data || !suffix || suffixLength==0 || dataLength<suffixLength)
        return false;

    return memcmp(
        data+dataLength-suffixLength,
        suffix,
        suffixLength)==0;
}

}

TCPInstrument::TCPInstrument()
    : m_activityCount(0)
{
}

uint32_t TCPInstrument::activityCount() const
{
    return m_activityCount;
}

TCPInstrument::TransferResult TCPInstrument::write(
    const char* host,
    uint16_t port,
    const uint8_t* data,
    size_t length,
    uint32_t timeoutMilliseconds)
{
    if (!host || !*host || port==0 || !data || length==0)
        return TransferResult::InvalidArgument;

    WiFiClient client;
    client.setTimeout(timeoutMilliseconds);

    if (!client.connect(host,port,timeoutMilliseconds))
        return TransferResult::ConnectionFailed;

    const size_t written=client.write(data,length);
    client.flush();
    client.stop();

    if (written==length)
    {
        ++m_activityCount;
        return TransferResult::Success;
    }

    return TransferResult::TransferError;
}

TCPInstrument::TransferResult TCPInstrument::queryUntil(
    const char* host,
    uint16_t port,
    const uint8_t* request,
    size_t requestLength,
    uint8_t* response,
    size_t responseCapacity,
    const uint8_t* terminator,
    size_t terminatorLength,
    size_t& responseLength,
    uint32_t timeoutMilliseconds)
{
    responseLength=0;

    if (!host || !*host || port==0 || !request || requestLength==0 ||
        !response || responseCapacity==0 || !terminator || terminatorLength==0)
        return TransferResult::InvalidArgument;

    WiFiClient client;
    client.setTimeout(timeoutMilliseconds);

    if (!client.connect(host,port,timeoutMilliseconds))
        return TransferResult::ConnectionFailed;

    if (client.write(request,requestLength)!=requestLength)
    {
        client.stop();
        return TransferResult::TransferError;
    }

    client.flush();
    const uint32_t start=millis();

    while (true)
    {
        while (client.available()>0)
        {
            const int value=client.read();
            if (value<0)
                break;

            if (responseLength>=responseCapacity)
            {
                client.stop();
                return TransferResult::BufferTooSmall;
            }

            response[responseLength++]=static_cast<uint8_t>(value);

            if (endsWith(response,responseLength,terminator,terminatorLength))
            {
                client.stop();
                ++m_activityCount;
                return TransferResult::Success;
            }
        }

        if (!client.connected() && client.available()==0)
        {
            client.stop();
            return TransferResult::TransferError;
        }

        if (static_cast<uint32_t>(millis()-start)>=timeoutMilliseconds)
        {
            client.stop();
            return TransferResult::Timeout;
        }

        delay(1);
    }
}

}
