/// Implementation of the logging helpers:
/// creating the rolling file logger, writing timestamped messages with the release-build stderr echo,
/// and logging the system information block at startup.

#include "Log.h"

#include "AppInfo.h"
#include "ConsolePrint.h"
#include "version.h"

namespace utils
{
/// Creates the default rolling file logger for the application.
/// The log file goes into the application's log folder and is named after the log name,
/// which defaults to the application name.
std::unique_ptr<juce::FileLogger> createDefaultLogger(const juce::String& appName, const juce::String& logName)
{
    /// Size in bytes the log file is trimmed to when it is opened.
    constexpr int maximumInitialLogSize = 32768;
    const auto name = logName.isNotEmpty() ? logName : appName;
    return std::unique_ptr<juce::FileLogger>(
        juce::FileLogger::createDefaultAppLogger(appName, format("{}.log", name), name, maximumInitialLogSize)
    );
}

/// Writes a message with a timestamp to the current logger.
/// In release builds the message is also printed to stderr, coloured by level when stderr is a terminal,
/// since the file logger only prints to the console in debug builds.
/// stderr keeps log output apart from the CLI results on stdout.
/// Prefer the leveled log functions, which add the level prefix.
void writeToLog(const juce::String& message, [[maybe_unused]] const Level logLevel)
{
    const auto timestamp = juce::Time::getCurrentTime().formatted("%H:%M:%S ");
    const auto logMessage = format("{}{}", timestamp, message);
    // In debug builds, the log messages are printed to console already by juce::FileLogger,
    // so this prevents them from being printed twice in debug builds.
#if !JUCE_DEBUG
    switch (logLevel) {
        case Level::error:
            console::printStyled(stderr, fmt::fg(fmt::terminal_color::red), "{}\n", logMessage);
            break;
        case Level::warn:
            console::printStyled(stderr, fmt::fg(fmt::terminal_color::yellow), "{}\n", logMessage);
            break;
        case Level::debug:
        case Level::info:
            fmt::print(stderr, "{}\n", logMessage);
            break;
    }
#endif
    juce::Logger::writeToLog(logMessage);
}

/// Logs the collected system information as an indented block.
/// Detail lines are written without the usual timestamp prefix so the block stays readable.
void logSystemInfo()
{
    // Get info first to avoid API call debug / error messages in the middle of the log message
    auto const info = systemInfo();
    logInfo("System info\n    {}", version::APP_NAME);
    for (const auto& line : info) {
#if !JUCE_DEBUG
        fmt::print(stderr, "    {}\n", line);
#endif
        juce::Logger::writeToLog(format("    {}", line));
    }
}
}  // namespace utils
