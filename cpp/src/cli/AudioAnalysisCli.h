/// Command-line interface for the headless batch analysis executable.
/// Declares CliCommand, the parsed and validated command of one invocation,
/// which is an AnalyzeCommand, or the ListPluginsCommand or CompletionCommand of a subcommand.
/// CliParseError says why the arguments were rejected.
/// The cli functions build the usage texts, parse the arguments, and run the analysis workflow.

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

/// Shells that the completion subcommand generates a completion script for.
enum class CompletionShell : std::uint8_t {
    zsh,
    bash,
    fish,
    powershell,
};

/// The subcommand an invocation names in its first argument, which selects its options and its usage text.
enum class CliSubcommand : std::uint8_t {
    /// No subcommand, so the arguments are an analysis run.
    none,
    /// Lists the available audio plugins.
    list,
    /// Prints or installs a shell completion script.
    completion,
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
    /// Input paths given to a command that takes none, or more arguments than a command takes.
    unexpectedInput,
    /// A command given without the argument it needs, such as completion without a shell.
    missingArgument,
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

/// A plugin listing parsed from the command line, selected by the list subcommand.
struct ListPluginsCommand {
    /// Scan for new or changed plugins before listing them.
    bool rescan {false};
    /// How the plugin list is grouped and ordered.
    PluginListGrouping grouping {PluginListGrouping::name};
};

/// A shell completion request parsed from the command line, selected by the completion subcommand.
struct CompletionCommand {
    /// The shell to generate the script for.
    CompletionShell shell {CompletionShell::zsh};
    /// Write the script into the shell's per-user completion directory instead of printing it.
    bool install {false};
    /// Print where the installed script was written.
    bool verbose {false};
};

/// A rejected command line.
struct CliParseError {
    /// The rule the arguments broke.
    CliParseErrorKind kind;
    /// The explanation shown to the user.
    juce::String message;
};

/// The command a CLI invocation runs.
using CliCommand = std::variant<AnalyzeCommand, ListPluginsCommand, CompletionCommand>;

namespace cli
{
juce::String buildUsage(const juce::String& executableName, CliSubcommand subcommand = CliSubcommand::none);
juce::String joinArguments(const juce::ArgumentList& arguments);

[[nodiscard]] CliSubcommand findSubcommand(const juce::ArgumentList& arguments);

juce::StringArray findSkippedInputs(
    const juce::Array<juce::File>& inputPaths,
    const std::vector<AudioAnalysisRecord>& results
);

[[nodiscard]] std::expected<CliCommand, CliParseError> parseArguments(juce::ArgumentList arguments);

[[nodiscard]] int runAnalysis(const AnalyzeCommand& command);
}  // namespace cli
}  // namespace audiobatch
