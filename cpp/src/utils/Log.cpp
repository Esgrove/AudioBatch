/// Implementation of the logging helpers:
/// creating the rolling file logger, writing timestamped messages with the release-build stderr echo,
/// logging the system information block at startup,
/// and logging an error with a stack trace when the process crashes or terminates on an uncaught exception.

#include "utils/Log.h"

#include "utils/AppInfo.h"
#include "utils/ConsolePrint.h"
#include "utils/version.h"
#include <string_view>

#include <atomic>
#include <bit>
#include <csignal>
#include <cstdlib>
#include <exception>

#if !JUCE_WINDOWS
#include <pthread.h>
#endif

namespace audiobatch::utils
{
namespace
{
/// Set by the first crash log, so a terminate handler followed by the abort signal it raises logs only once.
constinit std::atomic_flag crashLogged;

#if !JUCE_WINDOWS
/// Returns the conventional name of a fatal signal for the POSIX crash log.
constexpr std::string_view signalName(const int signalNumber)
{
    switch (signalNumber) {
        case SIGSEGV:
            return "SIGSEGV";
        case SIGABRT:
            return "SIGABRT";
        case SIGILL:
            return "SIGILL";
        case SIGFPE:
            return "SIGFPE";
        case SIGBUS:
            return "SIGBUS";
        case SIGSYS:
            return "SIGSYS";
        default:
            return "unknown signal";
    }
}
#endif

/// Logs the reason the process is about to die together with the current stack trace.
/// Runs inside a signal handler or the terminate handler,
/// so the logging is best effort: it allocates and takes the logger lock, which is not async-signal-safe.
/// That is acceptable because the process is terminating either way.
void logCrash(const juce::String& reason)
{
    if (crashLogged.test_and_set()) {
        return;
    }

    logError(
        "Application terminated unexpectedly: {}\nStack trace:\n{}",
        reason,
        juce::SystemStats::getStackBacktrace().trimEnd()
    );
}

/// Crash handler registered with JUCE for fatal signals, or for unhandled structured exceptions on Windows.
/// On POSIX systems JUCE passes the signal number and would kill the process with SIGKILL after this returns.
/// The signal is re-raised with its default action instead,
/// so the exit status and the system crash report still show the original signal.
void handleCrash(void* crashInfo)
{
#if JUCE_WINDOWS
    juce::ignoreUnused(crashInfo);
    logCrash("unhandled exception");
#else
    const auto signalNumber = static_cast<int>(std::bit_cast<std::intptr_t>(crashInfo));
    logCrash(format("{} (signal {})", signalName(signalNumber), signalNumber));

    std::signal(signalNumber, SIG_DFL);
    sigset_t signalSet {};
    sigemptyset(&signalSet);
    sigaddset(&signalSet, signalNumber);
    pthread_sigmask(SIG_UNBLOCK, &signalSet, nullptr);
    std::raise(signalNumber);
#endif
}

/// Terminate handler that logs the uncaught exception, if any, before aborting.
[[noreturn]] void handleTerminate()
{
    juce::String reason = "std::terminate called without an active exception";

    if (const auto exception = std::current_exception(); exception != nullptr) {
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& error) {
            reason = format("uncaught exception: {}", error.what());
        } catch (...) {
            reason = "uncaught exception of unknown type";
        }
    }

    logCrash(reason);
    std::abort();
}
}  // namespace

/// Makes crashes and uncaught exceptions write an error with a stack trace to the current logger,
/// so an unexpected quit, for example inside a hosted plugin, leaves a trace in the application log.
/// Call once after the logger is set up.
void installCrashLogging()
{
    juce::SystemStats::setApplicationCrashHandler(handleCrash);
    std::set_terminate(handleTerminate);
}

/// Creates the default rolling file logger for the application.
/// The log file goes into the application's log folder and is named after the log name,
/// which defaults to the application name.
std::unique_ptr<juce::FileLogger> createDefaultLogger(const juce::String& appName, const juce::String& logName)
{
    /// Size in bytes the log file is trimmed to when it is opened.
    constexpr int maximumInitialLogSize = 32'768;
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
}  // namespace audiobatch::utils
