/// Console application entry point for the headless batch analysis executable.
/// Sets up logging, parses arguments through AudioAnalysisCli,
/// handles the help and version flags, and runs the analysis workflow,
/// translating the outcome into a process exit code.

#include "AudioAnalysisCli.h"
#include "utils.h"
#include "version.h"

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
    const auto logger = utils::createDefaultLogger(executableName);
    juce::Logger::setCurrentLogger(logger.get());

    juce::String parseError;
    const auto cliOptions = AudioAnalysisCli::parse(std::move(arguments), parseError);

    if (!cliOptions.has_value()) {
        utils::logError(parseError);
        std::cerr << ansi::red << parseError << ansi::reset << std::endl << std::endl;
        std::cout << AudioAnalysisCli::buildUsage(executableName) << std::endl;
        return audiobatch::cli::exitCli(1);
    }

    if (cliOptions->showHelp) {
        std::cout << AudioAnalysisCli::buildUsage(executableName) << std::endl;
        return audiobatch::cli::exitCli(0);
    }

    if (cliOptions->showVersion) {
        std::cout << executableName << " " << version::VERSION_INFO << " " << version::BRANCH << std::endl;
        return audiobatch::cli::exitCli(0);
    }

    const auto exitCode = AudioAnalysisCli::run(*cliOptions);

    if (exitCode != 0) {
        utils::logError("CLI exiting with code " + juce::String(exitCode));
    } else {
        utils::logInfo("CLI exiting with code 0");
    }

    return audiobatch::cli::exitCli(exitCode);
}
