/// Implementation of the CLI argument handling: the usage text,
/// and the parsing of the arguments into a validated AnalyzeCommand or ListPluginsCommand.
/// The names each enum option accepts come from one table per enum,
/// which both the parser and its error messages use.

#include "cli/AudioAnalysisCli.h"
#include "utils/StringFormat.h"
#include "utils/version.h"

#include <algorithm>
#include <array>
#include <expected>
#include <optional>
#include <utility>

namespace audiobatch
{
namespace
{
/// One name an enum option accepts on the command line, such as "csv" for --output.
template<typename Enum>
struct OptionName {
    const char* name;
    Enum value;
};

/// Lowest upper limit for the worker count, used on machines with few cores.
constexpr int minimumWorkerLimit {64};
/// Longest worker count text that is parsed, which keeps the number far from the 64-bit limit.
constexpr int maximumWorkerCountDigits {9};

/// Names accepted by --sort, with "loudness" as an alias for "lufs".
constexpr auto sortModeNames = std::to_array<OptionName<AudioAnalysisSortMode>>({
    {.name = "peak", .value = AudioAnalysisSortMode::peak},
    {.name = "lufs", .value = AudioAnalysisSortMode::loudness},
    {.name = "loudness", .value = AudioAnalysisSortMode::loudness},
    {.name = "name", .value = AudioAnalysisSortMode::name},
    {.name = "path", .value = AudioAnalysisSortMode::path},
});

/// Names accepted by --output.
constexpr auto outputFormatNames = std::to_array<OptionName<AudioAnalysisOutputFormat>>({
    {.name = "table", .value = AudioAnalysisOutputFormat::table},
    {.name = "csv", .value = AudioAnalysisOutputFormat::csv},
    {.name = "json", .value = AudioAnalysisOutputFormat::json},
});

/// Names accepted by --group-by.
constexpr auto pluginGroupingNames = std::to_array<OptionName<PluginListGrouping>>({
    {.name = "name", .value = PluginListGrouping::name},
    {.name = "manufacturer", .value = PluginListGrouping::manufacturer},
    {.name = "type", .value = PluginListGrouping::type},
    {.name = "format", .value = PluginListGrouping::format},
});

/// Options that only an analysis run takes, which the plugin listing rejects.
constexpr std::array analysisOptions {
    "--recurse|-r",
    "--refresh|-f",
    "--normalize|-n",
    "--force|-F",
    "--reverse",
    "--jobs|-j",
    "--sort|-s",
    "--output|-o",
    "--peak",
    "--lufs",
};

/// Returns true when the argument is a plain number such as "-14" or "0.5",
/// which may start with a minus sign but is a value and not an option.
bool isNumericArgument(const juce::String& text)
{
    const auto digits = text.startsWithChar('-') || text.startsWithChar('+') ? text.substring(1) : text;
    return digits.isNotEmpty() && digits.containsOnly("0123456789.") && digits.containsAnyOf("0123456789")
        && digits.indexOfChar('.') == digits.lastIndexOfChar('.');
}

/// Removes an option and its value from the arguments.
/// Returns nothing when the option is absent, and an empty string when it is present without a value,
/// which juce::ArgumentList::removeValueForOption does not tell apart.
std::optional<juce::String> takeOptionValue(juce::ArgumentList& arguments, const juce::StringRef option)
{
    if (!arguments.containsOption(option)) {
        return std::nullopt;
    }

    return arguments.removeValueForOption(option);
}

/// Returns the value whose name matches the text, ignoring case and surrounding space,
/// or nothing for an unknown name.
template<typename Enum, std::size_t count>
std::optional<Enum> parseName(const std::array<OptionName<Enum>, count>& names, const juce::String& text)
{
    const auto token = utils::normalizedToken(text);
    const auto match
        = std::ranges::find_if(names, [&token](const OptionName<Enum>& entry) { return token == entry.name; });

    return match == names.end() ? std::nullopt : std::optional(match->value);
}

/// Joins the accepted names for an error message, such as "table, csv, json".
template<typename Enum, std::size_t count>
juce::String joinNames(const std::array<OptionName<Enum>, count>& names)
{
    juce::StringArray texts;

    for (const auto& entry : names) {
        texts.add(entry.name);
    }

    return texts.joinIntoString(", ");
}

/// Removes an option that takes one of the given names, and returns the value for it.
/// Returns nothing when the option is absent, and an error naming the accepted values for an unknown name.
template<typename Enum, std::size_t count>
std::expected<std::optional<Enum>, juce::String> takeNamedOption(
    juce::ArgumentList& arguments,
    const juce::StringRef option,
    const juce::String& description,
    const std::array<OptionName<Enum>, count>& names
)
{
    const auto text = takeOptionValue(arguments, option);

    if (!text.has_value()) {
        return std::nullopt;
    }

    if (const auto value = parseName(names, *text); value.has_value()) {
        return value;
    }

    return std::unexpected(utils::format("{} must be one of: {}", description, joinNames(names)));
}

/// Returns an error message for the first argument that combines several short options, such as "-rf".
/// juce::ArgumentList matches a short option by searching for its letter anywhere in the argument,
/// and removes the whole argument on the first match, so "-rf" would silently set only one of the flags.
std::expected<void, juce::String> checkCombinedShortOptions(const juce::ArgumentList& arguments)
{
    for (const auto& argument : arguments.arguments) {
        if (argument.isShortOption() && argument.text.length() > 2) {
            return std::unexpected(
                utils::format(
                    "Combined short options are not supported: {}. Pass each option separately.", argument.text
                )
            );
        }
    }

    return {};
}

/// Parses the worker count of --jobs.
/// Returns the count, or an error message when the value is not a whole number in the supported range.
std::expected<int, juce::String> parseWorkerCount(const juce::String& value)
{
    // The thread pool creates every worker up front, so the count is capped well above any useful value.
    const auto maximumWorkerCount = juce::jmax(minimumWorkerLimit, juce::SystemStats::getNumCpus() * 4);
    const auto workerCountText = value.trim();

    if (workerCountText.isEmpty() || !workerCountText.containsOnly("0123456789")) {
        return std::unexpected(juce::String("Worker count must be a whole number greater than zero"));
    }

    // Parsed as a 64-bit value, so a very long number cannot overflow into a small one.
    const auto workerCount = workerCountText.getLargeIntValue();

    if (workerCount <= 0) {
        return std::unexpected(juce::String("Worker count must be greater than zero"));
    }

    if (workerCount > maximumWorkerCount || workerCountText.length() > maximumWorkerCountDigits) {
        return std::unexpected(utils::format("Worker count must be at most {}", maximumWorkerCount));
    }

    return static_cast<int>(workerCount);
}

/// Parses a target level in the given range for --peak or --lufs.
/// Returns the level, or an error message when the value is not a number in the range.
std::expected<double, juce::String> parseTargetLevel(
    const juce::String& option,
    const juce::String& value,
    const double minimum,
    const double maximum
)
{
    const auto trimmedValue = value.trim();

    if (!isNumericArgument(trimmedValue)) {
        return std::unexpected(utils::format("{} needs a number, for example {} {}", option, option, minimum));
    }

    const auto target = trimmedValue.getDoubleValue();

    if (target < minimum || target > maximum) {
        return std::unexpected(utils::format("{} must be between {} and {}", option, minimum, maximum));
    }

    return target;
}

/// Builds the normalization options of --normalize from the values of --peak and --lufs,
/// keeping the default 0 dBFS sample peak target when neither is given.
/// Returns an error message when both are given or a value is invalid.
std::expected<AudioNormalizationOptions, juce::String> parseNormalization(
    const std::optional<juce::String>& peakValue,
    const std::optional<juce::String>& loudnessValue
)
{
    AudioNormalizationOptions normalization;

    if (peakValue.has_value() && loudnessValue.has_value()) {
        return std::unexpected(juce::String("--peak and --lufs cannot be used together"));
    }

    if (peakValue.has_value()) {
        const auto target = parseTargetLevel(
            "--peak",
            *peakValue,
            AudioNormalizationOptions::minimumTargetPeakDbfs,
            AudioNormalizationOptions::maximumTargetPeakDbfs
        );

        if (!target) {
            return std::unexpected(target.error());
        }

        normalization.mode = AudioNormalizationMode::peak;
        normalization.targetPeakDbfs = *target;
    }

    if (loudnessValue.has_value()) {
        const auto target = parseTargetLevel(
            "--lufs",
            *loudnessValue,
            AudioNormalizationOptions::minimumTargetLufs,
            AudioNormalizationOptions::maximumTargetLufs
        );

        if (!target) {
            return std::unexpected(target.error());
        }

        normalization.mode = AudioNormalizationMode::loudness;
        normalization.targetLufs = *target;
    }

    return normalization;
}

/// Parses the arguments left after --list-plugins into a plugin listing,
/// which takes only its grouping and rescan options, without input paths or analysis options.
std::expected<CliCommand, juce::String> parseListPluginsCommand(juce::ArgumentList& arguments)
{
    ListPluginsCommand command;
    command.rescan = arguments.removeOptionIfFound("--rescan|-R");

    const auto grouping = takeNamedOption(arguments, "--group-by|-g", "Plugin grouping", pluginGroupingNames);
    if (!grouping) {
        return std::unexpected(grouping.error());
    }
    command.grouping = grouping->value_or(command.grouping);

    if (std::ranges::any_of(
            analysisOptions, [&arguments](const char* option) { return arguments.containsOption(option); }
        ))
    {
        return std::unexpected(juce::String("--list-plugins cannot be combined with analysis options"));
    }

    for (const auto& argument : arguments.arguments) {
        if (argument.isOption()) {
            return std::unexpected(utils::format("Unknown option: {}", argument.text));
        }
    }

    if (arguments.size() > 0) {
        return std::unexpected(juce::String("--list-plugins does not take input paths"));
    }

    return command;
}

/// Parses the arguments into an analysis run, with the remaining arguments as its input paths.
/// The plugin listing options are rejected, and --force, --peak, and --lufs need --normalize.
std::expected<CliCommand, juce::String> parseAnalyzeCommand(juce::ArgumentList& arguments)
{
    if (arguments.containsOption("--group-by|-g") || arguments.containsOption("--rescan|-R")) {
        return std::unexpected(juce::String("--group-by and --rescan can only be used with --list-plugins"));
    }

    AnalyzeCommand command;
    command.recursive = arguments.removeOptionIfFound("--recurse|-r");
    command.refresh = arguments.removeOptionIfFound("--refresh|-f");
    command.reverse = arguments.removeOptionIfFound("--reverse");
    const bool normalize = arguments.removeOptionIfFound("--normalize|-n");
    const bool force = arguments.removeOptionIfFound("--force|-F");

    if (const auto workerCountValue = takeOptionValue(arguments, "--jobs|-j"); workerCountValue.has_value()) {
        const auto workerCount = parseWorkerCount(*workerCountValue);

        if (!workerCount) {
            return std::unexpected(workerCount.error());
        }

        command.workerCount = *workerCount;
    }

    const auto sortMode = takeNamedOption(arguments, "--sort|-s", "Sort mode", sortModeNames);
    if (!sortMode) {
        return std::unexpected(sortMode.error());
    }
    command.sortMode = sortMode->value_or(command.sortMode);

    const auto outputFormat = takeNamedOption(arguments, "--output|-o", "Output format", outputFormatNames);
    if (!outputFormat) {
        return std::unexpected(outputFormat.error());
    }
    command.outputFormat = outputFormat->value_or(command.outputFormat);

    const auto peakValue = takeOptionValue(arguments, "--peak");
    const auto loudnessValue = takeOptionValue(arguments, "--lufs");

    if (force && !normalize) {
        return std::unexpected(juce::String("--force can only be used with --normalize"));
    }

    if ((peakValue.has_value() || loudnessValue.has_value()) && !normalize) {
        return std::unexpected(juce::String("--peak and --lufs can only be used with --normalize"));
    }

    if (normalize) {
        auto normalization = parseNormalization(peakValue, loudnessValue);

        if (!normalization) {
            return std::unexpected(normalization.error());
        }

        normalization->overwriteExisting = force;
        command.normalization = std::move(*normalization);
    }

    for (const auto& argument : arguments.arguments) {
        if (argument.isOption()) {
            return std::unexpected(utils::format("Unknown option: {}", argument.text));
        }

        // A negative number is a value, so one that no option took was most likely meant for the option before it.
        if (isNumericArgument(argument.text) && argument.text.startsWithChar('-')) {
            return std::unexpected(
                utils::format(
                    "Unexpected number {}. Pass a negative value right after the option that takes it.", argument.text
                )
            );
        }

        command.inputPaths.add(argument.resolveAsFile());
    }

    return command;
}
}  // namespace

/// Builds the help text shown by the CLI executable.
juce::String cli::buildUsage(const juce::String& executableName)
{
    // Lines are joined with juce::newLine outside the format strings,
    // so the text uses the platform line ending when printed.
    const juce::StringArray lines {
        utils::format("{} {}", executableName, version::VERSION_INFO),
        utils::format("Usage: {} [options] <paths...>", executableName),
        "",
        "Options:",
        "  -h, --help              Print usage and exit",
        "  -V, --version           Print version and exit",
        "  -r, --recurse           Recurse into directories",
        "  -f, --refresh           Ignore cached analysis and re-analyze files",
        "  -n, --normalize         Normalize analyzed files, then report the re-analyzed output peaks",
        "  -F, --force             Let --normalize replace an existing output file",
        "      --peak <dBFS>       Normalize to this sample peak, -12 to 0 (default 0)",
        "      --lufs <LUFS>       Normalize to this integrated loudness, -24 to -6, with a -0.3 dBTP ceiling",
        "  -j, --jobs <count>      Override worker count",
        "  -s, --sort <mode>       Sort by peak, lufs, name, or path",
        "      --reverse           Sort in descending order",
        "  -o, --output <format>   Print results as table (default), csv, or json",
        "  -l, --list-plugins      List the available audio plugins and exit",
        "  -g, --group-by <mode>   Group the plugin list by name (alphabetical), manufacturer, type, or format",
        "  -R, --rescan            Scan for new or changed plugins before listing them",
        "",
        "Short options cannot be combined, so pass -r -f instead of -rf.",
    };

    return lines.joinIntoString(juce::newLine) + juce::newLine;
}

/// Joins the decoded arguments, without the executable name, into one space separated command line.
/// Used for the plugin scan worker command line,
/// so it carries the same decoded text as the arguments instead of decoding argv a second time.
juce::String cli::joinArguments(const juce::ArgumentList& arguments)
{
    juce::StringArray texts;

    for (const auto& argument : arguments.arguments) {
        texts.add(argument.text);
    }

    return texts.joinIntoString(" ");
}

/// Parses command-line arguments into a validated analysis or plugin listing command,
/// or returns the error to show the user.
/// The help and version flags are handled by the caller before parsing.
std::expected<CliCommand, juce::String> cli::parseArguments(juce::ArgumentList arguments)
{
    if (auto combined = checkCombinedShortOptions(arguments); !combined) {
        return std::unexpected(std::move(combined).error());
    }

    if (arguments.removeOptionIfFound("--list-plugins|-l")) {
        return parseListPluginsCommand(arguments);
    }

    return parseAnalyzeCommand(arguments);
}
}  // namespace audiobatch
