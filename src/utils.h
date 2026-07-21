/// Shared logging, terminal color, and small helper utilities.
/// The ansi namespace provides ANSI color codes and stream manipulators for CLI output.
/// The utils namespace provides leveled logging on top of juce::FileLogger,
/// file deletion and trash helpers, and system information formatting for logs and the About dialog.

#pragma once

#include "StringFormat.h"
#include "version.h"

#include <JuceHeader.h>

namespace ansi
{
// Foreground
[[maybe_unused]] constexpr auto ANSI_BLACK = "\033[0;30m";
[[maybe_unused]] constexpr auto ANSI_RED = "\033[0;31m";
[[maybe_unused]] constexpr auto ANSI_GREEN = "\033[0;32m";
[[maybe_unused]] constexpr auto ANSI_YELLOW = "\033[0;33m";
[[maybe_unused]] constexpr auto ANSI_BLUE = "\033[0;34m";
[[maybe_unused]] constexpr auto ANSI_MAGENTA = "\033[0;35m";
[[maybe_unused]] constexpr auto ANSI_CYAN = "\033[0;36m";
[[maybe_unused]] constexpr auto ANSI_WHITE = "\033[0;37m";

// Foreground bold
[[maybe_unused]] constexpr auto ANSI_BOLD_BLACK = "\033[1;30m";
[[maybe_unused]] constexpr auto ANSI_BOLD_RED = "\033[1;31m";
[[maybe_unused]] constexpr auto ANSI_BOLD_GREEN = "\033[1;32m";
[[maybe_unused]] constexpr auto ANSI_BOLD_YELLOW = "\033[1;33m";
[[maybe_unused]] constexpr auto ANSI_BOLD_BLUE = "\033[1;34m";
[[maybe_unused]] constexpr auto ANSI_BOLD_MAGENTA = "\033[1;35m";
[[maybe_unused]] constexpr auto ANSI_BOLD_CYAN = "\033[1;36m";
[[maybe_unused]] constexpr auto ANSI_BOLD_WHITE = "\033[1;37m";

[[maybe_unused]] constexpr auto ANSI_RESET = "\033[0m";

// Stream manipulators for easy ANSI color usage with stringstreams.
// Example usage: std::cout << ansi::red << "example" << ansi::reset << std::endl;

/// Resets the stream's terminal colors back to the default.
template<class CharT, class Traits>
constexpr std::basic_ostream<CharT, Traits>& reset(std::basic_ostream<CharT, Traits>& stream)
{
    return stream << ANSI_RESET;
}

/// Switches the stream's terminal foreground color to black.
template<class CharT, class Traits>
constexpr std::basic_ostream<CharT, Traits>& black(std::basic_ostream<CharT, Traits>& stream)
{
    return stream << ANSI_BLACK;
}

/// Switches the stream's terminal foreground color to red.
template<class CharT, class Traits>
constexpr std::basic_ostream<CharT, Traits>& red(std::basic_ostream<CharT, Traits>& stream)
{
    return stream << ANSI_RED;
}

/// Switches the stream's terminal foreground color to yellow.
template<class CharT, class Traits>
constexpr std::basic_ostream<CharT, Traits>& yellow(std::basic_ostream<CharT, Traits>& stream)
{
    return stream << ANSI_YELLOW;
}

/// Switches the stream's terminal foreground color to cyan.
template<class CharT, class Traits>
constexpr std::basic_ostream<CharT, Traits>& cyan(std::basic_ostream<CharT, Traits>& stream)
{
    return stream << ANSI_CYAN;
}

/// Switches the stream's terminal foreground color to magenta.
template<class CharT, class Traits>
constexpr std::basic_ostream<CharT, Traits>& magenta(std::basic_ostream<CharT, Traits>& stream)
{
    return stream << ANSI_MAGENTA;
}

/// Switches the stream's terminal foreground color to green.
template<class CharT, class Traits>
constexpr std::basic_ostream<CharT, Traits>& green(std::basic_ostream<CharT, Traits>& stream)
{
    return stream << ANSI_GREEN;
}

}  // namespace ansi

namespace utils
{
enum class Level { debug, info, warn, error };

/// Creates the default rolling file logger for the application.
std::unique_ptr<juce::FileLogger> createDefaultLogger(const juce::String& appName);

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
            std::cerr << ansi::red << log_message << ansi::reset << juce::newLine;
            break;
        case Level::debug:
            std::cout << ansi::yellow << log_message << ansi::reset << juce::newLine;
            break;
        case Level::warn:
        case Level::info:
            [[fallthrough]];
        default:
            std::cout << log_message << juce::newLine;
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
        std::cout << "    " << line << juce::newLine;
#endif
        juce::Logger::writeToLog("    " + line);
    }
}
}  // namespace utils
