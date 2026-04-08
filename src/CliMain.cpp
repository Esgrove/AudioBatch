#include "AudioAnalysisCli.h"
#include "version.h"

#include <JuceHeader.h>

/// Entry point for the headless batch analysis executable.
int main(int argc, char* argv[])
{
    juce::ArgumentList arguments(argc, argv);
    const auto executableName = juce::File(arguments.executableName).getFileNameWithoutExtension();
    juce::String parseError;
    auto cliOptions = AudioAnalysisCli::parse(arguments, parseError);

    if (!cliOptions.has_value()) {
        std::cerr << parseError << juce::newLine << juce::newLine;
        std::cout << AudioAnalysisCli::buildUsage(executableName) << std::endl;
        return 1;
    }

    if (cliOptions->showHelp) {
        std::cout << AudioAnalysisCli::buildUsage(executableName) << std::endl;
        return 0;
    }

    if (cliOptions->showVersion) {
        std::cout << executableName << " " << version::VERSION_INFO << " " << version::BRANCH << std::endl;
        return 0;
    }

    return AudioAnalysisCli::run(*cliOptions);
}
