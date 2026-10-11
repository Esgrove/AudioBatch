/// Implementation of the logging helpers:
/// creating the rolling file logger, writing timestamped messages with the release-build stderr echo,
/// and logging the system information block at startup.

#include "Log.h"

#include "AppInfo.h"
#include "ConsolePrint.h"
#include "version.h"

namespace utils
{
std::unique_ptr<juce::FileLogger> createDefaultLogger(const juce::String& appName, const juce::String& logName)
{
    /// Size in bytes the log file is trimmed to when it is opened.
    constexpr int maximumInitialLogSize = 32768;
    const auto name = logName.isNotEmpty() ? logName : appName;
    return std::unique_ptr<juce::FileLogger>(
        juce::FileLogger::createDefaultAppLogger(appName, format("{}.log", name), name, maximumInitialLogSize)
    );
}

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
