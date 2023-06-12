#pragma once

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

template<class CharT, class Traits>
constexpr std::basic_ostream<CharT, Traits>& reset(std::basic_ostream<CharT, Traits>& os)
{
    return os << ANSI_RESET;
}

template<class CharT, class Traits>
constexpr std::basic_ostream<CharT, Traits>& black(std::basic_ostream<CharT, Traits>& os)
{
    return os << ANSI_BLACK;
}

template<class CharT, class Traits>
constexpr std::basic_ostream<CharT, Traits>& red(std::basic_ostream<CharT, Traits>& os)
{
    return os << ANSI_RED;
}

template<class CharT, class Traits>
constexpr std::basic_ostream<CharT, Traits>& yellow(std::basic_ostream<CharT, Traits>& os)
{
    return os << ANSI_YELLOW;
}

template<class CharT, class Traits>
constexpr std::basic_ostream<CharT, Traits>& cyan(std::basic_ostream<CharT, Traits>& os)
{
    return os << ANSI_CYAN;
}

template<class CharT, class Traits>
constexpr std::basic_ostream<CharT, Traits>& magenta(std::basic_ostream<CharT, Traits>& os)
{
    return os << ANSI_MAGENTA;
}

template<class CharT, class Traits>
constexpr std::basic_ostream<CharT, Traits>& green(std::basic_ostream<CharT, Traits>& os)
{
    return os << ANSI_GREEN;
}

}  // namespace ansi

namespace utils
{
enum class Level { debug, info, warn, error };

/// Return full system information list
juce::StringArray system_info();

/// Return full system information formatted as a string
juce::String formatted_system_info();

inline juce::String format_json(const juce::var& object)
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
static void write_to_log(const juce::String& message, [[maybe_unused]] Level log_level = Level::info)
{
    auto timestamp = juce::Time::getCurrentTime().formatted("%H:%M:%S ");
    auto log_message = timestamp + message;
    // Print the log messages to stdout / stderr when running in release configuration, since
    // otherwise log output will not be visible when running the app from command line.
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
/// Update YS_LOG_DEBUG_MESSAGES definition to true (1) to show these.
inline void log_debug([[maybe_unused]] const juce::String& message)
{
#if JUCE_DEBUG
    // Write to log if debug logging is enabled
    write_to_log("[DEBUG]: " + message, Level::debug);
#endif
}

inline void log_info(const juce::String& message)
{
    write_to_log("[INFO]: " + message);
}

inline void log_error(const juce::String& message)
{
    write_to_log("[ERROR]: " + message, Level::error);
}

inline void log_json(const juce::var& object, bool debug = false)
{
    if (debug) {
        log_debug(format_json(object));
    } else {
        log_info(format_json(object));
    }
}

inline void log_system_info()
{
    // Get info first to avoid API call debug / error messages in the middle of the log message
    auto info = utils::system_info();
    log_info("System info\n    " + juce::JUCEApplication::getInstance()->getApplicationName());
    for (const auto& line : info) {
#if !JUCE_DEBUG
        std::cout << "    " << line << juce::newLine;
#endif
        juce::Logger::writeToLog("    " + line);
    }
}
}  // namespace utils
