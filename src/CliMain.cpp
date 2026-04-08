#include "AudioAnalysisCli.h"
#include "utils.h"
#include "version.h"

namespace audiobatch::cli
{
static int exitCli(const int exitCode)
{
    juce::Logger::setCurrentLogger(nullptr);
    return exitCode;
}
}  // namespace audiobatch::cli

/// Entry point for the headless batch analysis executable.
int main(const int argc, char* argv[])
{
    const juce::ArgumentList arguments(argc, argv);
    const auto executableName = juce::File(arguments.executableName).getFileNameWithoutExtension();
    auto logger = utils::create_default_logger(executableName);
    juce::Logger::setCurrentLogger(logger.get());

    juce::String parseError;
    const auto cliOptions = AudioAnalysisCli::parse(std::move(arguments), parseError);

    if (!cliOptions.has_value()) {
        utils::log_error(parseError);
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
        utils::log_error("CLI exiting with code " + juce::String(exitCode));
    } else {
        utils::log_info("CLI exiting with code 0");
    }

    return audiobatch::cli::exitCli(exitCode);
}
