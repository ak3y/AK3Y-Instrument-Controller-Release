/******************************************************************************
 * File: MacroExecutor.h
 * Project: AK3Y Instrument Controller
 * Description: Executes configured instrument-control macros.
 * Responsibilities: Performs preflight validation and executes macro steps, including command dispatch, delays, captures, sweeps, logging, progress reporting, and cancellation.
 * Dependencies: MacroConfiguration, CommandExecutor, SDCard, and Console.
 * Notes: Documentation-only update; implementation behavior is unchanged.
 ******************************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <MacroConfiguration.h>

namespace AK3Y
{
class CommandExecutor;
class Console;
class SDCard;

class MacroExecutor
{
public:
    enum class Result : uint8_t
    {
        Success = 0,
        InvalidArgument,
        MacroConfigurationError,
        InvalidDelay,
        InvalidCapture,
        InvalidLog,
        LogError,
        InvalidSweep,
        CommandError,
        Cancelled
    };

    typedef void (*ProgressCallback)(
        size_t completedSteps,
        size_t totalSteps,
        size_t currentStep,
        void* context);

    typedef bool (*CancellationCallback)(void* context);

    // Called synchronously when a declared result's QUERY capture changes.
    // Strings remain valid only for the duration of the callback.
    typedef void (*ResultCallback)(const char* name, const char* value,
                                   void* context);

    void setResultCallback(ResultCallback callback, void* context)
    {
        m_resultCallback = callback;
        m_resultContext = context;
    }


    MacroExecutor(MacroConfiguration& macroConfiguration,
                  CommandExecutor& commandExecutor,
                  SDCard& sdCard,
                  Console& console);

    Result execute(const char* macroFile,
                   uint32_t commandTimeoutMilliseconds = 1000);

    Result execute(
        const char* macroFile,
        const MacroConfiguration::ValueOverride* overrides,
        size_t overrideCount,
        uint32_t commandTimeoutMilliseconds = 1000);

    Result execute(
        const char* macroFile,
        const MacroConfiguration::ValueOverride* overrides,
        size_t overrideCount,
        ProgressCallback progressCallback,
        void* progressContext,
        uint32_t commandTimeoutMilliseconds = 1000);

    Result execute(
        const char* macroFile,
        const MacroConfiguration::ValueOverride* overrides,
        size_t overrideCount,
        ProgressCallback progressCallback,
        void* progressContext,
        CancellationCallback cancellationCallback,
        void* cancellationContext,
        uint32_t commandTimeoutMilliseconds = 1000);

    static const char* resultText(Result result);

private:
    static constexpr size_t MaxRuntimeValues = 16;

    Result preflight(
        const MacroConfiguration::Macro& macro,
        const MacroConfiguration::ValueOverride* overrides,
        size_t overrideCount);

    struct RuntimeValueStorage
    {
        char name[32];
        char value[64];
    };

    ResultCallback m_resultCallback = nullptr;
    void* m_resultContext = nullptr;

    MacroConfiguration& m_macroConfiguration;
    CommandExecutor& m_commandExecutor;
    SDCard& m_sdCard;
    Console& m_console;
};
}
