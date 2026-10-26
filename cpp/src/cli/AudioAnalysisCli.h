/// Command-line interface for the headless batch analysis executable.
/// Declares CliCommand, the parsed and validated command of one invocation,
/// which is either an AnalyzeCommand or a ListPluginsCommand.
/// CliParseError says why the arguments were rejected.
/// The cli functions build the usage text, parse the arguments, and run the analysis workflow.

#pragma once

#include "analysis/AnalysisCoordinator.h"
#include "cli/PluginListCli.h"
#include "normalization/AudioNormalization.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <variant>
#include <vector>

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

/// Why a command line was rejected, which callers and tests can check without matching the message text.
enum class CliParseErrorKind : std::uint8_t {
    /// Several short options in one argument, such as "-rf".
    combinedShortOptions,
    /// An option that the command does not take.
    unknownOption,
    /// An option value that is not one of the accepted names, or not a number.
    invalidValue,
    /// A numeric option value outside its allowed range.
    valueOutOfRange,
    /// Options that cannot be used together.
    conflictingOptions,
    /// An option given without the option it depends on, such as --force without --normalize.
    missingRequiredOption,
    /// A negative number that no option took as its value.
    unexpectedNumber,
    /// Input paths given to a command that takes none.
    unexpectedInput,
};

/// An analysis run parsed from the command line, with every value validated.
struct AnalyzeCommand {
    /// Files and directories to analyze.
    juce::Array<juce::File> inputPaths;
    /// Search directories in the input paths recursively for audio files.
    bool recursive {false};
    /// Ignore cached results and analyze every file again.
    bool refresh {false};
    /// The normalization to run after the analysis, set by --normalize.
    /// The target comes from --peak or --lufs, defaulting to a 0 dBFS sample peak,
    /// and --force allows replacing an existing output file.
    std::optional<AudioNormalizationOptions> normalization;
    /// Number of files analyzed in parallel.
    int workerCount {juce::SystemStats::getNumCpus()};
    /// Order of the printed results.
    AudioAnalysisSortMode sortMode {AudioAnalysisSortMode::peak};
    /// Sort in descending instead of ascending order.
    bool reverse {false};
    /// Format of the printed results.
    AudioAnalysisOutputFormat outputFormat {AudioAnalysisOutputFormat::table};
};

/// A plugin listing parsed from the command line, selected by --list-plugins.
struct ListPluginsCommand {
    /// Scan for new or changed plugins before listing them.
    bool rescan {false};
    /// How the plugin list is grouped and ordered.
    PluginListGrouping grouping {PluginListGrouping::name};
};

/// A rejected command line.
struct CliParseError {
    /// The rule the arguments broke.
    CliParseErrorKind kind;
    /// The explanation shown to the user.
    juce::String message;
};

/// The command a CLI invocation runs.
using CliCommand = std::variant<AnalyzeCommand, ListPluginsCommand>;

namespace cli
{
juce::String buildUsage(const juce::String& executableName);
juce::String joinArguments(const juce::ArgumentList& arguments);

juce::StringArray findSkippedInputs(
    const juce::Array<juce::File>& inputPaths,
    const std::vector<AudioAnalysisRecord>& results
);

[[nodiscard]] std::expected<CliCommand, CliParseError> parseArguments(juce::ArgumentList arguments);

[[nodiscard]] int runAnalysis(const AnalyzeCommand& command);
}  // namespace cli
}  // namespace audiobatch
