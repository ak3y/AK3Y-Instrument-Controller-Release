/******************************************************************************
 * File: InstrumentConfiguration.h
 * Project: AK3Y Instrument Controller
 * Description: Declares the instrument configuration model and loader.
 * Responsibilities: Defines interface-specific settings and APIs for enumerating and loading configured instruments.
 * Author: Bob Fontana, AK3Y
 ******************************************************************************/

#pragma once
#include <stddef.h>
#include <stdint.h>

namespace AK3Y {
class ConfigurationFile;
class SDCard;

class InstrumentConfiguration {
public:
    enum class Interface : uint8_t { GPIB, RS232, TCP };
    enum class Parity : uint8_t { None, Even, Odd };

    struct Definition {
        char section[32];
        char name[80];
        char profile[96];
        Interface interfaceType;
        uint8_t address;
        uint8_t port;
        uint32_t baudRate;
        uint8_t dataBits;
        Parity parity;
        uint8_t stopBits;
        char host[64];
        uint16_t tcpPort;
    };

    InstrumentConfiguration(SDCard& sdCard, ConfigurationFile& configurationFile);

    bool sectionName(size_t index, char* section, size_t sectionSize, bool& fileOK);
    bool load(const char* section, Definition& definition);

    static const char* interfaceText(Interface interfaceType);
    static const char* parityText(Parity parity);

private:
    SDCard& m_sdCard;
    ConfigurationFile& m_configurationFile;

    static char* trim(char* text);
    static bool parseLong(const char* text, long minimum, long maximum, long& value);
    static bool parseUnsignedLong(const char* text, unsigned long minimum,
                                  unsigned long maximum, unsigned long& value);
};
}
