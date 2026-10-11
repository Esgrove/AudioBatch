/// Console application entry point for the headless batch analysis executable.
/// Parses arguments through AudioAnalysisCli and answers the help and version flags
/// before any logging starts, so those flags only print to the console.
/// Every other invocation sets up file logging and runs the analysis workflow,
/// translating the outcome into a process exit code.

#include "AudioAnalysisCli.h"
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
}  // namespace audiobatch::cli

/// Entry point for the headless batch analysis executable.
int main(const int argc, char* argv[])
{
    juce::ArgumentList arguments(argc, argv);
    const auto executableName = juce::File(arguments.executableName).getFileNameWithoutExtension();

    juce::String parseError;
    const auto cliOptions = AudioAnalysisCli::parse(std::move(arguments), parseError);

    // Help and version only print to the console.
    // They return before the logger is created, so they never touch the log file.
    if (cliOptions.has_value() && cliOptions->showVersion) {
        fmt::print("{}\n", utils::versionLine(executableName));
        return 0;
    }

    if (cliOptions.has_value() && cliOptions->showHelp) {
        fmt::print("{}\n", AudioAnalysisCli::buildUsage(executableName));
        return 0;
    }

    const auto logger = utils::createDefaultLogger(executableName);
    juce::Logger::setCurrentLogger(logger.get());

    if (!cliOptions.has_value()) {
        utils::logError(parseError);
        console::printError(parseError);
        fmt::print("\n{}\n", AudioAnalysisCli::buildUsage(executableName));
        return audiobatch::cli::exitCli(1);
    }

    const auto exitCode = AudioAnalysisCli::run(*cliOptions);

    if (exitCode != 0) {
        utils::logError("CLI exiting with code {}", exitCode);
    } else {
        utils::logInfo("CLI exiting with code 0");
    }

    return audiobatch::cli::exitCli(exitCode);
}
