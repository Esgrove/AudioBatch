/// Command-line interface for the headless batch analysis executable.
/// Declares AudioAnalysisCliOptions, the parsed and validated CLI options,
/// and the AudioAnalysisCli class that builds the usage text, parses arguments, and runs the analysis workflow.

#pragma once

#include "AnalysisCoordinator.h"
#include "AudioNormalizationService.h"
#include "PluginListCli.h"

#include <cstdint>
#include <optional>

namespace audiobatch
{
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
    /// Search directories in the input paths recursively for audio files.
    bool recursive = false;
    /// Ignore cached results and analyze every file again.
    bool refresh = false;
    /// Normalize the files after analysis, using the normalization options below.
    bool normalize = false;
    /// Replace an existing normalized output file that is not the source itself.
    bool force = false;
    /// Normalization mode and target, set by --peak or --lufs.
    /// Defaults to a 0 dBFS sample peak.
    AudioNormalizationOptions normalization;
    /// True when --peak or --lufs was given, which requires --normalize.
    bool hasNormalizationTarget = false;
    /// List the known plugins instead of analyzing files.
    bool listPlugins = false;
    /// Scan for plugins again before listing them. Only valid with listPlugins.
    bool rescanPlugins = false;
    /// How the plugin list is grouped and ordered.
    PluginListGrouping pluginGrouping = PluginListGrouping::name;
    /// Number of files analyzed in parallel.
    int workerCount = juce::SystemStats::getNumCpus();
    /// Order of the printed results.
    AudioAnalysisSortMode sortMode = AudioAnalysisSortMode::peak;
    /// Sort in descending instead of ascending order.
    bool reverse = false;
    /// Format of the printed results.
    AudioAnalysisOutputFormat outputFormat = AudioAnalysisOutputFormat::table;
    /// Files and directories to analyze.
    juce::Array<juce::File> inputPaths;
};

/// Command-line front end for batch audio analysis.
class AudioAnalysisCli
{
public:
    static juce::String buildUsage(const juce::String& executableName);

    static void attachNegativeOptionValues(juce::ArgumentList& arguments);

    static bool containsExactOption(const juce::ArgumentList& arguments, const juce::StringArray& optionNames);

    static std::optional<AudioAnalysisCliOptions> parse(juce::ArgumentList arguments, juce::String& errorMessage);

    static int run(const AudioAnalysisCliOptions& options);
};
}  // namespace audiobatch
