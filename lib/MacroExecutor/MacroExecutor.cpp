/******************************************************************************
 * File: MacroExecutor.cpp
 * Project: AK3Y Instrument Controller
 * Description: Executes configured instrument-control macros.
 * Responsibilities: Performs preflight validation and executes macro steps, including command dispatch, delays, captures, sweeps, logging, progress reporting, and cancellation.
 * Dependencies: MacroConfiguration, CommandExecutor, SDCard, and Console.
 * Notes: Documentation-only update; implementation behavior is unchanged.
 ******************************************************************************/

#include "MacroExecutor.h"

#include <Arduino.h>
#include <CommandExecutor.h>
#include <Console.h>
#include <MacroConfiguration.h>
#include <SDCard.h>
#include <FS.h>
#include <stdlib.h>
#include <string.h>

namespace AK3Y
{

MacroExecutor::MacroExecutor(MacroConfiguration& macroConfiguration,
                             CommandExecutor& commandExecutor,
                             SDCard& sdCard,
                             Console& console)
    : m_macroConfiguration(macroConfiguration),
      m_commandExecutor(commandExecutor),
      m_sdCard(sdCard),
      m_console(console)
{
}

MacroExecutor::Result MacroExecutor::preflight(
    const MacroConfiguration::Macro& macro,
    const MacroConfiguration::ValueOverride* overrides,
    size_t overrideCount)
{
    // Preflight runs from inside execute(), whose existing execution state
    // already consumes substantial loopTask stack. Keep the preflight runtime
    // tables in static storage so the validation pass does not duplicate those
    // large arrays on the ESP32 stack.
    static RuntimeValueStorage runtimeStorage[MaxRuntimeValues];
    static MacroConfiguration::RuntimeValue runtimeValues[MaxRuntimeValues];
    size_t runtimeValueCount=0;

    for (size_t i=0; i<MaxRuntimeValues; ++i)
    {
        runtimeStorage[i].name[0]='\0';
        runtimeStorage[i].value[0]='\0';
        runtimeValues[i].name=runtimeStorage[i].name;
        runtimeValues[i].value=runtimeStorage[i].value;
    }

    bool sweepActive=false;

    auto installPlaceholder = [&](const char* name) -> bool
    {
        size_t slot=runtimeValueCount;
        for (size_t r=0; r<runtimeValueCount; ++r)
        {
            if (strcmp(runtimeStorage[r].name,name)==0)
            {
                slot=r;
                break;
            }
        }

        if (slot==runtimeValueCount)
        {
            if (runtimeValueCount>=MaxRuntimeValues)
                return false;
            ++runtimeValueCount;
        }

        if (strlen(name)>=sizeof(runtimeStorage[slot].name))
            return false;

        strcpy(runtimeStorage[slot].name,name);
        strcpy(runtimeStorage[slot].value,"0");
        return true;
    };

    auto validVariableName = [](const char* name) -> bool
    {
        if (!name || !*name ||
            !(isalpha(static_cast<unsigned char>(*name)) || *name=='_'))
            return false;

        size_t n=0;
        for (const char* p=name; *p!='\0'; ++p)
        {
            if (!(isalnum(static_cast<unsigned char>(*p)) || *p=='_') ||
                ++n>=32)
                return false;
        }
        return true;
    };

    auto parseNumber = [](const char* text, double& value) -> bool
    {
        char* end=nullptr;
        value=strtod(text,&end);
        if (end==text)
            return false;

        double multiplier=1.0;
        if (*end!='\0')
        {
            if (strcmp(end,"k")==0 || strcmp(end,"K")==0 ||
                strcmp(end,"kHz")==0 || strcmp(end,"KHz")==0)
                multiplier=1.0e3;
            else if (strcmp(end,"M")==0 || strcmp(end,"MHz")==0)
                multiplier=1.0e6;
            else if (strcmp(end,"G")==0 || strcmp(end,"GHz")==0)
                multiplier=1.0e9;
            else if (strcmp(end,"m")==0)
                multiplier=1.0e-3;
            else
                return false;
        }

        value*=multiplier;
        return isfinite(value);
    };

    for (size_t i=0; i<macro.stepCount; ++i)
    {
        char expanded[224];
        const MacroConfiguration::Result expansionResult=
            m_macroConfiguration.expandStep(
                macro,i,overrides,overrideCount,
                runtimeValues,runtimeValueCount,
                expanded,sizeof(expanded));

        if (expansionResult!=MacroConfiguration::Result::Success)
        {
            m_console.printf("PREFLIGHT FAIL step %u expansion: %s\n",
                static_cast<unsigned int>(i+1),
                MacroConfiguration::resultText(expansionResult));
            return Result::MacroConfigurationError;
        }

        if (strncmp(expanded,"SWEEP ",6)==0)
        {
            if (sweepActive)
            {
                m_console.printf("PREFLIGHT FAIL step %u: Nested SWEEP is not supported\n",
                    static_cast<unsigned int>(i+1));
                return Result::InvalidSweep;
            }

            char text[224];
            strncpy(text,expanded+6,sizeof(text)-1);
            text[sizeof(text)-1]='\0';

            char* save=nullptr;
            char* variable=strtok_r(text," \t",&save);
            char* startText=strtok_r(nullptr," \t",&save);
            char* stopText=strtok_r(nullptr," \t",&save);
            char* incrementText=strtok_r(nullptr," \t",&save);
            char* extra=strtok_r(nullptr," \t",&save);

            double startValue=0.0, stopValue=0.0, incrementValue=0.0;
            if (!variable || !startText || !stopText || !incrementText || extra ||
                !validVariableName(variable) ||
                !parseNumber(startText,startValue) ||
                !parseNumber(stopText,stopValue) ||
                !parseNumber(incrementText,incrementValue) ||
                incrementValue==0.0 ||
                (startValue<stopValue && incrementValue<0.0) ||
                (startValue>stopValue && incrementValue>0.0))
            {
                m_console.printf("PREFLIGHT FAIL step %u: Invalid SWEEP\n",
                    static_cast<unsigned int>(i+1));
                return Result::InvalidSweep;
            }

            if (!installPlaceholder(variable))
            {
                m_console.printf("PREFLIGHT FAIL step %u: Invalid/too many runtime variables\n",
                    static_cast<unsigned int>(i+1));
                return Result::InvalidSweep;
            }

            sweepActive=true;
            continue;
        }

        if (strcmp(expanded,"ENDSWEEP")==0)
        {
            if (!sweepActive)
            {
                m_console.printf("PREFLIGHT FAIL step %u: ENDSWEEP without SWEEP\n",
                    static_cast<unsigned int>(i+1));
                return Result::InvalidSweep;
            }
            sweepActive=false;
            continue;
        }

        if (strncmp(expanded,"DELAY ",6)==0)
        {
            const char* valueText=expanded+6;
            char* end=nullptr;
            const unsigned long delayMilliseconds=strtoul(valueText,&end,10);
            while (end && *end==' ') ++end;
            if (valueText==end || !end || *end!='\0' || delayMilliseconds>60000UL)
            {
                m_console.printf("PREFLIGHT FAIL step %u: Invalid DELAY value\n",
                    static_cast<unsigned int>(i+1));
                return Result::InvalidDelay;
            }
            continue;
        }

        if (strncmp(expanded,"LOG ",4)==0)
        {
            char logCommand[224];
            strncpy(logCommand,expanded+4,sizeof(logCommand)-1);
            logCommand[sizeof(logCommand)-1]='\0';

            char* save=nullptr;
            char* path=strtok_r(logCommand," \t",&save);
            char* fieldList=save;
            while (fieldList && (*fieldList==' ' || *fieldList=='\t')) ++fieldList;

            if (!path || !fieldList || !*fieldList ||
                strncmp(path,"/logs/",6)!=0 || strstr(path,"..")!=nullptr)
            {
                m_console.printf("PREFLIGHT FAIL step %u: Invalid LOG syntax or path\n",
                    static_cast<unsigned int>(i+1));
                return Result::InvalidLog;
            }

            const size_t pathLength=strlen(path);
            if (pathLength<5 || strcmp(path+pathLength-4,".csv")!=0)
            {
                m_console.printf("PREFLIGHT FAIL step %u: LOG file must be /logs/*.csv\n",
                    static_cast<unsigned int>(i+1));
                return Result::InvalidLog;
            }

            size_t fieldCount=0;
            char* fieldSave=nullptr;
            char* field=strtok_r(fieldList,",",&fieldSave);
            while (field)
            {
                while (*field==' ' || *field=='\t') ++field;
                char* end=field+strlen(field);
                while (end>field && (end[-1]==' ' || end[-1]=='\t')) --end;
                *end='\0';

                if (!*field || ++fieldCount>MaxRuntimeValues)
                    return Result::InvalidLog;

                bool found=false;
                for (size_t r=0; r<runtimeValueCount; ++r)
                    if (strcmp(runtimeStorage[r].name,field)==0) { found=true; break; }

                if (!found)
                {
                    m_console.printf("PREFLIGHT FAIL step %u: LOG variable not captured: %s\n",
                        static_cast<unsigned int>(i+1),field);
                    return Result::InvalidLog;
                }
                field=strtok_r(nullptr,",",&fieldSave);
            }

            if (fieldCount==0)
                return Result::InvalidLog;
            continue;
        }

        char command[224];
        strncpy(command,expanded,sizeof(command)-1);
        command[sizeof(command)-1]='\0';
        char captureName[32]="";

        if (strncmp(command,"QUERY ",6)==0)
        {
            char* as=strstr(command," AS ");
            if (as)
            {
                *as='\0';
                const char* name=as+4;
                if (!validVariableName(name) || strlen(name)>=sizeof(captureName))
                {
                    m_console.printf("PREFLIGHT FAIL step %u: Invalid capture variable\n",
                        static_cast<unsigned int>(i+1));
                    return Result::InvalidCapture;
                }
                strcpy(captureName,name);
            }
        }

        const CommandExecutor::Result commandResult=m_commandExecutor.validate(command);
        if (commandResult!=CommandExecutor::Result::Success)
        {
            m_console.printf("PREFLIGHT FAIL step %u: %s\n",
                static_cast<unsigned int>(i+1),
                CommandExecutor::resultText(commandResult));
            return Result::CommandError;
        }

        if (captureName[0]!='\0' && !installPlaceholder(captureName))
        {
            m_console.printf("PREFLIGHT FAIL step %u: Too many runtime variables\n",
                static_cast<unsigned int>(i+1));
            return Result::InvalidCapture;
        }
    }

    if (sweepActive)
    {
        m_console.println("PREFLIGHT FAIL: SWEEP has no matching ENDSWEEP");
        return Result::InvalidSweep;
    }

    return Result::Success;
}

MacroExecutor::Result MacroExecutor::execute(
    const char* macroFile,
    uint32_t commandTimeoutMilliseconds)
{
    return execute(
        macroFile,
        nullptr,
        0,
        commandTimeoutMilliseconds);
}

MacroExecutor::Result MacroExecutor::execute(
    const char* macroFile,
    const MacroConfiguration::ValueOverride* overrides,
    size_t overrideCount,
    uint32_t commandTimeoutMilliseconds)
{
    return execute(
        macroFile,
        overrides,
        overrideCount,
        nullptr,
        nullptr,
        commandTimeoutMilliseconds);
}

MacroExecutor::Result MacroExecutor::execute(
    const char* macroFile,
    const MacroConfiguration::ValueOverride* overrides,
    size_t overrideCount,
    ProgressCallback progressCallback,
    void* progressContext,
    uint32_t commandTimeoutMilliseconds)
{
    return execute(
        macroFile, overrides, overrideCount,
        progressCallback, progressContext,
        nullptr, nullptr,
        commandTimeoutMilliseconds);
}

MacroExecutor::Result MacroExecutor::execute(
    const char* macroFile,
    const MacroConfiguration::ValueOverride* overrides,
    size_t overrideCount,
    ProgressCallback progressCallback,
    void* progressContext,
    CancellationCallback cancellationCallback,
    void* cancellationContext,
    uint32_t commandTimeoutMilliseconds)
{
    if (macroFile == nullptr || *macroFile == '\0')
        return Result::InvalidArgument;

    // Keep the several-kilobyte parsed macro off the ESP32 loopTask stack.
    static MacroConfiguration::Macro macro;

    const MacroConfiguration::Result loadResult =
        m_macroConfiguration.load(macroFile, macro);

    if (loadResult != MacroConfiguration::Result::Success)
    {
        m_console.printf("FAIL: macro load: %s\n",
                         MacroConfiguration::resultText(loadResult));
        return Result::MacroConfigurationError;
    }

    // validate the complete expanded macro before any
    // instrument I/O or file logging can occur.
    const Result preflightResult=preflight(macro,overrides,overrideCount);
    if (preflightResult!=Result::Success)
        return preflightResult;

    RuntimeValueStorage runtimeStorage[MaxRuntimeValues];
    MacroConfiguration::RuntimeValue runtimeValues[MaxRuntimeValues];
    size_t runtimeValueCount = 0;

    for (size_t i = 0; i < MaxRuntimeValues; ++i)
    {
        runtimeStorage[i].name[0] = '\0';
        runtimeStorage[i].value[0] = '\0';
        runtimeValues[i].name = runtimeStorage[i].name;
        runtimeValues[i].value = runtimeStorage[i].value;
    }

    // one non-nested numeric SWEEP/ENDSWEEP block.
    bool sweepActive = false;
    size_t sweepBodyStart = 0;
    size_t sweepEndStep = 0;
    size_t sweepVariableSlot = 0;
    double sweepCurrent = 0.0;
    double sweepStop = 0.0;
    double sweepIncrement = 0.0;

    for (size_t i = 0; i < macro.stepCount; )
    {
        if (progressCallback != nullptr)
            progressCallback(i, macro.stepCount, i + 1, progressContext);

        if (cancellationCallback != nullptr &&
            cancellationCallback(cancellationContext))
        {
            m_console.println("      ABORT: Macro aborted by user");
            return Result::Cancelled;
        }

        char expanded[224];

        const MacroConfiguration::Result expansionResult =
            m_macroConfiguration.expandStep(
                macro,
                i,
                overrides,
                overrideCount,
                runtimeValues,
                runtimeValueCount,
                expanded,
                sizeof(expanded));

        if (expansionResult != MacroConfiguration::Result::Success)
        {
            m_console.printf(
                "FAIL step %u expansion: %s\n",
                static_cast<unsigned int>(i + 1),
                MacroConfiguration::resultText(expansionResult));
            return Result::MacroConfigurationError;
        }

        m_console.printf("  %u > %s\n",
                         static_cast<unsigned int>(i + 1),
                         expanded);

        // controlled numeric sweep.
        // Syntax: SWEEP <runtime-variable> <start> <stop> <increment>
        if (strncmp(expanded, "SWEEP ", 6) == 0)
        {
            if (sweepActive)
            {
                m_console.println("      FAIL: Nested SWEEP is not supported");
                return Result::InvalidSweep;
            }

            char text[224];
            strncpy(text, expanded + 6, sizeof(text) - 1);
            text[sizeof(text) - 1] = '\0';

            char* save = nullptr;
            char* variable = strtok_r(text, " \t", &save);
            char* startText = strtok_r(nullptr, " \t", &save);
            char* stopText = strtok_r(nullptr, " \t", &save);
            char* incrementText = strtok_r(nullptr, " \t", &save);
            char* extra = strtok_r(nullptr, " \t", &save);

            if (variable == nullptr || startText == nullptr ||
                stopText == nullptr || incrementText == nullptr ||
                extra != nullptr)
            {
                m_console.println("      FAIL: Invalid SWEEP syntax");
                return Result::InvalidSweep;
            }

            if (!(isalpha(static_cast<unsigned char>(variable[0])) ||
                  variable[0] == '_'))
            {
                m_console.println("      FAIL: Invalid SWEEP variable");
                return Result::InvalidSweep;
            }

            size_t variableLength = 0;
            for (const char* q = variable; *q != '\0'; ++q)
            {
                if (!(isalnum(static_cast<unsigned char>(*q)) || *q == '_') ||
                    ++variableLength >= sizeof(runtimeStorage[0].name))
                {
                    m_console.println("      FAIL: Invalid SWEEP variable");
                    return Result::InvalidSweep;
                }
            }

            auto parseNumber = [](const char* s, double& value) -> bool
            {
                char* end = nullptr;
                value = strtod(s, &end);
                if (end == s)
                    return false;

                double multiplier = 1.0;
                if (*end != '\0')
                {
                    if (strcmp(end, "k") == 0 || strcmp(end, "K") == 0 ||
                        strcmp(end, "kHz") == 0 || strcmp(end, "KHz") == 0)
                        multiplier = 1.0e3;
                    else if (strcmp(end, "M") == 0 || strcmp(end, "MHz") == 0)
                        multiplier = 1.0e6;
                    else if (strcmp(end, "G") == 0 || strcmp(end, "GHz") == 0)
                        multiplier = 1.0e9;
                    else if (strcmp(end, "m") == 0)
                        multiplier = 1.0e-3;
                    else
                        return false;
                }

                value *= multiplier;
                return isfinite(value);
            };

            double startValue = 0.0, stopValue = 0.0, incrementValue = 0.0;
            if (!parseNumber(startText, startValue) ||
                !parseNumber(stopText, stopValue) ||
                !parseNumber(incrementText, incrementValue) ||
                incrementValue == 0.0 ||
                (startValue < stopValue && incrementValue < 0.0) ||
                (startValue > stopValue && incrementValue > 0.0))
            {
                m_console.println("      FAIL: Invalid SWEEP range or increment");
                return Result::InvalidSweep;
            }

            // Locate the matching ENDSWEEP.  Give expansion a temporary
            // value for the sweep variable so steps such as {SweepValue}
            // can be inspected before the real runtime variable is installed.
            size_t matchingEnd = macro.stepCount;
            MacroConfiguration::RuntimeValue scanValues[MaxRuntimeValues];
            for (size_t r = 0; r < runtimeValueCount; ++r)
                scanValues[r] = runtimeValues[r];

            char placeholderValue[] = "0";
            MacroConfiguration::RuntimeValue placeholder;
            placeholder.name = variable;
            placeholder.value = placeholderValue;

            if (runtimeValueCount >= MaxRuntimeValues)
            {
                m_console.println("      FAIL: Too many runtime variables");
                return Result::InvalidSweep;
            }
            scanValues[runtimeValueCount] = placeholder;

            for (size_t s = i + 1; s < macro.stepCount; ++s)
            {
                char scan[224];
                if (m_macroConfiguration.expandStep(
                        macro, s, overrides, overrideCount,
                        scanValues, runtimeValueCount + 1,
                        scan, sizeof(scan)) !=
                    MacroConfiguration::Result::Success)
                    continue;

                if (strncmp(scan, "SWEEP ", 6) == 0)
                {
                    m_console.println("      FAIL: Nested SWEEP is not supported");
                    return Result::InvalidSweep;
                }

                if (strcmp(scan, "ENDSWEEP") == 0)
                {
                    matchingEnd = s;
                    break;
                }
            }

            if (matchingEnd == macro.stepCount)
            {
                m_console.println("      FAIL: SWEEP has no matching ENDSWEEP");
                return Result::InvalidSweep;
            }

            size_t slot = runtimeValueCount;
            for (size_t r = 0; r < runtimeValueCount; ++r)
                if (strcmp(runtimeStorage[r].name, variable) == 0)
                {
                    slot = r;
                    break;
                }

            if (slot == runtimeValueCount)
                ++runtimeValueCount;

            strcpy(runtimeStorage[slot].name, variable);
            snprintf(runtimeStorage[slot].value,
                     sizeof(runtimeStorage[slot].value), "%.12g", startValue);

            sweepActive = true;
            sweepBodyStart = i + 1;
            sweepEndStep = matchingEnd;
            sweepVariableSlot = slot;
            sweepCurrent = startValue;
            sweepStop = stopValue;
            sweepIncrement = incrementValue;

            m_console.printf("      %s = %s\n",
                             runtimeStorage[slot].name,
                             runtimeStorage[slot].value);

            // Publish only explicitly declared result fields, never arbitrary
            // runtime values. The callback copies the value if it needs to
            // retain it after this invocation.
            if (m_resultCallback != nullptr)
            {
                for (size_t r = 0; r < macro.resultCount; ++r)
                {
                    if (strcmp(macro.results[r].source, variable) == 0)
                    {
                        m_resultCallback(macro.results[r].name,
                                         runtimeStorage[slot].value,
                                         m_resultContext);
                    }
                }
            }
            m_console.println("      PASS");
            ++i;
            continue;
        }

        if (strcmp(expanded, "ENDSWEEP") == 0)
        {
            if (!sweepActive || i != sweepEndStep)
            {
                m_console.println("      FAIL: ENDSWEEP without active SWEEP");
                return Result::InvalidSweep;
            }

            const double nextValue = sweepCurrent + sweepIncrement;
            const bool again = (sweepIncrement > 0.0) ?
                (nextValue <= sweepStop) : (nextValue >= sweepStop);

            if (again)
            {
                sweepCurrent = nextValue;
                snprintf(runtimeStorage[sweepVariableSlot].value,
                         sizeof(runtimeStorage[sweepVariableSlot].value),
                         "%.12g", sweepCurrent);
                m_console.printf("      next %s = %s\n",
                                 runtimeStorage[sweepVariableSlot].name,
                                 runtimeStorage[sweepVariableSlot].value);
                m_console.println("      PASS");
                i = sweepBodyStart;
                continue;
            }

            sweepActive = false;
            m_console.println("      PASS");
            ++i;
            continue;
        }

        // DELAY is a macro-control operation, not an instrument command.
        if (strncmp(expanded, "DELAY ", 6) == 0)
        {
            const char* valueText = expanded + 6;
            char* end = nullptr;
            const unsigned long delayMilliseconds =
                strtoul(valueText, &end, 10);

            while (end != nullptr && *end == ' ')
                ++end;

            if (valueText == end || end == nullptr ||
                *end != '\0' || delayMilliseconds > 60000UL)
            {
                m_console.println("      FAIL: Invalid DELAY value");
                return Result::InvalidDelay;
            }

            m_console.printf("      wait %lu ms\n", delayMilliseconds);

            const uint32_t delayStart = millis();
            while (static_cast<uint32_t>(millis() - delayStart) <
                   static_cast<uint32_t>(delayMilliseconds))
            {
                if (progressCallback != nullptr)
                    progressCallback(i, macro.stepCount, i + 1, progressContext);

                if (cancellationCallback != nullptr &&
                    cancellationCallback(cancellationContext))
                {
                    m_console.println("      ABORT: Macro aborted by user");
                    return Result::Cancelled;
                }

                delay(25);
            }

            m_console.println("      PASS");

            if (progressCallback != nullptr)
                progressCallback(
                    i + 1,
                    macro.stepCount,
                    (i + 1 < macro.stepCount) ? i + 2 : 0,
                    progressContext);

            ++i;
            continue;
        }

        // append selected runtime values to a CSV file.
        // Syntax: LOG /logs/file.csv Variable1,Variable2,...
        if (strncmp(expanded, "LOG ", 4) == 0)
        {
            char logCommand[224];
            strncpy(logCommand, expanded + 4, sizeof(logCommand) - 1);
            logCommand[sizeof(logCommand) - 1] = '\0';

            char* save = nullptr;
            char* path = strtok_r(logCommand, " \t", &save);
            char* fieldList = save;
            while (fieldList != nullptr &&
                   (*fieldList == ' ' || *fieldList == '\t'))
                ++fieldList;

            if (path == nullptr || fieldList == nullptr || *fieldList == '\0' ||
                strncmp(path, "/logs/", 6) != 0 || strstr(path, "..") != nullptr)
            {
                m_console.println("      FAIL: Invalid LOG syntax or path");
                return Result::InvalidLog;
            }

            const size_t pathLength = strlen(path);
            if (pathLength < 5 || strcmp(path + pathLength - 4, ".csv") != 0)
            {
                m_console.println("      FAIL: LOG file must be /logs/*.csv");
                return Result::InvalidLog;
            }

            static constexpr size_t MaxLogFields = MaxRuntimeValues;
            const char* fieldNames[MaxLogFields];
            const char* fieldValues[MaxLogFields];
            size_t fieldCount = 0;

            char* fieldSave = nullptr;
            char* field = strtok_r(fieldList, ",", &fieldSave);
            while (field != nullptr)
            {
                while (*field == ' ' || *field == '\t') ++field;
                char* end = field + strlen(field);
                while (end > field && (end[-1] == ' ' || end[-1] == '\t')) --end;
                *end = '\0';

                if (*field == '\0' || fieldCount >= MaxLogFields)
                {
                    m_console.println("      FAIL: Invalid LOG field list");
                    return Result::InvalidLog;
                }

                const char* value = nullptr;
                for (size_t r = 0; r < runtimeValueCount; ++r)
                {
                    if (strcmp(runtimeStorage[r].name, field) == 0)
                    {
                        value = runtimeStorage[r].value;
                        break;
                    }
                }

                if (value == nullptr)
                {
                    m_console.printf("      FAIL: LOG variable not captured: %s\n", field);
                    return Result::InvalidLog;
                }

                fieldNames[fieldCount] = field;
                fieldValues[fieldCount] = value;
                ++fieldCount;
                field = strtok_r(nullptr, ",", &fieldSave);
            }

            if (fieldCount == 0)
            {
                m_console.println("      FAIL: LOG requires at least one variable");
                return Result::InvalidLog;
            }

            File file = m_sdCard.open(path, FILE_APPEND);
            if (!file)
            {
                m_console.printf("      FAIL: Unable to open %s\n", path);
                return Result::LogError;
            }

            const bool writeHeader = (file.size() == 0);

            if (writeHeader)
            {
                for (size_t f = 0; f < fieldCount; ++f)
                {
                    if (f != 0 && file.print(',') != 1)
                    {
                        file.close(); return Result::LogError;
                    }
                    if (file.print(fieldNames[f]) != strlen(fieldNames[f]))
                    {
                        file.close(); return Result::LogError;
                    }
                }
                if (file.print("\r\n") != 2)
                {
                    file.close(); return Result::LogError;
                }
            }

            for (size_t f = 0; f < fieldCount; ++f)
            {
                if (f != 0 && file.print(',') != 1)
                {
                    file.close(); return Result::LogError;
                }
                if (file.print(fieldValues[f]) != strlen(fieldValues[f]))
                {
                    file.close(); return Result::LogError;
                }
            }
            if (file.print("\r\n") != 2)
            {
                file.close(); return Result::LogError;
            }

            file.flush();
            file.close();

            m_console.printf("      logged %u value%s to %s\n",
                static_cast<unsigned int>(fieldCount),
                (fieldCount == 1) ? "" : "s", path);
            m_console.println("      PASS");

            if (progressCallback != nullptr)
                progressCallback(i + 1, macro.stepCount,
                    (i + 1 < macro.stepCount) ? i + 2 : 0, progressContext);
            ++i;
            continue;
        }

        char response[256];
        size_t responseLength = 0;

        // optional QUERY response capture:
        // QUERY <instrument> <capability> AS <variable>
        // The AS suffix belongs to the macro language and is removed before
        // the semantic QUERY is passed to CommandExecutor.
        char command[224];
        strncpy(command, expanded, sizeof(command) - 1);
        command[sizeof(command) - 1] = '\0';

        char captureName[32] = "";

        if (strncmp(command, "QUERY ", 6) == 0)
        {
            char* as = strstr(command, " AS ");
            if (as != nullptr)
            {
                *as = '\0';
                const char* name = as + 4;

                if (*name == '\0' ||
                    !(isalpha(static_cast<unsigned char>(*name)) ||
                      *name == '_'))
                {
                    m_console.println("      FAIL: Invalid capture variable");
                    return Result::InvalidCapture;
                }

                size_t n = 0;
                for (const char* p = name; *p != '\0'; ++p)
                {
                    if (!(isalnum(static_cast<unsigned char>(*p)) ||
                          *p == '_') ||
                        n + 1 >= sizeof(captureName))
                    {
                        m_console.println("      FAIL: Invalid capture variable");
                        return Result::InvalidCapture;
                    }
                    captureName[n++] = *p;
                }
                captureName[n] = '\0';
            }
        }

        const CommandExecutor::Result commandResult =
            m_commandExecutor.execute(
                command, response, sizeof(response), responseLength,
                commandTimeoutMilliseconds);

        if (commandResult != CommandExecutor::Result::Success)
        {
            m_console.printf("      FAIL: %s\n",
                             CommandExecutor::resultText(commandResult));
            return Result::CommandError;
        }

        if (captureName[0] != '\0')
        {
            char* first = response;
            while (*first != '\0' &&
                   isspace(static_cast<unsigned char>(*first)))
                ++first;

            char* last = first + strlen(first);
            while (last > first &&
                   isspace(static_cast<unsigned char>(last[-1])))
                --last;
            *last = '\0';

            size_t slot = runtimeValueCount;
            for (size_t r = 0; r < runtimeValueCount; ++r)
            {
                if (strcmp(runtimeStorage[r].name, captureName) == 0)
                {
                    slot = r;
                    break;
                }
            }

            if (slot == runtimeValueCount)
            {
                if (runtimeValueCount >= MaxRuntimeValues)
                {
                    m_console.println("      FAIL: Too many runtime variables");
                    return Result::InvalidCapture;
                }
                ++runtimeValueCount;
            }

            if (strlen(captureName) >= sizeof(runtimeStorage[slot].name) ||
                strlen(first) >= sizeof(runtimeStorage[slot].value))
            {
                m_console.println("      FAIL: Captured value too long");
                return Result::InvalidCapture;
            }

            strcpy(runtimeStorage[slot].name, captureName);
            strcpy(runtimeStorage[slot].value, first);

            m_console.printf("      %s = %s\n",
                             runtimeStorage[slot].name,
                             runtimeStorage[slot].value);

            // Publish only explicitly declared result fields, never arbitrary
            // runtime values. The callback copies the value if it needs to
            // retain it after this invocation.
            if (m_resultCallback != nullptr)
            {
                for (size_t r = 0; r < macro.resultCount; ++r)
                {
                    if (strcmp(macro.results[r].source, captureName) == 0)
                    {
                        m_resultCallback(macro.results[r].name,
                                         runtimeStorage[slot].value,
                                         m_resultContext);
                    }
                }
            }
        }

        m_console.println("      PASS");

        if (progressCallback != nullptr)
            progressCallback(
                i + 1,
                macro.stepCount,
                (i + 1 < macro.stepCount) ? i + 2 : 0,
                progressContext);

        ++i;
    }

    return Result::Success;
}

const char* MacroExecutor::resultText(Result result)
{
    switch (result)
    {
        case Result::Success: return "Success";
        case Result::InvalidArgument: return "Invalid argument";
        case Result::MacroConfigurationError: return "Macro configuration error";
        case Result::InvalidDelay: return "Invalid DELAY value";
        case Result::InvalidCapture: return "Invalid capture variable";
        case Result::InvalidLog: return "Invalid LOG command";
        case Result::LogError: return "CSV logging error";
        case Result::InvalidSweep: return "Invalid SWEEP command";
        case Result::CommandError: return "Instrument command error";
        case Result::Cancelled: return "Macro aborted by user";
    }
    return "Unknown macro executor result";
}

}
