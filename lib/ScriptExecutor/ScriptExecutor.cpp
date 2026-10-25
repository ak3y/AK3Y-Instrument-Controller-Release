/******************************************************************************
 * File: ScriptExecutor.cpp
 * Project: AK3Y Instrument Controller
 * Description: Executes command scripts stored on the SD card.
 * Responsibilities: Reads script lines, processes supported directives, dispatches instrument commands, and reports execution errors.
 * Dependencies: SDCard, CommandExecutor, and Console.
 * Notes: Documentation-only update; implementation behavior is unchanged.
 ******************************************************************************/

#include "ScriptExecutor.h"

#include <CommandExecutor.h>
#include <Console.h>
#include <SDCard.h>

#include <Arduino.h>
#include <FS.h>
#include <stdlib.h>
#include <string.h>

namespace AK3Y {

namespace {

char* trim(char* text)
{
    while (*text == ' ' || *text == '\t' ||
           *text == '\r' || *text == '\n')
    {
        ++text;
    }

    char* end = text + strlen(text);

    while (end > text)
    {
        const char c = *(end - 1);

        if (c != ' ' && c != '\t' &&
            c != '\r' && c != '\n')
        {
            break;
        }

        --end;
    }

    *end = '\0';
    return text;
}

}

ScriptExecutor::ScriptExecutor(
    SDCard& sdCard,
    CommandExecutor& commandExecutor,
    Console& console)
    :
    m_sdCard(sdCard),
    m_commandExecutor(commandExecutor),
    m_console(console)
{
}

ScriptExecutor::Result ScriptExecutor::execute(
    const char* fileName,
    uint32_t commandTimeoutMilliseconds)
{
    if (fileName == nullptr || *fileName == '\0')
    {
        return Result::InvalidArgument;
    }

    File file = m_sdCard.open(fileName, FILE_READ);

    if (!file)
    {
        return Result::FileOpenError;
    }

    char line[192];
    size_t lineLength = 0;
    unsigned int lineNumber = 1;

    while (file.available())
    {
        const int input = file.read();

        if (input < 0)
        {
            break;
        }

        const char c = static_cast<char>(input);

        if (c == '\r')
        {
            continue;
        }

        if (c != '\n')
        {
            if (lineLength + 1 >= sizeof(line))
            {
                file.close();
                return Result::LineTooLong;
            }

            line[lineLength++] = c;
            continue;
        }

        line[lineLength] = '\0';
        char* command = trim(line);

        if (*command != '\0' && *command != '#' && *command != ';')
        {
            m_console.printf("[%u] %s\n", lineNumber, command);

            if (strncmp(command, "DELAY ", 6) == 0)
            {
                char* delayText = trim(command + 6);
                char* endPointer = nullptr;
                const unsigned long delayMilliseconds =
                    strtoul(delayText, &endPointer, 10);

                if (endPointer == delayText ||
                    *endPointer != '\0')
                {
                    file.close();
                    return Result::InvalidDelay;
                }

                m_console.printf(
                    "    Delay: %lu ms\n",
                    delayMilliseconds);

                delay(delayMilliseconds);
            }
            else
            {
                char response[256];
                size_t responseLength = 0;

                const CommandExecutor::Result result =
                    m_commandExecutor.execute(
                        command,
                        response,
                        sizeof(response),
                        responseLength,
                        commandTimeoutMilliseconds);

                m_console.print("    Result: ");
                m_console.println(
                    CommandExecutor::resultText(result));

                if (result != CommandExecutor::Result::Success)
                {
                    file.close();
                    return Result::CommandError;
                }

                if (responseLength > 0)
                {
                    m_console.print("    Response: ");
                    m_console.println(response);
                }
            }
        }

        lineLength = 0;
        ++lineNumber;
    }

    // Process a final line that does not end in newline.
    if (lineLength > 0)
    {
        line[lineLength] = '\0';
        char* command = trim(line);

        if (*command != '\0' && *command != '#' && *command != ';')
        {
            m_console.printf("[%u] %s\n", lineNumber, command);

            if (strncmp(command, "DELAY ", 6) == 0)
            {
                char* delayText = trim(command + 6);
                char* endPointer = nullptr;
                const unsigned long delayMilliseconds =
                    strtoul(delayText, &endPointer, 10);

                if (endPointer == delayText ||
                    *endPointer != '\0')
                {
                    file.close();
                    return Result::InvalidDelay;
                }

                m_console.printf(
                    "    Delay: %lu ms\n",
                    delayMilliseconds);

                delay(delayMilliseconds);
            }
            else
            {
                char response[256];
                size_t responseLength = 0;

                const CommandExecutor::Result result =
                    m_commandExecutor.execute(
                        command,
                        response,
                        sizeof(response),
                        responseLength,
                        commandTimeoutMilliseconds);

                m_console.print("    Result: ");
                m_console.println(
                    CommandExecutor::resultText(result));

                if (result != CommandExecutor::Result::Success)
                {
                    file.close();
                    return Result::CommandError;
                }

                if (responseLength > 0)
                {
                    m_console.print("    Response: ");
                    m_console.println(response);
                }
            }
        }
    }

    file.close();
    return Result::Success;
}

bool ScriptExecutor::listScripts()
{
    File directory = m_sdCard.open("/scripts", FILE_READ);

    if (!directory || !directory.isDirectory())
    {
        if (directory)
        {
            directory.close();
        }

        return false;
    }

    m_console.println("Available scripts:");

    bool found = false;

    File entry = directory.openNextFile();

    while (entry)
    {
        if (!entry.isDirectory())
        {
            const char* name = entry.name();

            //
            // Ignore hidden/metadata files such as macOS AppleDouble
            // entries (for example, ._test.txt).
            //

            if (name != nullptr &&
                name[0] != '.')
            {
                m_console.print("  /scripts/");
                m_console.println(name);
                found = true;
            }
        }

        entry.close();
        entry = directory.openNextFile();
    }

    directory.close();

    if (!found)
    {
        m_console.println("  (none)");
    }

    return true;
}

const char* ScriptExecutor::resultText(Result result)
{
    switch (result)
    {
        case Result::Success:
            return "Success";
        case Result::InvalidArgument:
            return "Invalid argument";
        case Result::FileOpenError:
            return "File open error";
        case Result::LineTooLong:
            return "Script line too long";
        case Result::InvalidDelay:
            return "Invalid DELAY";
        case Result::CommandError:
            return "Command execution error";
    }

    return "Unknown";
}

}
