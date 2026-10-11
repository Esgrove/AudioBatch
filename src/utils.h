/// Shared logging and small helper utilities.
/// The utils namespace provides leveled logging on top of juce::FileLogger,
/// file deletion and trash helpers, and system information formatting for logs and the About dialog.

#pragma once

#include "ConsolePrint.h"
#include "StringFormat.h"
#include "version.h"

#include <JuceHeader.h>

namespace utils
{
enum class Level { debug, info, warn, error };

/// Creates the default rolling file logger for the application.
/// The log file goes into the application's log folder and is named after the log name,
/// which defaults to the application name.
std::unique_ptr<juce::FileLogger> createDefaultLogger(const juce::String& appName, const juce::String& logName = {});

/// Returns the storage options for the AudioBatch.settings file that holds the GUI settings and plugin chain.
/// The CLI only reads this file, for example the plugin search folders of the last GUI scan.
juce::PropertiesFile::Options createApplicationSettingsOptions();

/// Move a file or folder to the OS trash / recycle bin.
bool moveToTrash(const juce::File& file);

/// Delete a file and log an error if the deletion fails.
/// Returns true if the file did not exist or was deleted successfully.
bool deleteFile(const juce::File& file);

/// Collects build and runtime environment details for logs and diagnostics.
juce::StringArray systemInfo();

/// Return full system information formatted as a string
juce::String formattedSystemInfo();

/// Builds the multi-line About dialog message with application, build, and system information.
juce::String aboutMessage(const juce::String& appName);

/// Returns the line printed by the --version flag of both executables:
/// the application name, the build version string from CMake, and the JUCE version.
juce::String versionLine(const juce::String& appName);

/// Prints one line of text to stdout and flushes it.
/// On Windows a GUI subsystem executable has no console of its own,
/// so when stdout is not already redirected to a pipe or file,
/// this attaches to the console of the parent process first, so the text shows up in cmd or PowerShell.
void printLineToConsole(const juce::String& text);

/// Renders a JSON var as plain text for log output,
/// stripping quotes and the outer braces from the serialized form.
inline juce::String formatJson(const juce::var& object)
{
    // clang-format off
    return juce::JSON::toString(object)
        .removeCharacters("\"")
        .trimCharactersAtStart("{")
        .trimCharactersAtEnd("}")
        .replace("\\n", juce::newLine)
        .trimStart()
        .trimEnd();
    // clang-format on
}

/// Write message to log with a timestamp.
/// This function should not be called directly outside utils.h!
static void writeToLog(const juce::String& message, [[maybe_unused]] Level log_level = Level::info)
{
    const auto timestamp = juce::Time::getCurrentTime().formatted("%H:%M:%S ");
    const auto log_message = format("{}{}", timestamp, message);
    // Print the log messages to stdout / stderr when running in release configuration,
    // since otherwise log output will not be visible when running the app from command line.
    // In debug builds, the log messages are printed to console already by juce::FileLogger,
    // so this prevents them from being printed twice in debug builds.
#if !JUCE_DEBUG
    switch (log_level) {
        case Level::error:
            console::printStyled(stderr, fmt::fg(fmt::terminal_color::red), "{}\n", log_message);
            break;
        case Level::debug:
            console::printYellow("{}\n", log_message);
            break;
        case Level::warn:
        case Level::info:
            [[fallthrough]];
        default:
            fmt::print("{}\n", log_message);
            break;
    }
#endif
    juce::Logger::writeToLog(log_message);
}

/// By default, these messages will not be logged in release builds, only in debug builds.
inline void logDebug([[maybe_unused]] const juce::String& message)
{
#if JUCE_DEBUG
    // Write to log if debug logging is enabled
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

/// Writes an error message to the application log, and to stderr in release builds.
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

/// Logs a JSON var as formatted plain text, at debug level when requested.
inline void logJson(const juce::var& object, const bool debug = false)
{
    if (debug) {
        logDebug(formatJson(object));
    } else {
        logInfo(formatJson(object));
    }
}

/// Logs the collected system information as an indented block.
/// Detail lines are written without the usual timestamp prefix so the block stays readable.
inline void logSystemInfo()
{
    // Get info first to avoid API call debug / error messages in the middle of the log message
    auto info = utils::systemInfo();
    logInfo("System info\n    {}", version::APP_NAME);
    for (const auto& line : info) {
#if !JUCE_DEBUG
        fmt::print("    {}\n", line);
#endif
        juce::Logger::writeToLog("    " + line);
    }
}
}  // namespace utils
