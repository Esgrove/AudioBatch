/// Leveled application logging.
/// The utils logging functions write timestamped messages through the current juce::Logger,
/// and echo them to stderr in release builds where the file logger does not print to the console.
/// They accept either a finished message or an fmt format string with its arguments.

#pragma once

#include "StringFormat.h"

#include <JuceHeader.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace utils
{
/// Severity of a log message, which picks its prefix and console colour.
enum class Level : std::uint8_t { debug, info, warn, error };

/// Creates the default rolling file logger for the application.
/// The log file goes into the application's log folder and is named after the log name,
/// which defaults to the application name.
std::unique_ptr<juce::FileLogger> createDefaultLogger(const juce::String& appName, const juce::String& logName = {});

/// Writes a message with a timestamp to the current logger.
/// In release builds the message is also printed to stderr, coloured by level when stderr is a terminal,
/// since the file logger only prints to the console in debug builds.
/// stderr keeps log output apart from the CLI results on stdout.
/// Prefer the leveled log functions, which add the level prefix.
void writeToLog(const juce::String& message, Level logLevel = Level::info);

/// Logs a debug message. Debug messages are only logged in debug builds.
inline void logDebug([[maybe_unused]] const juce::String& message)
{
#if JUCE_DEBUG
    writeToLog(format("[DEBUG]: {}", message), Level::debug);
#endif
}

/// Formats and logs a debug message directly from an fmt format string and arguments.
/// In release builds the formatting work is skipped entirely since the message is not logged.
template<typename... Args>
void logDebug([[maybe_unused]] fmt::format_string<Args...> formatString, [[maybe_unused]] Args&&... args)
{
#if JUCE_DEBUG
    logDebug(utils::format(formatString, std::forward<Args>(args)...));
#endif
}

/// Writes an informational message to the application log.
inline void logInfo(const juce::String& message)
{
    writeToLog(format("[INFO]: {}", message));
}

/// Formats and logs an informational message directly from an fmt format string and arguments.
template<typename... Args>
void logInfo(fmt::format_string<Args...> formatString, Args&&... args)
{
    logInfo(utils::format(formatString, std::forward<Args>(args)...));
}

/// Writes a warning message to the application log.
inline void logWarn(const juce::String& message)
{
    writeToLog(format("[WARN]: {}", message), Level::warn);
}

/// Formats and logs a warning message directly from an fmt format string and arguments.
template<typename... Args>
void logWarn(fmt::format_string<Args...> formatString, Args&&... args)
{
    logWarn(utils::format(formatString, std::forward<Args>(args)...));
}

/// Writes an error message to the application log.
inline void logError(const juce::String& message)
{
    writeToLog(format("[ERROR]: {}", message), Level::error);
}

/// Formats and logs an error message directly from an fmt format string and arguments.
template<typename... Args>
void logError(fmt::format_string<Args...> formatString, Args&&... args)
{
    logError(utils::format(formatString, std::forward<Args>(args)...));
}

/// Logs the collected system information as an indented block.
/// Detail lines are written without the usual timestamp prefix so the block stays readable.
void logSystemInfo();
}  // namespace utils
