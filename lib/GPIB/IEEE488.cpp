/******************************************************************************
 * File: IEEE488.cpp
 * Project: AK3Y Instrument Controller
 * Description: Implements IEEE-488 byte transfers and bus transactions
 *              above the GPIB electrical-interface abstraction.
 *
 * Talker byte-transfer sequence:
 *   1. Enter Talker mode.
 *   2. Release DAV.
 *   3. Place the byte on DIO1-DIO8.
 *   4. Establish the requested EOI state.
 *   5. Wait until NRFD is released: all listeners are ready.
 *   6. Assert DAV: data is valid.
 *   7. Wait until NDAC is released: all listeners accepted the byte.
 *   8. Release DAV.
 *   9. Wait until NDAC is asserted again: handshake reset.
 *  10. Release EOI.
 *
 * Command-byte transfers assert ATN for the entire handshake; data-byte
 * transfers release ATN. Primary addressing uses UNL (0x3F), UNT (0x5F),
 * LAD (0x20 + address), and TAD (0x40 + address), with addresses 0..30.
 * Protocol waits are bounded by timeouts, and hardware read failures are
 * distinguished from asserted/released signal states.
 *
 * Author: Bob Fontana, AK3Y
 ******************************************************************************/

#include "IEEE488.h"
#include "Configuration.h"

#include <Arduino.h>
#include <string.h>

namespace AK3Y
{

namespace
{

constexpr uint8_t UNLISTEN_COMMAND = 0x3F;
constexpr uint8_t UNTALK_COMMAND   = 0x5F;

constexpr uint8_t LISTENER_ADDRESS_BASE = 0x20;
constexpr uint8_t TALKER_ADDRESS_BASE   = 0x40;

constexpr uint8_t MAX_PRIMARY_ADDRESS = 30;

}   // anonymous namespace

IEEE488::IEEE488(
    GPIB& gpib)
    :
    m_gpib(gpib),
    m_initialized(false),
    m_activityCount(0)
{
}

bool IEEE488::initialize()
{
    m_activityCount = 0;
    m_initialized = true;
    return true;
}

uint32_t IEEE488::activityCount() const
{
    return m_activityCount;
}

/******************************************************************************
 *
 *  sendByte()
 *
 *  Compatibility entry point retained for existing diagnostics.
 *
 *  ATN is deliberately not changed here so that the existing diagnostic
 *  continues to exercise the byte-transfer primitive exactly as before,
 *  with verification of the final NDAC handshake reset.
 *
 ******************************************************************************/

IEEE488::TransferResult IEEE488::sendByte(
    uint8_t data,
    bool assertEOI,
    uint32_t timeoutMilliseconds)
{
    return transferByte(
        data,
        assertEOI,
        timeoutMilliseconds);
}

/******************************************************************************
 *
 *  sendCommandByte()
 *
 *  A command byte is transferred while ATN is asserted.
 *
 ******************************************************************************/

IEEE488::TransferResult IEEE488::sendCommandByte(
    uint8_t command,
    uint32_t timeoutMilliseconds)
{
    if (!m_initialized)
    {
        return TransferResult::NotInitialized;
    }

    if (!m_gpib.assertATN())
    {
        return TransferResult::ElectricalInterfaceError;
    }

    TransferResult result =
        transferByte(
            command,
            false,
            timeoutMilliseconds);

    if (!m_gpib.releaseATN())
    {
        return TransferResult::ElectricalInterfaceError;
    }

    return result;
}

/******************************************************************************
 *
 *  sendDataByte()
 *
 *  A data byte is transferred while ATN is released.
 *
 ******************************************************************************/

IEEE488::TransferResult IEEE488::sendDataByte(
    uint8_t data,
    bool assertEOI,
    uint32_t timeoutMilliseconds)
{
    if (!m_initialized)
    {
        return TransferResult::NotInitialized;
    }

    if (!m_gpib.releaseATN())
    {
        return TransferResult::ElectricalInterfaceError;
    }

    return transferByte(
        data,
        assertEOI,
        timeoutMilliseconds);
}

/******************************************************************************
 *
 *  IEEE-488 command helpers
 *
 ******************************************************************************/

IEEE488::TransferResult IEEE488::unlisten(
    uint32_t timeoutMilliseconds)
{
    return sendCommandByte(
        UNLISTEN_COMMAND,
        timeoutMilliseconds);
}

IEEE488::TransferResult IEEE488::untalk(
    uint32_t timeoutMilliseconds)
{
    return sendCommandByte(
        UNTALK_COMMAND,
        timeoutMilliseconds);
}

IEEE488::TransferResult IEEE488::addressListener(
    uint8_t address,
    uint32_t timeoutMilliseconds)
{
    if (!m_initialized)
    {
        return TransferResult::NotInitialized;
    }

    if (address > MAX_PRIMARY_ADDRESS)
    {
        return TransferResult::InvalidAddress;
    }

    return sendCommandByte(
        static_cast<uint8_t>(
            LISTENER_ADDRESS_BASE + address),
        timeoutMilliseconds);
}

IEEE488::TransferResult IEEE488::addressTalker(
    uint8_t address,
    uint32_t timeoutMilliseconds)
{
    if (!m_initialized)
    {
        return TransferResult::NotInitialized;
    }

    if (address > MAX_PRIMARY_ADDRESS)
    {
        return TransferResult::InvalidAddress;
    }

    return sendCommandByte(
        static_cast<uint8_t>(
            TALKER_ADDRESS_BASE + address),
        timeoutMilliseconds);
}

/******************************************************************************
 *
 *  beginReceiveFromTalker()
 *
 *  Address an instrument as Talker while ATN is asserted, then establish the
 *  controller Listener handshake state BEFORE releasing ATN.  This prevents
 *  a fast instrument from presenting its first response byte while the
 *  controller is still electrically configured as a Talker.
 *
 ******************************************************************************/

IEEE488::TransferResult IEEE488::beginReceiveFromTalker(
    uint8_t address,
    uint32_t timeoutMilliseconds)
{
    if (!m_initialized)
    {
        return TransferResult::NotInitialized;
    }

    if (address > MAX_PRIMARY_ADDRESS)
    {
        return TransferResult::InvalidAddress;
    }

    if (!m_gpib.assertATN())
    {
        return TransferResult::ElectricalInterfaceError;
    }

    TransferResult result =
        transferByte(
            static_cast<uint8_t>(
                TALKER_ADDRESS_BASE + address),
            false,
            timeoutMilliseconds);

    if (result != TransferResult::Success)
    {
        m_gpib.releaseATN();
        return result;
    }

    //
    // Explicitly address this controller as the Listener while ATN
    // remains asserted.
    //
    result =
        transferByte(
            static_cast<uint8_t>(
                LISTENER_ADDRESS_BASE + Configuration::GPIB::ControllerPrimaryAddress),
            false,
            timeoutMilliseconds);

    if (result != TransferResult::Success)
    {
        m_gpib.releaseATN();
        return result;
    }

    //
    // Keep ATN asserted while changing the controller from Talker to Listener.
    //

    if (!m_gpib.setMode(
            GPIB::Mode::Listener))
    {
        m_gpib.releaseATN();
        return TransferResult::ElectricalInterfaceError;
    }

    //
    // Initial Listener handshake state: not ready and not accepted.
    //

    if (!m_gpib.assertNRFD() ||
        !m_gpib.assertNDAC())
    {
        m_gpib.releaseATN();
        return TransferResult::ElectricalInterfaceError;
    }

    //
    // ATN release permits the addressed instrument to begin its response.
    // EOI also becomes a Listener input through the GPIB electrical layer.
    //

    if (!m_gpib.releaseATN())
    {
        return TransferResult::ElectricalInterfaceError;
    }

    return TransferResult::Success;
}

/******************************************************************************
 *
 *  receiveDataByte()
 *
 *  Complementary IEEE-488 Listener handshake for one data byte.
 *
 ******************************************************************************/

IEEE488::TransferResult IEEE488::receiveDataByte(
    uint8_t& data,
    bool& endOfMessage,
    uint32_t timeoutMilliseconds)
{
    if (!m_initialized)
    {
        return TransferResult::NotInitialized;
    }

    if (m_gpib.mode() != GPIB::Mode::Listener)
    {
        return TransferResult::ElectricalInterfaceError;
    }

    data = 0;
    endOfMessage = false;

    //
    // We are ready for one byte.
    //

    if (!m_gpib.releaseNRFD())
    {
        return TransferResult::ElectricalInterfaceError;
    }

    //
    // Wait for the Talker to assert DAV.
    //

    uint32_t startMilliseconds = millis();

    while (!m_gpib.isDAVAsserted())
    {
        if (timedOut(
                startMilliseconds,
                timeoutMilliseconds))
        {
            m_gpib.assertNRFD();
            return TransferResult::TimeoutWaitingForDAV;
        }

        yield();
    }

    //
    // DAV is asserted: data and EOI are stable.  Prevent the next byte while
    // we capture this one, then acknowledge acceptance by releasing NDAC.
    //

    if (!m_gpib.assertNRFD())
    {
        return TransferResult::ElectricalInterfaceError;
    }

    if (!m_gpib.readData(data))
    {
        return TransferResult::ElectricalInterfaceError;
    }

    endOfMessage =
        m_gpib.isEOIAsserted();

    if (!m_gpib.releaseNDAC())
    {
        return TransferResult::ElectricalInterfaceError;
    }

    //
    // Wait for the Talker to release DAV.
    //

    startMilliseconds = millis();

    while (m_gpib.isDAVAsserted())
    {
        if (timedOut(
                startMilliseconds,
                timeoutMilliseconds))
        {
            m_gpib.assertNDAC();
            return TransferResult::TimeoutWaitingForDAVRelease;
        }

        yield();
    }

    //
    // Return NDAC to the initial not-accepted state.  NRFD remains asserted
    // until the next receiveDataByte() call declares us ready.
    //

    if (!m_gpib.assertNDAC())
    {
        return TransferResult::ElectricalInterfaceError;
    }

    ++m_activityCount;
    return TransferResult::Success;
}

/******************************************************************************
 *
 *  transferByte()
 *
 *  Common Talker byte-transfer engine.
 *
 ******************************************************************************/

IEEE488::TransferResult IEEE488::transferByte(
    uint8_t data,
    bool assertEOI,
    uint32_t timeoutMilliseconds)
{
    if (!m_initialized)
    {
        return TransferResult::NotInitialized;
    }

    if (!m_gpib.setMode(
            GPIB::Mode::Talker))
    {
        return TransferResult::ElectricalInterfaceError;
    }

    if (!m_gpib.releaseDAV())
    {
        return TransferResult::ElectricalInterfaceError;
    }

    if (!m_gpib.writeData(data))
    {
        return TransferResult::ElectricalInterfaceError;
    }

    if (assertEOI)
    {
        if (!m_gpib.assertEOI())
        {
            return TransferResult::ElectricalInterfaceError;
        }
    }
    else
    {
        if (!m_gpib.releaseEOI())
        {
            return TransferResult::ElectricalInterfaceError;
        }
    }

    //
    // Wait until NRFD is released.
    //
    // NRFD is active LOW and wired-OR.  Released therefore means that all
    // addressed listeners are ready to accept the byte.
    //

    uint32_t startMilliseconds =
        millis();

    while (true)
    {
        bool nrfdAsserted = false;

        if (!m_gpib.readNRFD(
                nrfdAsserted))
        {
            m_gpib.releaseEOI();
            return TransferResult::ElectricalInterfaceError;
        }

        if (!nrfdAsserted)
        {
            break;
        }

        if (timedOut(
                startMilliseconds,
                timeoutMilliseconds))
        {
            m_gpib.releaseEOI();
            return TransferResult::TimeoutWaitingForReady;
        }

        yield();
    }

    //
    // Data is stable and all listeners are ready.  Assert DAV.
    //

    if (!m_gpib.assertDAV())
    {
        m_gpib.releaseEOI();
        return TransferResult::ElectricalInterfaceError;
    }

    //
    // Wait until NDAC is released.
    //
    // Released NDAC means that all addressed listeners have accepted
    // the byte.
    //

    startMilliseconds =
        millis();

    while (true)
    {
        bool ndacAsserted = false;

        if (!m_gpib.readNDAC(
                ndacAsserted))
        {
            m_gpib.releaseDAV();
            m_gpib.releaseEOI();
            return TransferResult::ElectricalInterfaceError;
        }

        if (!ndacAsserted)
        {
            break;
        }

        if (timedOut(
                startMilliseconds,
                timeoutMilliseconds))
        {
            m_gpib.releaseDAV();
            m_gpib.releaseEOI();
            return TransferResult::TimeoutWaitingForAccept;
        }

        yield();
    }

    //
    // All listeners have accepted the byte.  Release DAV.
    //

    if (!m_gpib.releaseDAV())
    {
        m_gpib.releaseEOI();
        return TransferResult::ElectricalInterfaceError;
    }

    //
    // Complete the handshake before allowing the data lines to change.
    //
    // After DAV is released, each listener reasserts NDAC.  Waiting for
    // NDAC to become asserted again establishes that the listener side
    // has returned to its initial handshake state and the byte cycle is
    // complete.
    //

    startMilliseconds =
        millis();

    while (true)
    {
        bool ndacAsserted = false;

        if (!m_gpib.readNDAC(
                ndacAsserted))
        {
            m_gpib.releaseEOI();
            return TransferResult::ElectricalInterfaceError;
        }

        if (ndacAsserted)
        {
            break;
        }

        if (timedOut(
                startMilliseconds,
                timeoutMilliseconds))
        {
            m_gpib.releaseEOI();
            return TransferResult::TimeoutWaitingForHandshakeReset;
        }

        yield();
    }

    if (!m_gpib.releaseEOI())
    {
        return TransferResult::ElectricalInterfaceError;
    }

    ++m_activityCount;
    return TransferResult::Success;
}

/******************************************************************************
 *
 *  write()
 *
 *  Send one complete program message to an addressed Listener.
 *
 ******************************************************************************/

IEEE488::TransferResult IEEE488::write(
    uint8_t address,
    const char* message,
    uint32_t timeoutMilliseconds)
{
    if (!m_initialized)
    {
        return TransferResult::NotInitialized;
    }

    if (address > MAX_PRIMARY_ADDRESS)
    {
        return TransferResult::InvalidAddress;
    }

    if (message == nullptr || message[0] == '\0')
    {
        return TransferResult::ElectricalInterfaceError;
    }

    TransferResult result =
        unlisten(timeoutMilliseconds);

    if (result != TransferResult::Success)
    {
        return result;
    }

    //
    // Explicitly address this controller as the Talker.
    //
    result =
        sendCommandByte(
            static_cast<uint8_t>(
                TALKER_ADDRESS_BASE + Configuration::GPIB::ControllerPrimaryAddress),
            timeoutMilliseconds);

    if (result != TransferResult::Success)
    {
        return result;
    }

    result =
        addressListener(
            address,
            timeoutMilliseconds);

    if (result != TransferResult::Success)
    {
        return result;
    }

    const size_t messageLength = strlen(message);

    for (size_t index = 0; index < messageLength; ++index)
    {
        result =
            sendDataByte(
                static_cast<uint8_t>(message[index]),
                index == messageLength - 1,
                timeoutMilliseconds);

        if (result != TransferResult::Success)
        {
            return result;
        }
    }

    return TransferResult::Success;
}

/******************************************************************************
 *
 *  query()
 *
 *  Send a query to an addressed Listener, turn the bus around, then receive
 *  bytes until the instrument asserts EOI.
 *
 ******************************************************************************/

IEEE488::TransferResult IEEE488::query(
    uint8_t address,
    const char* message,
    char* response,
    size_t responseSize,
    size_t& responseLength,
    uint32_t timeoutMilliseconds)
{
    responseLength = 0;

    if (response != nullptr && responseSize > 0)
    {
        response[0] = '\0';
    }

    if (!m_initialized)
    {
        return TransferResult::NotInitialized;
    }

    if (address > MAX_PRIMARY_ADDRESS)
    {
        return TransferResult::InvalidAddress;
    }

    if (message == nullptr ||
        message[0] == '\0' ||
        response == nullptr ||
        responseSize < 2)
    {
        return TransferResult::ElectricalInterfaceError;
    }

    TransferResult result =
        write(
            address,
            message,
            timeoutMilliseconds);

    if (result != TransferResult::Success)
    {
        return result;
    }

    result =
        untalk(timeoutMilliseconds);

    if (result != TransferResult::Success)
    {
        return result;
    }

    result =
        beginReceiveFromTalker(
            address,
            timeoutMilliseconds);

    if (result != TransferResult::Success)
    {
        return result;
    }

    bool endOfMessage = false;

    while (!endOfMessage)
    {
        if (responseLength >= responseSize - 1)
        {
            response[responseLength] = '\0';
            return TransferResult::BufferTooSmall;
        }

        uint8_t data = 0;

        result =
            receiveDataByte(
                data,
                endOfMessage,
                timeoutMilliseconds);

        if (result != TransferResult::Success)
        {
            response[responseLength] = '\0';
            return result;
        }

        response[responseLength++] =
            static_cast<char>(data);
    }

    response[responseLength] = '\0';

    return TransferResult::Success;
}

/******************************************************************************
 *
 *  transferResultText()
 *
 ******************************************************************************/

const char* IEEE488::transferResultText(
    TransferResult result)
{
    switch (result)
    {
        case TransferResult::Success:
            return "Success";

        case TransferResult::NotInitialized:
            return "IEEE488 not initialized";

        case TransferResult::ElectricalInterfaceError:
            return "GPIB electrical interface error";

        case TransferResult::InvalidAddress:
            return "Invalid IEEE-488 primary address";

        case TransferResult::TimeoutWaitingForReady:
            return "Timeout waiting for NRFD release";

        case TransferResult::TimeoutWaitingForAccept:
            return "Timeout waiting for NDAC release";

        case TransferResult::TimeoutWaitingForHandshakeReset:
            return "Timeout waiting for NDAC handshake reset";

        case TransferResult::TimeoutWaitingForDAV:
            return "Timeout waiting for DAV assertion";

        case TransferResult::TimeoutWaitingForDAVRelease:
            return "Timeout waiting for DAV release";

        case TransferResult::BufferTooSmall:
            return "Response buffer too small";

        default:
            return "Unknown IEEE488 transfer result";
    }
}

/******************************************************************************
 *
 *  timedOut()
 *
 ******************************************************************************/

bool IEEE488::timedOut(
    uint32_t startMilliseconds,
    uint32_t timeoutMilliseconds) const
{
    //
    // Unsigned subtraction intentionally handles millis() wraparound.
    //

    return static_cast<uint32_t>(
               millis() - startMilliseconds) >=
           timeoutMilliseconds;
}

}   // namespace AK3Y
