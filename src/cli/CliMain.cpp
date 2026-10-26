/// Console application entry point for the headless batch analysis executable.
/// Parses arguments through the cli functions and answers the help and version flags
/// before any logging starts, so those flags only print to the console.
/// Every other invocation sets up file logging and runs the analysis workflow or the plugin listing,
/// translating the outcome into a process exit code.
/// When launched as a plugin scan worker by a plugin rescan, it runs the worker instead.

#include "cli/AudioAnalysisCli.h"
#include "cli/PluginListCli.h"
#include "plugins/PluginScanner.h"
#include "utils/AppInfo.h"
#include "utils/ConsolePrint.h"
#include "utils/Log.h"

#include <span>
#include <variant>

namespace audiobatch
{
namespace
{
/// Detaches the logger before returning the process exit code,
/// so the juce::Logger never points at a logger that is about to be destroyed.
int exitCli(const int exitCode)
{
    juce::Logger::setCurrentLogger(nullptr);
    return exitCode;
}

/// Runs this process as the plugin scan worker for a plugin rescan in another audiobatch process.
/// Runs a message loop until the scanning process disconnects, and logs to its own file.
int runPluginScanWorker(const juce::String& executableName, const juce::String& commandLine)
{
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;
    const auto logger = utils::createDefaultLogger(executableName, "audiobatchPluginScanner");
    juce::Logger::setCurrentLogger(logger.get());

    PluginScanWorker worker([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });

    if (!worker.initialiseFromCommandLine(commandLine)) {
        utils::logError("Could not connect the plugin scan worker to the scanning process");
        return exitCli(1);
    }

    juce::MessageManager::getInstance()->runDispatchLoop();
    return exitCli(0);
}

/// Runs the headless batch analysis executable and returns its process exit code.
int runCliMain(const std::span<char*> argv)
{
    const juce::ArgumentList arguments(static_cast<int>(argv.size()), argv.data());
    // argv[0] is often a relative path, which juce::File asserts on, so resolve it against the working directory.
    // An absolute path is kept as it is.
    const auto executableName
        = juce::File::getCurrentWorkingDirectory().getChildFile(arguments.executableName).getFileNameWithoutExtension();

    // The plugin rescan relaunches this executable as its scan worker.
    // The command line is rebuilt from the decoded arguments.
    if (const auto commandLine = cli::joinArguments(arguments); isPluginScanWorkerCommandLine(commandLine)) {
        return runPluginScanWorker(executableName, commandLine);
    }

    // Help and version only print to the console.
    // They are checked before parsing, so they win over any other argument, even an invalid one,
    // and return before the logger is created, so they never touch the log file.
    if (arguments.containsOption("--version|-V")) {
        fmt::print("{}\n", utils::versionLine(executableName));
        return 0;
    }

    if (arguments.containsOption("--help|-h")) {
        fmt::print("{}\n", cli::buildUsage(executableName));
        return 0;
    }

    const auto command = cli::parseArguments(arguments);

    const auto logger = utils::createDefaultLogger(executableName);
    juce::Logger::setCurrentLogger(logger.get());
    utils::installCrashLogging();

    if (!command) {
        utils::logError(command.error());
        console::printError(command.error());
        fmt::print("\n{}\n", cli::buildUsage(executableName));
        return exitCli(1);
    }

    int exitCode = 0;
    try {
        exitCode = std::visit(
            Overloaded {
                [](const ListPluginsCommand& listing) { return cli::runPluginList(listing.rescan, listing.grouping); },
                [](const AnalyzeCommand& analysis) { return cli::runAnalysis(analysis); },
            },
            *command
        );
    } catch (const std::system_error& error) {
        // fmt::print throws when stdout is closed early, for example when piping into head.
        utils::logError("Writing output failed: {}", error.what());
        return exitCli(1);
    }

    return exitCli(exitCode);
}
}  // namespace
}  // namespace audiobatch

/// Entry point for the headless batch analysis executable.
int main(const int argc, char* argv[])
{
    return audiobatch::runCliMain(std::span(argv, static_cast<std::size_t>(argc)));
}
