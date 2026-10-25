/******************************************************************************
 * File: MacroConfiguration.cpp
 * Project: AK3Y Instrument Controller
 * Description: Parses and validates SD-card macro definitions.
 * Responsibilities: Provides macro step and parameter definitions, value overrides, and runtime substitution data to the execution engine.
 * Dependencies: SDCard and the filesystem interface.
 * Notes: Documentation-only update; implementation behavior is unchanged.
 ******************************************************************************/

#include "MacroConfiguration.h"
#include "SDCard.h"

#include <FS.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

namespace AK3Y
{

MacroConfiguration::MacroConfiguration(SDCard& sdCard)
    : m_sdCard(sdCard)
{
}

char* MacroConfiguration::trim(char* text)
{
    if (text == nullptr)
        return text;

    while (*text != '\0' &&
           isspace(static_cast<unsigned char>(*text)))
        ++text;

    char* end = text + strlen(text);

    while (end > text &&
           isspace(static_cast<unsigned char>(*(end - 1))))
        --end;

    *end = '\0';
    return text;
}

bool MacroConfiguration::copyText(
    char* destination,
    size_t destinationSize,
    const char* source)
{
    if (destination == nullptr ||
        destinationSize == 0 ||
        source == nullptr)
        return false;

    const int written =
        snprintf(destination, destinationSize, "%s", source);

    return written >= 0 &&
           static_cast<size_t>(written) < destinationSize;
}

MacroConfiguration::Result MacroConfiguration::load(
    const char* fileName,
    Macro& macro)
{
    memset(&macro, 0, sizeof(macro));

    if (fileName == nullptr || *fileName == '\0')
        return Result::InvalidArgument;

    // the macro filename is the authoritative macro name.
    // Strip the directory and the .cfg extension for display purposes.
    const char* baseName = strrchr(fileName, '/');
    baseName = (baseName != nullptr) ? baseName + 1 : fileName;

    const size_t baseLength = strlen(baseName);
    if (baseLength <= 4 || strcmp(baseName + baseLength - 4, ".cfg") != 0)
        return Result::InvalidArgument;

    char fileMacroName[80];
    const size_t nameLength = baseLength - 4;
    if (nameLength == 0 || nameLength >= sizeof(fileMacroName))
        return Result::InvalidArgument;

    memcpy(fileMacroName, baseName, nameLength);
    fileMacroName[nameLength] = '\0';

    if (!copyText(macro.name, sizeof(macro.name), fileMacroName))
        return Result::InvalidArgument;

    File file = m_sdCard.open(fileName);

    if (!file)
        return Result::OpenFailed;

    enum class Section
    {
        None,
        Macro,
        Parameter,
        ResultField,
        Steps
    };

    Section section = Section::None;
    Parameter* parameter = nullptr;
    ResultField* resultField = nullptr;
    bool macroSectionSeen = false;

    char line[256];

    while (file.available())
    {
        const size_t length =
            file.readBytesUntil('\n', line, sizeof(line) - 1);

        line[length] = '\0';

        char* text = trim(line);

        if (*text == '\0' || *text == '#' || *text == ';')
            continue;

        if (*text == '[')
        {
            char* closeBracket = strchr(text + 1, ']');

            if (closeBracket == nullptr)
            {
                file.close();
                return Result::InvalidSyntax;
            }

            *closeBracket = '\0';
            char* sectionName = trim(text + 1);

            section = Section::None;
            parameter = nullptr;
            resultField = nullptr;

            if (strcmp(sectionName, "macro") == 0)
            {
                section = Section::Macro;
                macroSectionSeen = true;
            }
            else if (strcmp(sectionName, "steps") == 0)
            {
                section = Section::Steps;
            }
            else if (strncmp(sectionName, "result:", 7) == 0)
            {
                char* resultName = trim(sectionName + 7);
                size_t n = strlen(resultName);
                if (n == 0 || n >= sizeof(macro.results[0].name) ||
                    !(isalpha(static_cast<unsigned char>(resultName[0])) || resultName[0] == '_'))
                {
                    file.close();
                    return Result::InvalidResult;
                }
                for (size_t j = 1; j < n; ++j)
                    if (!(isalnum(static_cast<unsigned char>(resultName[j])) || resultName[j] == '_'))
                    {
                        file.close();
                        return Result::InvalidResult;
                    }
                if (macro.resultCount >= MaxResults)
                {
                    file.close();
                    return Result::TooManyResults;
                }
                for (size_t j = 0; j < macro.resultCount; ++j)
                    if (strcmp(macro.results[j].name, resultName) == 0)
                    {
                        file.close();
                        return Result::InvalidResult;
                    }
                resultField = &macro.results[macro.resultCount++];
                memset(resultField, 0, sizeof(*resultField));
                copyText(resultField->name, sizeof(resultField->name), resultName);
                section = Section::ResultField;
            }
            else if (strncmp(sectionName, "parameter:", 10) == 0)
            {
                char* parameterName = trim(sectionName + 10);

                if (*parameterName == '\0')
                {
                    file.close();
                    return Result::InvalidParameter;
                }

                if (macro.parameterCount >= MaxParameters)
                {
                    file.close();
                    return Result::TooManyParameters;
                }

                parameter =
                    &macro.parameters[macro.parameterCount++];

                memset(parameter, 0, sizeof(*parameter));

                if (!copyText(
                        parameter->name,
                        sizeof(parameter->name),
                        parameterName))
                {
                    file.close();
                    return Result::InvalidParameter;
                }

                section = Section::Parameter;
            }
            else
            {
                file.close();
                return Result::InvalidSyntax;
            }

            continue;
        }

        char* equals = strchr(text, '=');

        if (equals == nullptr)
        {
            file.close();
            return Result::InvalidSyntax;
        }

        *equals = '\0';

        char* key = trim(text);
        char* value = trim(equals + 1);

        if (*key == '\0')
        {
            file.close();
            return Result::InvalidSyntax;
        }

        if (section == Section::Macro)
        {
            bool ok = true;

            // The filename now supplies macro.name.  Accept a legacy
            // name= entry for backward compatibility, but deliberately
            // ignore its value so it cannot disagree with the filename.
            if (strcmp(key, "name") == 0)
                ok = true;
            else if (strcmp(key, "description") == 0)
                ok = copyText(
                    macro.description,
                    sizeof(macro.description),
                    value);
            else
                ok = false;

            if (!ok)
            {
                file.close();
                return Result::InvalidSyntax;
            }
        }
        else if (section == Section::Parameter)
        {
            if (parameter == nullptr)
            {
                file.close();
                return Result::InvalidParameter;
            }

            bool ok = true;

            if (strcmp(key, "type") == 0)
                ok = copyText(
                    parameter->type,
                    sizeof(parameter->type),
                    value);
            else if (strcmp(key, "label") == 0)
                ok = copyText(
                    parameter->label,
                    sizeof(parameter->label),
                    value);
            else if (strcmp(key, "quantity") == 0)
                ok = copyText(
                    parameter->quantity,
                    sizeof(parameter->quantity),
                    value);
            else if (strcmp(key, "base_unit") == 0)
                ok = copyText(
                    parameter->baseUnit,
                    sizeof(parameter->baseUnit),
                    value);
            else if (strcmp(key, "default") == 0)
                ok = copyText(
                    parameter->defaultValue,
                    sizeof(parameter->defaultValue),
                    value);
            else if (strcmp(key, "options") == 0)
                ok = copyText(
                    parameter->options,
                    sizeof(parameter->options),
                    value);
            else
                ok = false;

            if (!ok)
            {
                file.close();
                return Result::InvalidParameter;
            }
        }
        else if (section == Section::ResultField)
        {
            if (resultField == nullptr)
            {
                file.close();
                return Result::InvalidResult;
            }
            bool ok = false;
            if (strcmp(key, "label") == 0)
                ok = copyText(resultField->label, sizeof(resultField->label), value);
            else if (strcmp(key, "source") == 0)
                ok = copyText(resultField->source, sizeof(resultField->source), value);
            else if (strcmp(key, "unit") == 0)
                ok = copyText(resultField->unit, sizeof(resultField->unit), value);
            if (!ok)
            {
                file.close();
                return Result::InvalidResult;
            }
        }
        else if (section == Section::Steps)
        {
            char* end = nullptr;
            const long stepNumber = strtol(key, &end, 10);

            if (end == key ||
                *end != '\0' ||
                stepNumber != static_cast<long>(macro.stepCount + 1) ||
                *value == '\0')
            {
                file.close();
                return Result::InvalidStep;
            }

            if (macro.stepCount >= MaxSteps)
            {
                file.close();
                return Result::TooManySteps;
            }

            if (!copyText(
                    macro.steps[macro.stepCount],
                    sizeof(macro.steps[macro.stepCount]),
                    value))
            {
                file.close();
                return Result::InvalidStep;
            }

            ++macro.stepCount;
        }
        else
        {
            file.close();
            return Result::InvalidSyntax;
        }
    }

    file.close();

    if (!macroSectionSeen)
        return Result::MissingMacroSection;

    for (size_t i = 0; i < macro.parameterCount; ++i)
    {
        const Parameter& p = macro.parameters[i];

        if (p.type[0] == '\0' ||
            p.label[0] == '\0')
            return Result::InvalidParameter;

        if (strcmp(p.type, "number") == 0)
        {
            if (p.quantity[0] == '\0' ||
                p.baseUnit[0] == '\0')
                return Result::InvalidParameter;
        }
        else if (strcmp(p.type, "boolean") == 0)
        {
            // No additional fields are required.
        }
        else if (strcmp(p.type, "select") == 0)
        {
            if (p.options[0] == '\0')
                return Result::InvalidParameter;
        }
        else
        {
            return Result::InvalidParameter;
        }
    }

    for (size_t i = 0; i < macro.resultCount; ++i)
    {
        const ResultField& r = macro.results[i];
        if (!r.label[0] || !r.source[0] ||
            !(isalpha(static_cast<unsigned char>(r.source[0])) || r.source[0] == '_'))
            return Result::InvalidResult;
        for (const char* p = r.source + 1; *p; ++p)
            if (!(isalnum(static_cast<unsigned char>(*p)) || *p == '_'))
                return Result::InvalidResult;
    }

    if (macro.stepCount == 0)
        return Result::InvalidStep;

    return Result::Success;
}

const MacroConfiguration::Parameter* MacroConfiguration::findParameter(
    const Macro& macro,
    const char* name)
{
    for (size_t i = 0; i < macro.parameterCount; ++i)
    {
        if (strcmp(macro.parameters[i].name, name) == 0)
            return &macro.parameters[i];
    }

    return nullptr;
}

MacroConfiguration::Result MacroConfiguration::normalizedValue(
    const Parameter& parameter,
    const char* sourceValue,
    char* value,
    size_t valueSize)
{
    if (sourceValue == nullptr || *sourceValue == '\0' ||
        value == nullptr || valueSize == 0)
        return Result::InvalidArgument;

    value[0] = '\0';


    if (strcmp(parameter.type, "boolean") == 0)
    {
        if (strcasecmp(sourceValue, "on") == 0 ||
            strcasecmp(sourceValue, "true") == 0 ||
            strcmp(sourceValue, "1") == 0)
            return copyText(value, valueSize, "1")
                ? Result::Success : Result::ExpansionOverflow;

        if (strcasecmp(sourceValue, "off") == 0 ||
            strcasecmp(sourceValue, "false") == 0 ||
            strcmp(sourceValue, "0") == 0)
            return copyText(value, valueSize, "0")
                ? Result::Success : Result::ExpansionOverflow;

        return Result::InvalidParameterValue;
    }

    if (strcmp(parameter.type, "select") == 0)
    {
        // A select default may already be the instrument value, or may be
        // the display label from an options list such as Off:0,On:1.
        char options[sizeof(parameter.options)];

        if (!copyText(options, sizeof(options), parameter.options))
            return Result::InvalidParameterValue;

        char* item = strtok(options, ",");

        while (item != nullptr)
        {
            char* colon = strchr(item, ':');

            if (colon != nullptr)
            {
                *colon = '\0';
                char* label = trim(item);
                char* optionValue = trim(colon + 1);

                if (strcasecmp(sourceValue, label) == 0 ||
                    strcmp(sourceValue, optionValue) == 0)
                    return copyText(value, valueSize, optionValue)
                        ? Result::Success : Result::ExpansionOverflow;
            }

            item = strtok(nullptr, ",");
        }

        return Result::InvalidParameterValue;
    }

    if (strcmp(parameter.type, "number") == 0)
    {
        char* end = nullptr;
        const double numeric = strtod(sourceValue, &end);

        if (end == sourceValue || !isfinite(numeric))
            return Result::InvalidParameterValue;

        while (*end != '\0' &&
               isspace(static_cast<unsigned char>(*end)))
            ++end;

        double multiplier = 1.0;

        if (*end != '\0')
        {
            // Engineering prefixes are case-sensitive where necessary:
            // m = milli, k/K = kilo, M = mega, G = giga.
            if (*end == 'm')
                multiplier = 1.0e-3;
            else if (*end == 'k' || *end == 'K')
                multiplier = 1.0e3;
            else if (*end == 'M')
                multiplier = 1.0e6;
            else if (*end == 'G')
                multiplier = 1.0e9;
            else
                return Result::InvalidParameterValue;

            ++end;

            // If a unit follows the prefix, it must match base_unit.
            if (*end != '\0' &&
                strcmp(end, parameter.baseUnit) != 0)
                return Result::InvalidParameterValue;
        }

        const double baseValue = numeric * multiplier;

        const int written =
            snprintf(value, valueSize, "%.12g", baseValue);

        return (written >= 0 &&
                static_cast<size_t>(written) < valueSize)
            ? Result::Success : Result::ExpansionOverflow;
    }

    return Result::InvalidParameterValue;
}

MacroConfiguration::Result MacroConfiguration::expandStepDefaults(
    const Macro& macro,
    size_t stepIndex,
    char* expanded,
    size_t expandedSize) const
{
    return expandStep(
        macro,
        stepIndex,
        nullptr,
        0,
        expanded,
        expandedSize);
}

MacroConfiguration::Result MacroConfiguration::expandStep(
    const Macro& macro,
    size_t stepIndex,
    const ValueOverride* overrides,
    size_t overrideCount,
    char* expanded,
    size_t expandedSize) const
{
    return expandStep(
        macro, stepIndex,
        overrides, overrideCount,
        nullptr, 0,
        expanded, expandedSize);
}

MacroConfiguration::Result MacroConfiguration::expandStep(
    const Macro& macro,
    size_t stepIndex,
    const ValueOverride* overrides,
    size_t overrideCount,
    const RuntimeValue* runtimeValues,
    size_t runtimeValueCount,
    char* expanded,
    size_t expandedSize) const
{
    if (expanded == nullptr ||
        expandedSize == 0 ||
        stepIndex >= macro.stepCount ||
        (overrideCount != 0 && overrides == nullptr) ||
        (runtimeValueCount != 0 && runtimeValues == nullptr))
        return Result::InvalidArgument;

    expanded[0] = '\0';

    const char* source = macro.steps[stepIndex];
    size_t used = 0;

    while (*source != '\0')
    {
        if (*source != '{')
        {
            if (used + 1 >= expandedSize)
                return Result::ExpansionOverflow;

            expanded[used++] = *source++;
            expanded[used] = '\0';
            continue;
        }

        const char* close = strchr(source + 1, '}');

        if (close == nullptr)
            return Result::InvalidStep;

        const size_t expressionLength =
            static_cast<size_t>(close - (source + 1));

        if (expressionLength == 0 || expressionLength >= 96)
            return Result::InvalidStep;

        char expression[96];
        memcpy(expression, source + 1, expressionLength);
        expression[expressionLength] = '\0';

        char* expressionStart = trim(expression);
        char* operatorPosition = nullptr;

        // Runtime arithmetic is deliberately limited to one + or - operator
        // surrounded by optional whitespace.
        for (char* p = expressionStart + 1; *p != '\0'; ++p)
        {
            if (*p == '+' || *p == '-')
            {
                operatorPosition = p;
                break;
            }
        }

        char* name = expressionStart;
        char operation = '\0';
        char* constantText = nullptr;

        if (operatorPosition != nullptr)
        {
            operation = *operatorPosition;
            *operatorPosition = '\0';
            name = trim(name);
            constantText = trim(operatorPosition + 1);
            if (*constantText == '\0')
                return Result::InvalidStep;
        }
        else
        {
            name = trim(name);
        }

        char value[64];
        value[0] = '\0';

        const Parameter* parameter = findParameter(macro, name);

        if (parameter != nullptr && operation == '\0')
        {
            const char* sourceValue = parameter->defaultValue;

            for (size_t i = 0; i < overrideCount; ++i)
            {
                if (overrides[i].name == nullptr ||
                    overrides[i].value == nullptr)
                    return Result::InvalidArgument;

                if (strcmp(overrides[i].name, name) == 0)
                {
                    sourceValue = overrides[i].value;
                    break;
                }
            }

            const Result valueResult =
                normalizedValue(
                    *parameter,
                    sourceValue,
                    value,
                    sizeof(value));

            if (valueResult != Result::Success)
                return valueResult;
        }
        else
        {
            const char* runtimeText = nullptr;

            for (size_t i = 0; i < runtimeValueCount; ++i)
            {
                if (runtimeValues[i].name == nullptr ||
                    runtimeValues[i].value == nullptr)
                    return Result::InvalidArgument;

                if (strcmp(runtimeValues[i].name, name) == 0)
                {
                    runtimeText = runtimeValues[i].value;
                    break;
                }
            }

            if (runtimeText == nullptr)
                return Result::UnknownParameter;

            if (operation == '\0')
            {
                if (!copyText(value, sizeof(value), runtimeText))
                    return Result::ExpansionOverflow;
            }
            else
            {
                char* runtimeEnd = nullptr;
                const double runtimeNumber = strtod(runtimeText, &runtimeEnd);

                if (runtimeEnd == runtimeText || !isfinite(runtimeNumber))
                    return Result::InvalidParameterValue;

                while (*runtimeEnd != '\0' &&
                       isspace(static_cast<unsigned char>(*runtimeEnd)))
                    ++runtimeEnd;

                if (*runtimeEnd != '\0')
                    return Result::InvalidParameterValue;

                char* constantEnd = nullptr;
                double constant = strtod(constantText, &constantEnd);

                if (constantEnd == constantText || !isfinite(constant))
                    return Result::InvalidParameterValue;

                while (*constantEnd != '\0' &&
                       isspace(static_cast<unsigned char>(*constantEnd)))
                    ++constantEnd;

                double multiplier = 1.0;
                if (*constantEnd == 'm')
                {
                    multiplier = 1.0e-3;
                    ++constantEnd;
                }
                else if (*constantEnd == 'k' || *constantEnd == 'K')
                {
                    multiplier = 1.0e3;
                    ++constantEnd;
                }
                else if (*constantEnd == 'M')
                {
                    multiplier = 1.0e6;
                    ++constantEnd;
                }
                else if (*constantEnd == 'G')
                {
                    multiplier = 1.0e9;
                    ++constantEnd;
                }

                // An optional alphabetic unit (for example Hz or dB) is
                // accepted.  Dimensional checking is intentionally outside
                // the scope of the macro language.
                while (*constantEnd != '\0' &&
                       isalpha(static_cast<unsigned char>(*constantEnd)))
                    ++constantEnd;

                while (*constantEnd != '\0' &&
                       isspace(static_cast<unsigned char>(*constantEnd)))
                    ++constantEnd;

                if (*constantEnd != '\0')
                    return Result::InvalidParameterValue;

                constant *= multiplier;
                const double result =
                    (operation == '+')
                        ? runtimeNumber + constant
                        : runtimeNumber - constant;

                const int written =
                    snprintf(value, sizeof(value), "%.12g", result);

                if (written < 0 ||
                    static_cast<size_t>(written) >= sizeof(value))
                    return Result::ExpansionOverflow;
            }
        }

        const size_t valueLength = strlen(value);

        if (used + valueLength >= expandedSize)
            return Result::ExpansionOverflow;

        memcpy(expanded + used, value, valueLength);
        used += valueLength;
        expanded[used] = '\0';

        source = close + 1;
    }

    return Result::Success;
}

const char* MacroConfiguration::resultText(Result result)
{
    switch (result)
    {
        case Result::Success:
            return "Success";
        case Result::InvalidArgument:
            return "Invalid argument";
        case Result::OpenFailed:
            return "Unable to open macro file";
        case Result::InvalidSyntax:
            return "Invalid macro syntax";
        case Result::MissingMacroSection:
            return "Missing [macro] section";
        case Result::MissingMacroName:
            return "Missing macro name";
        case Result::InvalidParameter:
            return "Invalid macro parameter";
        case Result::TooManyParameters:
            return "Too many macro parameters";
        case Result::InvalidResult:
            return "Invalid macro result definition";
        case Result::TooManyResults:
            return "Too many macro results";
        case Result::InvalidStep:
            return "Invalid macro step";
        case Result::TooManySteps:
            return "Too many macro steps";
        case Result::UnknownParameter:
            return "Unknown macro parameter";
        case Result::InvalidParameterValue:
            return "Invalid macro parameter value";
        case Result::ExpansionOverflow:
            return "Expanded macro step is too long";
    }

    return "Unknown macro result";
}

}   // namespace AK3Y
