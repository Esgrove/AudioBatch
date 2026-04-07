#pragma once

#include "AnalysisCoordinator.h"

#include <optional>

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

class AudioAnalysisCli
{
public:
    static juce::String buildUsage(const juce::String& executableName);
    static std::optional<AudioAnalysisCliOptions> parse(
        const juce::ArgumentList& arguments,
        juce::String& errorMessage
    );
    static int run(const AudioAnalysisCliOptions& options);
};