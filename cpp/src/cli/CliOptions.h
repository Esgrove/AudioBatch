/// The command line options and subcommands of the CLI executable, as constant tables.
/// The parser matches the options through these tables, the usage texts list them,
/// and the shell completion scripts are generated from them, so the three cannot drift apart.
/// The names each enum option accepts are tables too, shared by the parser, its error messages, and the completions.

#pragma once

#include "cli/AudioAnalysisCli.h"
#include "utils/StringFormat.h"
#include <string_view>
#include <type_traits>

#include <array>
#include <cstddef>
#include <functional>
#include <span>

namespace audiobatch
{
/// One name an enum option accepts on the command line, such as "csv" for --output.
template<typename Enum>
    requires std::is_enum_v<Enum>
struct OptionName {
    /// The name as it is typed on the command line.
    const char* name {nullptr};
    /// The enum value the name selects.
    Enum value {};
};

/// One command line option, which the parser matches and the usage text and the shell completions describe.
struct CliOption {
    /// Returns true when the option takes a value, such as the count of --jobs.
    [[nodiscard]] constexpr bool takesValue() const
    {
        return !std::string_view(valueName).empty();
    }

    /// Returns true when the option has a short form besides the long one.
    [[nodiscard]] constexpr bool hasShortName() const
    {
        return !std::string_view(shortName).empty();
    }

    /// Returns the pattern juce::ArgumentList matches the option with, such as "--recurse|-r".
    [[nodiscard]] juce::String pattern() const
    {
        return hasShortName() ? utils::format("{}|{}", longName, shortName) : juce::String(longName);
    }

    /// Long form, such as "--recurse".
    const char* longName {""};
    /// Short form, such as "-r", or empty for an option that only has the long form.
    const char* shortName {""};
    /// Placeholder for the value in the usage text, such as "count", or empty for an option without a value.
    const char* valueName {""};
    /// One line description for the usage text and the shell completions.
    const char* description {""};
    /// Values the shell completions offer for the option, empty when the value is free text.
    std::span<const char* const> values;
};

/// The options of one command, in the order the usage text lists them.
using CliOptionList = std::span<const std::reference_wrapper<const CliOption>>;

/// A subcommand, which runs instead of the analysis when it is the first argument.
struct CliSubcommandInfo {
    /// The subcommand the entry describes.
    CliSubcommand subcommand {CliSubcommand::none};
    /// The word that selects the subcommand, such as "list".
    const char* name {""};
    /// One line description for the usage texts and the shell completions.
    const char* description {""};
    /// The options the subcommand takes, besides --help.
    CliOptionList options;
    /// Placeholder of the one argument the subcommand takes, such as "shell", or empty when it takes none.
    const char* argumentName {""};
    /// Description of that argument for the usage text, which lists the accepted values after it.
    const char* argumentDescription {""};
    /// Whether the argument must be given, which the usage text shows as "<name>" instead of "[name]".
    bool argumentRequired {true};
    /// Values the shell completions offer for that argument.
    std::span<const char* const> argumentValues;
};

namespace cli
{
/// Returns the names of an enum option table, for the shell completions to offer.
template<typename Enum, std::size_t count>
    requires std::is_enum_v<Enum>
consteval std::array<const char*, count> namesOf(const std::array<OptionName<Enum>, count>& names)
{
    std::array<const char*, count> result {};

    for (std::size_t index = 0; index < count; ++index) {
        result.at(index) = names.at(index).name;
    }

    return result;
}

/// Names accepted by --sort, with "loudness" as an alias for "lufs".
inline constexpr auto sortModeNames {
    std::to_array<OptionName<AudioAnalysisSortMode>>({
        {.name = "peak", .value = AudioAnalysisSortMode::peak},
        {.name = "lufs", .value = AudioAnalysisSortMode::loudness},
        {.name = "loudness", .value = AudioAnalysisSortMode::loudness},
        {.name = "name", .value = AudioAnalysisSortMode::name},
        {.name = "path", .value = AudioAnalysisSortMode::path},
    }),
};

/// Names accepted by --output.
inline constexpr auto outputFormatNames {
    std::to_array<OptionName<AudioAnalysisOutputFormat>>({
        {.name = "table", .value = AudioAnalysisOutputFormat::table},
        {.name = "csv", .value = AudioAnalysisOutputFormat::csv},
        {.name = "json", .value = AudioAnalysisOutputFormat::json},
    }),
};

/// Names accepted by --group-by.
inline constexpr auto pluginGroupingNames {
    std::to_array<OptionName<PluginListGrouping>>({
        {.name = "name", .value = PluginListGrouping::name},
        {.name = "manufacturer", .value = PluginListGrouping::manufacturer},
        {.name = "type", .value = PluginListGrouping::type},
        {.name = "format", .value = PluginListGrouping::format},
    }),
};

/// Shell names accepted by the completion subcommand.
inline constexpr auto completionShellNames {
    std::to_array<OptionName<CompletionShell>>({
        {.name = "zsh", .value = CompletionShell::zsh},
        {.name = "bash", .value = CompletionShell::bash},
        {.name = "fish", .value = CompletionShell::fish},
        {.name = "powershell", .value = CompletionShell::powershell},
    }),
};

/// The names of the enum option tables, which the options below offer as completions.
inline constexpr auto sortModeValues {namesOf(sortModeNames)};
inline constexpr auto outputFormatValues {namesOf(outputFormatNames)};
inline constexpr auto pluginGroupingValues {namesOf(pluginGroupingNames)};
inline constexpr auto completionShellValues {namesOf(completionShellNames)};

inline constexpr CliOption helpOption {
    .longName = "--help",
    .shortName = "-h",
    .valueName = "",
    .description = "Print usage and exit",
    .values = {},
};
inline constexpr CliOption versionOption {
    .longName = "--version",
    .shortName = "-V",
    .valueName = "",
    .description = "Print version and exit",
    .values = {},
};

inline constexpr CliOption recurseOption {
    .longName = "--recurse",
    .shortName = "-r",
    .valueName = "",
    .description = "Recurse into directories",
    .values = {},
};
inline constexpr CliOption refreshOption {
    .longName = "--refresh",
    .shortName = "-f",
    .valueName = "",
    .description = "Ignore cached analysis and re-analyze files",
    .values = {},
};
inline constexpr CliOption normalizeOption {
    .longName = "--normalize",
    .shortName = "-n",
    .valueName = "",
    .description = "Normalize analyzed files, then report the re-analyzed output peaks",
    .values = {},
};
inline constexpr CliOption forceOption {
    .longName = "--force",
    .shortName = "-F",
    .valueName = "",
    .description = "Let normalize replace an existing output file",
    .values = {},
};
inline constexpr CliOption peakOption {
    .longName = "--peak",
    .shortName = "",
    .valueName = "dBFS",
    .description = "Normalize to this sample peak, -12 to 0 (default 0)",
    .values = {},
};
inline constexpr CliOption lufsOption {
    .longName = "--lufs",
    .shortName = "",
    .valueName = "LUFS",
    .description = "Normalize to this integrated loudness, -24 to -6, with a -0.3 dBTP ceiling",
    .values = {},
};
inline constexpr CliOption jobsOption {
    .longName = "--jobs",
    .shortName = "-j",
    .valueName = "count",
    .description = "Override worker count",
    .values = {},
};
inline constexpr CliOption sortOption {
    .longName = "--sort",
    .shortName = "-s",
    .valueName = "mode",
    .description = "Sort by peak, lufs, name, or path",
    .values = sortModeValues,
};
inline constexpr CliOption reverseOption {
    .longName = "--reverse",
    .shortName = "-S",
    .valueName = "",
    .description = "Sort in descending order",
    .values = {},
};
inline constexpr CliOption outputOption {
    .longName = "--output",
    .shortName = "-o",
    .valueName = "format",
    .description = "Print results as table (default), csv, or json",
    .values = outputFormatValues,
};

inline constexpr CliOption groupByOption {
    .longName = "--group-by",
    .shortName = "-g",
    .valueName = "mode",
    .description = "Group the plugin list by name (alphabetical), manufacturer, type, or format",
    .values = pluginGroupingValues,
};
inline constexpr CliOption rescanOption {
    .longName = "--rescan",
    .shortName = "-R",
    .valueName = "",
    .description = "Scan for new or changed plugins before listing them",
    .values = {},
};

inline constexpr CliOption installOption {
    .longName = "--install",
    .shortName = "-I",
    .valueName = "",
    .description = "Install the script to the shell's completion directory",
    .values = {},
};
inline constexpr CliOption verboseOption {
    .longName = "--verbose",
    .shortName = "-v",
    .valueName = "",
    .description = "Print where the script was installed",
    .values = {},
};

/// The options of an analysis run, besides --help and --version.
inline constexpr std::array analysisOptions {
    std::cref(recurseOption),
    std::cref(refreshOption),
    std::cref(normalizeOption),
    std::cref(forceOption),
    std::cref(peakOption),
    std::cref(lufsOption),
    std::cref(jobsOption),
    std::cref(sortOption),
    std::cref(reverseOption),
    std::cref(outputOption),
};

/// The options of the list subcommand.
inline constexpr std::array listOptions {std::cref(groupByOption), std::cref(rescanOption)};

/// The options of the completion subcommand.
inline constexpr std::array completionOptions {std::cref(installOption), std::cref(verboseOption)};

/// The commands the help subcommand describes.
inline constexpr auto helpCommandValues {std::to_array<const char*>({"list", "completion", "help"})};

/// The subcommands, in the order the usage text and the shell completions list them.
inline constexpr auto subcommands {
    std::to_array<CliSubcommandInfo>({
        {
            .subcommand = CliSubcommand::list,
            .name = "list",
            .description = "List the available audio plugins",
            .options = listOptions,
            .argumentName = "",
            .argumentDescription = "",
            .argumentRequired = false,
            .argumentValues = {},
        },
        {
            .subcommand = CliSubcommand::completion,
            .name = "completion",
            .description = "Print or install a shell completion script",
            .options = completionOptions,
            .argumentName = "shell",
            .argumentDescription = "Shell to generate the script for",
            .argumentRequired = true,
            .argumentValues = completionShellValues,
        },
        {
            .subcommand = CliSubcommand::help,
            .name = "help",
            .description = "Print this message or the help of the given command",
            .options = {},
            .argumentName = "command",
            .argumentDescription = "Command to print the help of",
            .argumentRequired = false,
            .argumentValues = helpCommandValues,
        },
    }),
};
}  // namespace cli
}  // namespace audiobatch
