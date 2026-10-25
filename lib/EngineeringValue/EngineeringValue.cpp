/******************************************************************************
 * File: EngineeringValue.cpp
 * Project: AK3Y Instrument Controller
 *
 * Description:
 *   Parsing and formatting of numeric engineering values.
 *
 * Responsibilities:
 *   Interprets values with supported engineering units; converts to and from base units; formats values for instrument controls. Bare numbers are interpreted in the declared base unit.
 *
 * Dependencies:
 *   Standard C/C++ numeric and string utilities; no hardware dependency.
 *
 * Author: Bob Fontana, AK3Y
 ******************************************************************************/

#include "EngineeringValue.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace AK3Y
{

namespace
{

const EngineeringValue::UnitDefinition FrequencyUnits[] =
{
    { "Hz",  1.0 },
    { "kHz", 1.0e3 },
    { "MHz", 1.0e6 },
    { "GHz", 1.0e9 }
};

const EngineeringValue::UnitDefinition TimeUnits[] =
{
    { "ps", 1.0e-12 },
    { "ns", 1.0e-9  },
    { "us", 1.0e-6  },
    { "ms", 1.0e-3  },
    { "s",  1.0     }
};

const EngineeringValue::UnitDefinition VoltageUnits[] =
{
    { "uV", 1.0e-6 },
    { "mV", 1.0e-3 },
    { "V",  1.0    },
    { "kV", 1.0e3  }
};

const EngineeringValue::UnitDefinition CurrentUnits[] =
{
    { "uA", 1.0e-6 },
    { "mA", 1.0e-3 },
    { "A",  1.0    }
};

const EngineeringValue::UnitDefinition PowerUnits[] =
{
    { "uW", 1.0e-6 },
    { "mW", 1.0e-3 },
    { "W",  1.0    },
    { "kW", 1.0e3  }
};

//
// Logarithmic power level. dBm is used directly as the profile base unit.
//
const EngineeringValue::UnitDefinition PowerLevelUnits[] =
{
    { "dBm", 1.0 }
};

//
// Dimensionless logarithmic quantities expressed in dB.
//
const EngineeringValue::UnitDefinition AttenuationUnits[] =
{
    { "dB", 1.0 }
};

const EngineeringValue::UnitDefinition AmplitudeScaleUnits[] =
{
    { "dB", 1.0 }
};

//
// Dimensionless integer/count quantity.  The parser/formatter remains generic;
// profile limits determine the allowed range.
//
const EngineeringValue::UnitDefinition CountUnits[] =
{
    { "count", 1.0 }
};

//
// Dimensionless percentage. Percent is used directly as the profile base unit.
//
const EngineeringValue::UnitDefinition PercentUnits[] =
{
    { "%", 1.0 }
};

const EngineeringValue::UnitDefinition ResistanceUnits[] =
{
    { "ohm",  1.0   },
    { "kohm", 1.0e3 },
    { "Mohm", 1.0e6 }
};

bool equals(const char* a, const char* b)
{
    return a != nullptr &&
           b != nullptr &&
           strcmp(a, b) == 0;
}

} // anonymous namespace

EngineeringValue::Result EngineeringValue::parse(
    const char* text,
    const char* quantity,
    const char* baseUnit,
    double& baseValue)
{
    if (text == nullptr ||
        quantity == nullptr ||
        baseUnit == nullptr)
    {
        return Result::InvalidArgument;
    }

    while (isspace(static_cast<unsigned char>(*text)))
        ++text;

    char* numberEnd = nullptr;
    const double numericValue = strtod(text, &numberEnd);

    if (numberEnd == text || !isfinite(numericValue))
        return Result::InvalidNumber;

    while (isspace(static_cast<unsigned char>(*numberEnd)))
        ++numberEnd;

    const char* unitStart = numberEnd;
    const char* unitEnd = unitStart + strlen(unitStart);

    while (unitEnd > unitStart &&
           isspace(static_cast<unsigned char>(unitEnd[-1])))
    {
        --unitEnd;
    }

    char enteredUnit[24];
    const size_t unitLength =
        static_cast<size_t>(unitEnd - unitStart);

    if (unitLength >= sizeof(enteredUnit))
        return Result::InvalidUnit;

    memcpy(enteredUnit, unitStart, unitLength);
    enteredUnit[unitLength] = '\0';

    size_t count = 0;
    const UnitDefinition* units =
        unitTable(quantity, baseUnit, count);

    if (units == nullptr || count == 0)
        return Result::UnsupportedQuantity;

    //
    // No unit means the profile's declared base unit.
    //

    if (enteredUnit[0] == '\0')
    {
        baseValue = numericValue;
        return Result::Success;
    }

    for (size_t i = 0; i < count; ++i)
    {
        if (unitMatches(
                enteredUnit,
                units[i].symbol,
                quantity))
        {
            baseValue = numericValue * units[i].multiplier;
            return Result::Success;
        }
    }

    return Result::InvalidUnit;
}

EngineeringValue::Result EngineeringValue::format(
    double baseValue,
    const char* quantity,
    const char* baseUnit,
    char* output,
    size_t outputSize)
{
    if (quantity == nullptr ||
        baseUnit == nullptr ||
        output == nullptr ||
        outputSize == 0 ||
        !isfinite(baseValue))
    {
        return Result::InvalidArgument;
    }

    size_t count = 0;
    const UnitDefinition* units =
        unitTable(quantity, baseUnit, count);

    if (units == nullptr || count == 0)
        return Result::UnsupportedQuantity;

    //
    // Select the largest unit for which the magnitude is at least 1.
    // Zero remains in the base unit.
    //

    size_t selected = 0;
    const double magnitude = fabs(baseValue);

    if (magnitude > 0.0)
    {
        for (size_t i = 0; i < count; ++i)
        {
            const double scaled =
                magnitude / units[i].multiplier;

            if (scaled >= 1.0)
                selected = i;
        }
    }

    const double displayedValue =
        baseValue / units[selected].multiplier;

    const int written =
        snprintf(
            output,
            outputSize,
            "%.12g %s",
            displayedValue,
            units[selected].symbol);

    if (written < 0 ||
        static_cast<size_t>(written) >= outputSize)
    {
        return Result::BufferTooSmall;
    }

    return Result::Success;
}

const char* EngineeringValue::resultText(Result result)
{
    switch (result)
    {
        case Result::Success:
            return "Success";

        case Result::InvalidArgument:
            return "Invalid argument";

        case Result::InvalidNumber:
            return "Invalid number";

        case Result::UnsupportedQuantity:
            return "Unsupported quantity";

        case Result::InvalidUnit:
            return "Invalid unit";

        case Result::BufferTooSmall:
            return "Buffer too small";

        default:
            return "Unknown result";
    }
}

const EngineeringValue::UnitDefinition*
EngineeringValue::unitTable(
    const char* quantity,
    const char* baseUnit,
    size_t& count)
{
    count = 0;

    if (equals(quantity, "frequency") &&
        equals(baseUnit, "Hz"))
    {
        count = sizeof(FrequencyUnits) /
                sizeof(FrequencyUnits[0]);
        return FrequencyUnits;
    }

    if (equals(quantity, "time") &&
        equals(baseUnit, "s"))
    {
        count = sizeof(TimeUnits) /
                sizeof(TimeUnits[0]);
        return TimeUnits;
    }

    if (equals(quantity, "voltage") &&
        equals(baseUnit, "V"))
    {
        count = sizeof(VoltageUnits) /
                sizeof(VoltageUnits[0]);
        return VoltageUnits;
    }

    if (equals(quantity, "current") &&
        equals(baseUnit, "A"))
    {
        count = sizeof(CurrentUnits) /
                sizeof(CurrentUnits[0]);
        return CurrentUnits;
    }

    if (equals(quantity, "power") &&
        equals(baseUnit, "W"))
    {
        count = sizeof(PowerUnits) /
                sizeof(PowerUnits[0]);
        return PowerUnits;
    }

    if (equals(quantity, "power_level") &&
        equals(baseUnit, "dBm"))
    {
        count = sizeof(PowerLevelUnits) /
                sizeof(PowerLevelUnits[0]);
        return PowerLevelUnits;
    }

    if (equals(quantity, "attenuation") &&
        equals(baseUnit, "dB"))
    {
        count = sizeof(AttenuationUnits) /
                sizeof(AttenuationUnits[0]);
        return AttenuationUnits;
    }

    if (equals(quantity, "amplitude_scale") &&
        equals(baseUnit, "dB"))
    {
        count = sizeof(AmplitudeScaleUnits) /
                sizeof(AmplitudeScaleUnits[0]);
        return AmplitudeScaleUnits;
    }

    if (equals(quantity, "count") &&
        equals(baseUnit, "count"))
    {
        count = sizeof(CountUnits) /
                sizeof(CountUnits[0]);
        return CountUnits;
    }

    if (equals(quantity, "percent") &&
        equals(baseUnit, "%"))
    {
        count = sizeof(PercentUnits) /
                sizeof(PercentUnits[0]);
        return PercentUnits;
    }

    if (equals(quantity, "resistance") &&
        equals(baseUnit, "ohm"))
    {
        count = sizeof(ResistanceUnits) /
                sizeof(ResistanceUnits[0]);
        return ResistanceUnits;
    }

    return nullptr;
}

bool EngineeringValue::unitMatches(
    const char* enteredUnit,
    const char* definedUnit,
    const char* quantity)
{
    if (strcmp(enteredUnit, definedUnit) == 0)
        return true;

    //
    // Frequency entry is deliberately case-insensitive.  In this controller
    // the supported frequency units are only Hz, kHz, MHz, and GHz, so the
    // user's intent is unambiguous for common spellings such as KHz, khz,
    // MHZ, ghz, etc.  Formatting remains canonical.
    //
    if (quantity != nullptr &&
        strcmp(quantity, "frequency") == 0)
    {
        const size_t enteredLength = strlen(enteredUnit);
        const size_t definedLength = strlen(definedUnit);

        if (enteredLength == definedLength)
        {
            bool same = true;

            for (size_t i = 0; i < enteredLength; ++i)
            {
                const unsigned char entered =
                    static_cast<unsigned char>(enteredUnit[i]);
                const unsigned char defined =
                    static_cast<unsigned char>(definedUnit[i]);

                if (tolower(entered) != tolower(defined))
                {
                    same = false;
                    break;
                }
            }

            if (same)
                return true;
        }
    }

    //
    // For time only, allow the unit letter S as an alias for s:
    // ps/pS, ns/nS, us/uS, ms/mS, s/S.
    // Prefix case remains significant.
    //

    if (isTimeQuantity(quantity))
    {
        const size_t enteredLength = strlen(enteredUnit);
        const size_t definedLength = strlen(definedUnit);

        if (enteredLength == definedLength &&
            enteredLength > 0)
        {
            for (size_t i = 0; i + 1 < enteredLength; ++i)
            {
                if (enteredUnit[i] != definedUnit[i])
                    return false;
            }

            return definedUnit[definedLength - 1] == 's' &&
                   enteredUnit[enteredLength - 1] == 'S';
        }
    }

    return false;
}

bool EngineeringValue::isTimeQuantity(const char* quantity)
{
    return quantity != nullptr &&
           strcmp(quantity, "time") == 0;
}

}   // namespace AK3Y
