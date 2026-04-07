#pragma once

#include "AnalysisCoordinator.h"

#include <optional>

/// Parsed command-line options for the headless audio analysis executable.
struct AudioAnalysisCliOptions {
    bool cliMode = false;
    bool recursive = false;
    bool refresh = false;
    bool showHelp = false;
    bool showVersion = false;
    int workerCount = juce::SystemStats::getNumCpus();
    AudioAnalysisSortMode sortMode = AudioAnalysisSortMode::peak;
    juce::Array<juce::File> inputPaths;
};

/// Command-line front end for batch audio analysis.
class AudioAnalysisCli
{
public:
    /// Builds the help text shown by the CLI executable.
    static juce::String buildUsage(const juce::String& executableName);

    /// Parses command-line arguments into a validated options object.
    static std::optional<AudioAnalysisCliOptions> parse(
        const juce::ArgumentList& arguments,
        juce::String& errorMessage
    );

    /// Executes the CLI analysis workflow and returns the process exit code.
    static int run(const AudioAnalysisCliOptions& options);
};
