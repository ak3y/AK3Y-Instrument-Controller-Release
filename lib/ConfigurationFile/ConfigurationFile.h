/******************************************************************************
 * File: ConfigurationFile.h
 * Project: AK3Y Instrument Controller
 * Description: Declares the read-only INI-style configuration-file reader.
 * Responsibilities: Exposes single-key and batched-key retrieval with required-field reporting.
 * Author: Bob Fontana, AK3Y
 ******************************************************************************/

#pragma once

#include <stddef.h>

namespace AK3Y {

class SDCard;

class ConfigurationFile
{
public:
    struct ValueRequest
    {
        const char* key;
        char* value;
        size_t valueSize;
        bool required;
        bool found;
    };

    explicit ConfigurationFile(SDCard& sdCard);

    bool getValue(
        const char* fileName,
        const char* section,
        const char* key,
        char* value,
        size_t valueSize);

    /*
     * Read several keys from one INI section with a single file open/scan.
     *
     * Each destination is cleared before the scan.  On return, request.found
     * indicates whether that key was present.  The function returns false for
     * an invalid request, file/open error, oversized value, or a missing key
     * marked required.
     */
    bool getValues(
        const char* fileName,
        const char* section,
        ValueRequest* requests,
        size_t requestCount);

private:
    SDCard& m_sdCard;

    static char* trim(char* text);
};

}
