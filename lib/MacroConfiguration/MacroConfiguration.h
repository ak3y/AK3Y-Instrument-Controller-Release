/******************************************************************************
 * File: MacroConfiguration.h
 * Project: AK3Y Instrument Controller
 * Description: Parses and validates SD-card macro definitions.
 * Responsibilities: Provides macro step and parameter definitions, value overrides, and runtime substitution data to the execution engine.
 * Dependencies: SDCard and the filesystem interface.
 * Notes: Documentation-only update; implementation behavior is unchanged.
 ******************************************************************************/

#pragma once

#include <stddef.h>

namespace AK3Y
{

class SDCard;

class MacroConfiguration
{
public:
    static constexpr size_t MaxParameters = 8;
    static constexpr size_t MaxSteps = 32;
    static constexpr size_t MaxResults = 8;

    struct Parameter
    {
        char name[32];
        char type[16];
        char label[80];
        char quantity[32];
        char baseUnit[16];
        char defaultValue[64];
        char options[192];
    };

    // Optional read-only measurement fields. source names a runtime value
    // captured by QUERY ... AS; unit is display metadata, not conversion.
    struct ResultField
    {
        char name[32];
        char label[80];
        char source[32];
        char unit[16];
    };

    struct Macro
    {
        char name[80];
        char description[160];

        Parameter parameters[MaxParameters];
        size_t parameterCount;

        ResultField results[MaxResults];
        size_t resultCount;

        char steps[MaxSteps][224];
        size_t stepCount;
    };

    struct ValueOverride
    {
        const char* name;
        const char* value;
    };

    // Values created while a macro is executing (for example by
    // QUERY ... AS ...).  These are distinct from input parameters.
    struct RuntimeValue
    {
        const char* name;
        const char* value;
    };

    enum class Result
    {
        Success,
        InvalidArgument,
        OpenFailed,
        InvalidSyntax,
        MissingMacroSection,
        MissingMacroName,
        InvalidParameter,
        TooManyParameters,
        InvalidResult,
        TooManyResults,
        InvalidStep,
        TooManySteps,
        UnknownParameter,
        InvalidParameterValue,
        ExpansionOverflow
    };

    explicit MacroConfiguration(SDCard& sdCard);

    Result load(const char* fileName, Macro& macro);

    // Expand {parameter} tokens using the parameter defaults.
    // This performs no instrument I/O.
    Result expandStepDefaults(
        const Macro& macro,
        size_t stepIndex,
        char* expanded,
        size_t expandedSize) const;

    // Expand {parameter} tokens using caller-supplied overrides where
    // present, otherwise use the parameter defaults.
    Result expandStep(
        const Macro& macro,
        size_t stepIndex,
        const ValueOverride* overrides,
        size_t overrideCount,
        char* expanded,
        size_t expandedSize) const;

    // Expand both declared input parameters and runtime values.  Runtime
    // expressions support {name}, {name + constant}, and {name - constant}.
    Result expandStep(
        const Macro& macro,
        size_t stepIndex,
        const ValueOverride* overrides,
        size_t overrideCount,
        const RuntimeValue* runtimeValues,
        size_t runtimeValueCount,
        char* expanded,
        size_t expandedSize) const;

    static const char* resultText(Result result);

private:
    SDCard& m_sdCard;

    static char* trim(char* text);
    static bool copyText(char* destination, size_t destinationSize,
                         const char* source);
    static const Parameter* findParameter(
        const Macro& macro,
        const char* name);
    static Result normalizedValue(
        const Parameter& parameter,
        const char* sourceValue,
        char* value,
        size_t valueSize);
};

}   // namespace AK3Y
