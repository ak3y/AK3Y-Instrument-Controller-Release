/******************************************************************************
 * File: InstrumentConfiguration.cpp
 * Project: AK3Y Instrument Controller
 * Description: Loads and validates instrument definitions from the SD card.
 * Responsibilities: Reads /config/instruments.cfg and parses GPIB, RS-232, and TCP instrument settings.
 * Author: Bob Fontana, AK3Y
 ******************************************************************************/

#include "InstrumentConfiguration.h"
#include <ConfigurationFile.h>
#include <SDCard.h>
#include <FS.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace AK3Y {
namespace { constexpr const char* ConfigPath="/config/instruments.cfg"; }

InstrumentConfiguration::InstrumentConfiguration(
    SDCard& sdCard, ConfigurationFile& configurationFile)
    : m_sdCard(sdCard), m_configurationFile(configurationFile) {}

char* InstrumentConfiguration::trim(char* text) {
    if (!text) return text;
    while (*text && isspace(static_cast<unsigned char>(*text))) ++text;
    char* end=text+strlen(text);
    while (end>text && isspace(static_cast<unsigned char>(*(end-1)))) --end;
    *end='\0';
    return text;
}

bool InstrumentConfiguration::sectionName(
    size_t index, char* section, size_t sectionSize, bool& fileOK) {
    fileOK=false;
    if (!section || sectionSize==0) return false;
    section[0]='\0';

    File file=m_sdCard.open(ConfigPath, FILE_READ);
    if (!file) return false;
    fileOK=true;

    size_t sectionIndex=0;
    char line[160];

    while (file.available()) {
        size_t length=file.readBytesUntil('\n',line,sizeof(line)-1);
        line[length]='\0';
        char* text=trim(line);

        if (*text=='\0' || *text=='#' || *text==';') continue;

        size_t textLength=strlen(text);
        if (textLength<3 || text[0]!='[' || text[textLength-1]!=']') continue;

        text[textLength-1]='\0';
        char* name=trim(text+1);
        if (*name=='\0') continue;

        if (sectionIndex++ != index) continue;

        if (strlen(name)>=sectionSize) {
            file.close();
            fileOK=false;
            return false;
        }

        strcpy(section,name);
        file.close();
        return true;
    }

    file.close();
    return false;
}

bool InstrumentConfiguration::parseLong(
    const char* text,long minimum,long maximum,long& value) {
    if (!text || !*text) return false;
    char* end=nullptr;
    long parsed=strtol(text,&end,10);
    if (end==text || *end!='\0' || parsed<minimum || parsed>maximum) return false;
    value=parsed;
    return true;
}

bool InstrumentConfiguration::parseUnsignedLong(
    const char* text,unsigned long minimum,unsigned long maximum,unsigned long& value) {
    if (!text || !*text) return false;
    char* end=nullptr;
    unsigned long parsed=strtoul(text,&end,10);
    if (end==text || *end!='\0' || parsed<minimum || parsed>maximum) return false;
    value=parsed;
    return true;
}

bool InstrumentConfiguration::load(
    const char* section, Definition& definition) {
    memset(&definition,0,sizeof(definition));
    if (!section || strlen(section)>=sizeof(definition.section)) return false;
    strcpy(definition.section,section);

    char interfaceName[16]="";
    char addressText[16]="";
    char portText[8]="";
    char baudText[16]="";
    char dataBitsText[8]="";
    char parityValue[16]="";
    char stopBitsText[8]="";
    char tcpPortText[8]="";

    ConfigurationFile::ValueRequest common[]={
        {"name",definition.name,sizeof(definition.name),true,false},
        {"profile",definition.profile,sizeof(definition.profile),true,false},
        {"interface",interfaceName,sizeof(interfaceName),true,false}
    };

    if (!m_configurationFile.getValues(
            ConfigPath,section,common,sizeof(common)/sizeof(common[0])))
        return false;

    if (strcmp(interfaceName,"GPIB")==0) {
        ConfigurationFile::ValueRequest requests[]={
            {"address",addressText,sizeof(addressText),true,false}
        };
        if (!m_configurationFile.getValues(
                ConfigPath,section,requests,sizeof(requests)/sizeof(requests[0])))
            return false;

        long address=0;
        if (!parseLong(addressText,0,30,address)) return false;

        definition.interfaceType=Interface::GPIB;
        definition.address=static_cast<uint8_t>(address);
        return true;
    }

    if (strcmp(interfaceName,"RS232")==0) {
        ConfigurationFile::ValueRequest requests[]={
            {"port",portText,sizeof(portText),true,false},
            {"baud",baudText,sizeof(baudText),true,false},
            {"data_bits",dataBitsText,sizeof(dataBitsText),true,false},
            {"parity",parityValue,sizeof(parityValue),true,false},
            {"stop_bits",stopBitsText,sizeof(stopBitsText),true,false}
        };
        if (!m_configurationFile.getValues(
                ConfigPath,section,requests,sizeof(requests)/sizeof(requests[0])))
            return false;

        long port=0,dataBits=0,stopBits=0;
        unsigned long baud=0;
        if (!parseLong(portText,1,2,port) ||
            !parseUnsignedLong(baudText,1,4000000UL,baud) ||
            !parseLong(dataBitsText,5,8,dataBits) ||
            !parseLong(stopBitsText,1,2,stopBits))
            return false;

        Parity parity;
        if (strcasecmp(parityValue,"none")==0) parity=Parity::None;
        else if (strcasecmp(parityValue,"even")==0) parity=Parity::Even;
        else if (strcasecmp(parityValue,"odd")==0) parity=Parity::Odd;
        else return false;

        definition.interfaceType=Interface::RS232;
        definition.port=static_cast<uint8_t>(port);
        definition.baudRate=static_cast<uint32_t>(baud);
        definition.dataBits=static_cast<uint8_t>(dataBits);
        definition.parity=parity;
        definition.stopBits=static_cast<uint8_t>(stopBits);
        return true;
    }

    if (strcmp(interfaceName,"TCP")==0) {
        ConfigurationFile::ValueRequest requests[]={
            {"host",definition.host,sizeof(definition.host),true,false},
            {"port",tcpPortText,sizeof(tcpPortText),true,false}
        };
        if (!m_configurationFile.getValues(
                ConfigPath,section,requests,sizeof(requests)/sizeof(requests[0])))
            return false;

        long tcpPort=0;
        if (!parseLong(tcpPortText,1,65535,tcpPort)) return false;

        definition.interfaceType=Interface::TCP;
        definition.tcpPort=static_cast<uint16_t>(tcpPort);
        return true;
    }

    return false;
}

const char* InstrumentConfiguration::interfaceText(Interface interfaceType) {
    switch(interfaceType) {
        case Interface::GPIB: return "GPIB";
        case Interface::RS232: return "RS232";
        case Interface::TCP: return "TCP";
    }
    return "unknown";
}

const char* InstrumentConfiguration::parityText(Parity parity) {
    switch(parity) {
        case Parity::None: return "none";
        case Parity::Even: return "even";
        case Parity::Odd: return "odd";
    }
    return "unknown";
}
}
