/******************************************************************************
 * File: Version.h
 * Project: AK3Y Instrument Controller
 * Purpose: Build and project identification constants.
 * Details: Exposes project metadata and compile-time build date/time. Firmware and hardware revisions are aliases of the central identifiers in Configuration.h.
 * Author: Bob Fontana, AK3Y
 *
 * Documentation only; firmware history is maintained in Git.
 ******************************************************************************/

#pragma once

#include <Configuration.h>

namespace AK3Y
{
    namespace Version
    {
        inline constexpr char Project[] =
            "AK3Y Instrument Controller";

        inline constexpr char Description[] =
            "Wi-Fi Instrument Controller Firmware";

        inline constexpr char Author[] =
            "Bob Fontana, AK3Y";

        inline constexpr const char* Hardware =
            Configuration::Firmware::HardwareRevision;

        inline constexpr const char* Firmware =
            Configuration::Firmware::Version;

        inline constexpr char BuildDate[] =
            __DATE__;

        inline constexpr char BuildTime[] =
            __TIME__;
    }
}