/******************************************************************************
 * File: ScriptExecutor.h
 * Project: AK3Y Instrument Controller
 * Description: Executes command scripts stored on the SD card.
 * Responsibilities: Reads script lines, processes supported directives, dispatches instrument commands, and reports execution errors.
 * Dependencies: SDCard, CommandExecutor, and Console.
 * Notes: Documentation-only update; implementation behavior is unchanged.
 ******************************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace AK3Y {

class CommandExecutor;
class Console;
class SDCard;

class ScriptExecutor {
public:
    enum class Result : uint8_t {
        Success = 0,
        InvalidArgument,
        FileOpenError,
        LineTooLong,
        InvalidDelay,
        CommandError
    };

    ScriptExecutor(
        SDCard& sdCard,
        CommandExecutor& commandExecutor,
        Console& console);

    Result execute(
        const char* fileName,
        uint32_t commandTimeoutMilliseconds = 5000);

    bool listScripts();

    static const char* resultText(Result result);

private:
    SDCard& m_sdCard;
    CommandExecutor& m_commandExecutor;
    Console& m_console;
};

}
