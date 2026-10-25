/******************************************************************************
 * File: ConfigurationFile.cpp
 * Project: AK3Y Instrument Controller
 * Description: Implements read-only retrieval of INI-style configuration values.
 * Responsibilities: Uses SDCard file access; trims fields and supports single or batched key lookups.
 * Author: Bob Fontana, AK3Y
 ******************************************************************************/

#include "ConfigurationFile.h"
#include "SDCard.h"

#include <FS.h>
#include <ctype.h>
#include <string.h>

namespace AK3Y {

ConfigurationFile::ConfigurationFile(SDCard& sdCard)
    : m_sdCard(sdCard)
{
}

char* ConfigurationFile::trim(char* text)
{
    if (text == nullptr) {
        return text;
    }

    while (*text != '\0' && isspace(static_cast<unsigned char>(*text))) {
        ++text;
    }

    char* end = text + strlen(text);

    while (end > text &&
           isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }

    *end = '\0';
    return text;
}

bool ConfigurationFile::getValue(
    const char* fileName,
    const char* section,
    const char* key,
    char* value,
    size_t valueSize)
{
    ValueRequest request = {
        key,
        value,
        valueSize,
        true,
        false
    };

    return getValues(
        fileName,
        section,
        &request,
        1);
}

bool ConfigurationFile::getValues(
    const char* fileName,
    const char* section,
    ValueRequest* requests,
    size_t requestCount)
{
    if (fileName == nullptr ||
        section == nullptr ||
        requests == nullptr ||
        requestCount == 0) {
        return false;
    }

    for (size_t i = 0; i < requestCount; ++i) {
        if (requests[i].key == nullptr ||
            requests[i].value == nullptr ||
            requests[i].valueSize == 0) {
            return false;
        }

        requests[i].value[0] = '\0';
        requests[i].found = false;
    }

    File file = m_sdCard.open(fileName);

    if (!file) {
        return false;
    }

    bool inRequestedSection = false;
    bool sectionSeen = false;
    char line[160];

    while (file.available()) {
        size_t length = file.readBytesUntil(
            '\n',
            line,
            sizeof(line) - 1);

        line[length] = '\0';

        char* text = trim(line);

        if (*text == '\0' || *text == '#' || *text == ';') {
            continue;
        }

        if (*text == '[') {
            char* closeBracket = strchr(text + 1, ']');

            if (closeBracket == nullptr) {
                inRequestedSection = false;
                continue;
            }

            *closeBracket = '\0';
            char* sectionName = trim(text + 1);

            const bool isRequestedSection =
                (strcmp(sectionName, section) == 0);

            if (inRequestedSection && !isRequestedSection) {
                break;
            }

            inRequestedSection = isRequestedSection;

            if (inRequestedSection) {
                sectionSeen = true;
            }

            continue;
        }

        if (!inRequestedSection) {
            continue;
        }

        char* equals = strchr(text, '=');

        if (equals == nullptr) {
            continue;
        }

        *equals = '\0';

        char* candidateKey = trim(text);
        char* candidateValue = trim(equals + 1);

        for (size_t i = 0; i < requestCount; ++i) {
            if (requests[i].found ||
                strcmp(candidateKey, requests[i].key) != 0) {
                continue;
            }

            if (strlen(candidateValue) >= requests[i].valueSize) {
                file.close();
                return false;
            }

            strcpy(requests[i].value, candidateValue);
            requests[i].found = true;
            break;
        }
    }

    file.close();

    if (!sectionSeen) {
        return false;
    }

    for (size_t i = 0; i < requestCount; ++i) {
        if (requests[i].required && !requests[i].found) {
            return false;
        }
    }

    return true;
}

}
