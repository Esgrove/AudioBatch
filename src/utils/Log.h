/// Leveled application logging.
/// The utils logging functions write timestamped messages through the current juce::Logger,
/// and echo them to stderr in release builds where the file logger does not print to the console.
/// They accept either a finished message or an fmt format string with its arguments.
/// installCrashLogging makes crashes and uncaught exceptions leave an error with a stack trace in the log.

#pragma once

#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"

#include <JuceHeader.h>

#include <memory>
#include <utility>

namespace audiobatch::utils
{
/// Severity of a log message, which picks its prefix and console colour.
enum class Level : std::uint8_t { debug, info, warn, error };

/// Returns elapsed seconds for duration logs without discarding fractional milliseconds.
/// The start value must come from juce::Time::getMillisecondCounterHiRes.
inline double elapsedSecondsSince(const double startedAtMilliseconds)
{
    return (juce::Time::getMillisecondCounterHiRes() - startedAtMilliseconds) / millisecondsPerSecond;
}

std::unique_ptr<juce::FileLogger> createDefaultLogger(const juce::String& appName, const juce::String& logName = {});

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

void logSystemInfo();

void installCrashLogging();
}  // namespace audiobatch::utils
