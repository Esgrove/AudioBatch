/// Command-line interface for the headless batch analysis executable.
/// Declares AudioAnalysisCliOptions, the parsed and validated CLI options,
/// and the AudioAnalysisCli class that builds the usage text,
/// parses arguments, and runs the analysis workflow.

#pragma once

#include "AnalysisCoordinator.h"
#include "PluginListCli.h"

#include <cstdint>
#include <optional>

/// Formats for the analysis results printed to stdout.
enum class AudioAnalysisOutputFormat : std::uint8_t {
    /// Aligned columns for reading in a terminal.
    table,
    /// One header row and one row per file, for spreadsheets.
    csv,
    /// An array with one object per file, for scripts.
    json,
};

/// Parsed command-line options for the headless audio analysis executable.
struct AudioAnalysisCliOptions {
    bool recursive = false;
    bool refresh = false;
    bool normalize = false;
    /// Replace an existing normalized output file that is not the source itself.
    bool force = false;
    bool listPlugins = false;
    bool rescanPlugins = false;
    PluginListGrouping pluginGrouping = PluginListGrouping::name;
    int workerCount = juce::SystemStats::getNumCpus();
    AudioAnalysisSortMode sortMode = AudioAnalysisSortMode::peak;
    /// Sort in descending instead of ascending order.
    bool reverse = false;
    AudioAnalysisOutputFormat outputFormat = AudioAnalysisOutputFormat::table;
    juce::Array<juce::File> inputPaths;
};

/// Command-line front end for batch audio analysis.
class AudioAnalysisCli
{
public:
    /// Builds the help text shown by the CLI executable.
    static juce::String buildUsage(const juce::String& executableName);

    /// Parses command-line arguments into a validated options object.
    /// The help and version flags are handled by the caller before parsing.
    static std::optional<AudioAnalysisCliOptions> parse(juce::ArgumentList arguments, juce::String& errorMessage);

    /// Executes the CLI analysis workflow and returns the process exit code.
    static int run(const AudioAnalysisCliOptions& options);
};
