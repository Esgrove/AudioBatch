/// Console application entry point for the headless batch analysis executable.
/// Parses arguments through AudioAnalysisCli and answers the help and version flags
/// before any logging starts, so those flags only print to the console.
/// Every other invocation sets up file logging and runs the analysis workflow or the plugin listing,
/// translating the outcome into a process exit code.
/// When launched as a plugin scan worker by a plugin rescan, it runs the worker instead.

#include "AudioAnalysisCli.h"
#include "PluginListCli.h"
#include "PluginScanner.h"
#include "utils.h"

namespace audiobatch::cli
{
/// Detaches the logger before returning the process exit code,
/// so the juce::Logger never points at a logger that is about to be destroyed.
static int exitCli(const int exitCode)
{
    juce::Logger::setCurrentLogger(nullptr);
    return exitCode;
}

/// Runs this process as the plugin scan worker for a plugin rescan in another audiobatch process.
/// Runs a message loop until the scanning process disconnects, and logs to its own file.
static int runPluginScanWorker(const juce::String& executableName, const juce::String& commandLine)
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
}  // namespace audiobatch::cli

/// Entry point for the headless batch analysis executable.
int main(const int argc, char* argv[])
{
    const juce::ArgumentList arguments(argc, argv);
    const auto executableName = juce::File(arguments.executableName).getFileNameWithoutExtension();

    // The plugin rescan relaunches this executable as its scan worker.
    if (const auto commandLine = juce::StringArray(argv + 1, argc - 1).joinIntoString(" ");
        isPluginScanWorkerCommandLine(commandLine))
    {
        return audiobatch::cli::runPluginScanWorker(executableName, commandLine);
    }

    // Help and version only print to the console.
    // They are checked before parsing, so they win over any other argument, even an invalid one,
    // and return before the logger is created, so they never touch the log file.
    if (arguments.containsOption("--version|-V")) {
        fmt::print("{}\n", utils::versionLine(executableName));
        return 0;
    }

    if (arguments.containsOption("--help|-h")) {
        fmt::print("{}\n", AudioAnalysisCli::buildUsage(executableName));
        return 0;
    }

    juce::String parseError;
    const auto cliOptions = AudioAnalysisCli::parse(arguments, parseError);

    const auto logger = utils::createDefaultLogger(executableName);
    juce::Logger::setCurrentLogger(logger.get());

    if (!cliOptions.has_value()) {
        utils::logError(parseError);
        console::printError(parseError);
        fmt::print("\n{}\n", AudioAnalysisCli::buildUsage(executableName));
        return audiobatch::cli::exitCli(1);
    }

    int exitCode = 0;
    try {
        exitCode = cliOptions->listPlugins ? PluginListCli::run(cliOptions->rescanPlugins, cliOptions->pluginGrouping)
                                           : AudioAnalysisCli::run(*cliOptions);
    } catch (const std::system_error& error) {
        // fmt::print throws when stdout is closed early, for example when piping into head.
        utils::logError("Writing output failed: {}", error.what());
        return audiobatch::cli::exitCli(1);
    }

    if (exitCode != 0) {
        utils::logError("CLI exiting with code {}", exitCode);
    } else {
        utils::logInfo("CLI exiting with code 0");
    }

    return audiobatch::cli::exitCli(exitCode);
}
