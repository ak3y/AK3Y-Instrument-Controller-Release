/******************************************************************************
 * File: EngineeringValue.h
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

#pragma once

#include <stddef.h>

namespace AK3Y
{

class EngineeringValue
{
public:
    enum class Result
    {
        Success = 0,
        InvalidArgument,
        InvalidNumber,
        UnsupportedQuantity,
        InvalidUnit,
        BufferTooSmall
    };

    static Result parse(
        const char* text,
        const char* quantity,
        const char* baseUnit,
        double& baseValue);

    static Result format(
        double baseValue,
        const char* quantity,
        const char* baseUnit,
        char* output,
        size_t outputSize);

    static const char* resultText(Result result);

public:
    struct UnitDefinition
    {
        const char* symbol;
        double multiplier;
    };

private:
    static const UnitDefinition* unitTable(
        const char* quantity,
        const char* baseUnit,
        size_t& count);

    static bool unitMatches(
        const char* enteredUnit,
        const char* definedUnit,
        const char* quantity);

    static bool isTimeQuantity(const char* quantity);
};

}   // namespace AK3Y
